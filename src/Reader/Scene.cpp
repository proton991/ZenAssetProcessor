#include "ZenAsset/Reader/Scene.h"
#include "Diagnostics/StageMeasurements.h"
#include "Format/SceneValidation.h"
#include "Format/SceneArchive.h"
#include "Format/SceneResources.h"
#include "Format/PackageLayout.h"
#include "Format/MeshEncoding.h"
#include <limits>
#include "Format/WorkerPool.h"
#include <atomic>
#include <type_traits>
#include <bit>

namespace zenasset
{
namespace
{
Status ValidateReadLimits(const Package&         package,
                          const SceneReadLimits& limits,
                          uint64_t               objectBytes,
                          uint64_t&              allocationBytes)
{
    Status       status;
    const uint64_t directoryBytes = package.DirectoryBytes();
    if (!package.FitsReadLimits(limits.package) || directoryBytes > UINT64_MAX - objectBytes)
    {
        status = {StatusCode::ResourceLimit, "Retained package exceeds scene read limits"};
    }
    else
    {
        allocationBytes = objectBytes + directoryBytes;
        if (allocationBytes > limits.maxAllocationBytes)
        {
            status = {StatusCode::ResourceLimit, "Scene and directory exceed the decoded allocation budget"};
        }
    }
    return status;
}

class PackageImageSource final : public ImagePayloadSource
{
public:
    PackageImageSource(std::shared_ptr<const Package> package, uint32_t index, std::shared_ptr<const SceneReadLimits> limits) :
        m_package(std::move(package)), m_index(index), m_limits(std::move(limits))
    {}
    Status Read(std::shared_ptr<const ImagePixels>& pixels, const Cancellation* cancellation) const override
    {
        return ReadBounded(pixels, cancellation, m_limits->maxAllocationBytes);
    }
    Status ReadBounded(std::shared_ptr<const ImagePixels>& pixels,
                       const Cancellation*                 cancellation,
                       uint64_t                            maxBytes) const override
    {
        SceneReadLimits limits      = *m_limits;
        limits.maxAllocationBytes   = std::min(limits.maxAllocationBytes, maxBytes);
        SceneImageReadResult result = ReadSceneImage(*m_package, m_index, limits, cancellation);
        pixels                      = result.status.Succeeded() ? std::move(result.image.pixels) : nullptr;
        return result.status;
    }

private:
    std::shared_ptr<const Package>         m_package;
    uint32_t                               m_index;
    std::shared_ptr<const SceneReadLimits> m_limits;
};

class ReadTransfers
{
public:
    ReadTransfers(const Package& package, SceneReadProgress progress) : m_progress(progress)
    {
        if (m_progress.transfer != nullptr)
        {
            for (const ResourceRecord& record : package.Resources())
            {
                if (record.flags == RequiredResource)
                {
                    m_state.totalBytes  += record.storedBytes;
                    m_geometry          += detail::ResourcePart(record.type) == detail::PartKind::Geometry ? 1 : 0;
                    m_state.totalImages += record.type == ResourceType::Image ? 1 : 0;
                }
            }
        }
    }
    static void Bytes(void* context, uint64_t bytes)
    {
        ReadTransfers&              self = *static_cast<ReadTransfers*>(context);
        std::lock_guard<std::mutex> lock(self.m_mutex);
        self.m_state.completedBytes += bytes;
        if (self.m_state.completedBytes - self.m_reported >= (1u << 20))
        {
            self.Publish();
        }
    }
    void Completed(ResourceType type)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (type == ResourceType::Image)
        {
            ++m_state.completedImages;
        }
        if (detail::ResourcePart(type) == detail::PartKind::Geometry)
        {
            m_state.geometryComplete = --m_geometry == 0;
        }
        Publish();
    }

private:
    void Publish()
    {
        m_reported = m_state.completedBytes;
        m_progress.transfer(m_progress.user, m_state);
    }
    SceneReadProgress  m_progress;
    SceneTransferState m_state;
    uint64_t           m_geometry{0};
    uint64_t           m_reported{0};
    std::mutex         m_mutex;
};
class SceneDecoder
{
public:
    SceneDecoder(const Package&                 package,
                 const SceneReadLimits&         limits,
                 const Cancellation*            cancellation,
                 std::shared_ptr<const Package> deferredPackage = {},
                 SceneReadProgress              progress        = {},
                 ReadTransfers*                 transfers       = nullptr) :
        m_package(package),
        m_limits(limits),
        m_cancellation(cancellation),
        m_budget(limits.maxAllocationBytes),
        m_validationGeometryBudget(limits.maxRuntimeGeometryBytes),
        m_deferredPackage(std::move(deferredPackage)),
        m_progress(progress),
        m_transfers(transfers)
    {}

    template <class T> void Decode(ResourceType type, uint32_t index, T& value)
    {
        ZENASSET_MEASURE_SCOPE(Deserialize);
        const ResourceRecord* record = m_package.Find(detail::ResourceId(type, index));
        if (m_status.Succeeded() && record == nullptr)
        {
            m_status = {StatusCode::InvalidData, "Required scene resource is missing"};
        }
        if (m_status.Succeeded()
            && (record->type != type || !detail::SupportedResourceVersion(type, record->version)
                || !detail::SupportedEncoding(type, record->encoding) || record->flags != RequiredResource))
        {
            m_status = {StatusCode::InvalidData, "Required scene resource identity or schema is invalid"};
        }
        if (m_status.Succeeded() && (record->decodedBytes > m_budget || record->decodedBytes > m_limits.package.maxChunkBytes))
        {
            m_status = {StatusCode::ResourceLimit, "Scene resource buffer exceeds the decoded allocation budget"};
        }
        if (m_status.Succeeded())
        {
            ResourceData data = m_package.Read(record->id, m_cancellation,
                                               std::min(m_budget - record->decodedBytes, m_limits.package.maxCodecBytes),
                                               {m_transfers, m_transfers ? &ReadTransfers::Bytes : nullptr});
            m_status          = data.status;
            if (m_status.Succeeded())
            {
                const uint64_t initialBudget  = m_budget;
                m_budget                     -= record->decodedBytes;
                ResourceData               expanded;
                std::span<const std::byte> canonical = data.bytes;
                bool                       typedMesh = false;
                if constexpr (std::endian::native == std::endian::little
                              && (std::is_same_v<T, std::vector<Vertex>> || std::is_same_v<T, std::vector<uint32_t>>))
                {
                    if (record->encoding != 0)
                    {
                        SceneReadLimits meshLimits          = m_limits;
                        meshLimits.maxRuntimeGeometryBytes -= m_geometryBytes;
                        m_status =
                            detail::DecodeMeshResourceTo(*record, data.bytes, meshLimits, m_budget, m_cancellation, value);
                        typedMesh = true;
                        if (m_status.Succeeded())
                        {
                            const uint64_t bytes  = value.size() * sizeof(typename T::value_type);
                            m_geometryBytes      += bytes;
                            m_budget             -= bytes;
                        }
                    }
                }
                if (record->encoding != 0 && !typedMesh)
                {
                    expanded = detail::DecodeMeshResource(*record, data.bytes, m_limits, m_budget, m_cancellation);
                    m_status = expanded.status;
                    if (m_status.Succeeded())
                    {
                        canonical  = expanded.bytes;
                        m_budget  -= expanded.bytes.size();
                    }
                }
                if (m_status.Succeeded() && !typedMesh
                    && (type == ResourceType::VertexStream || type == ResourceType::IndexStream))
                {
                    if (canonical.size() < 8 || canonical.size() - 8 > m_limits.maxRuntimeGeometryBytes - m_geometryBytes)
                    {
                        m_status = {StatusCode::ResourceLimit, "Canonical mesh streams exceed the runtime geometry budget"};
                    }
                    else
                    {
                        m_geometryBytes += canonical.size() - 8;
                    }
                }
                const SceneReadLimits limits = detail::ResourceLimits(type, m_limits);
                if (m_status.Succeeded() && !typedMesh)
                {
                    detail::SceneArchive<true> archive(canonical, nullptr, limits, m_budget, m_cancellation, record->version);
                    archive(value);
                    m_status = archive.Finish();
                }
                m_budget += record->decodedBytes + expanded.bytes.size();
                if constexpr (std::is_same_v<T, ImageAsset>)
                {
                    // The decoded chunk coexists first with codec scratch, then
                    // with the deserialized pixels. Leases use this same decoder.
                    const uint64_t peak = record->decodedBytes + std::max(data.scratchBytes, initialBudget - m_budget);
                    m_largestImageLease = std::max(m_largestImageLease, peak);
                    if (m_status.Succeeded() && value.pixels)
                    {
                        std::const_pointer_cast<ImagePixels>(value.pixels)->sourceImageChecksum = record->sourceImageChecksum;
                    }
                }
                if (m_status.Succeeded() && m_transfers)
                {
                    m_transfers->Completed(type);
                }
                if (m_status.Succeeded() && m_totalResources != 0)
                {
                    ReportDecoded();
                }
            }
        }
        if (!m_status.Succeeded() && record)
        {
            const std::string_view path = m_package.PartPath(record->id);
            if (!path.empty() && m_status.message.find(path) == std::string::npos)
            {
                m_status.message.append(" in ").append(path);
            }
        }
    }

    Status ReadManifest(Scene& scene)
    {
        detail::SceneManifest manifest;
        Decode(ResourceType::Scene, 0, manifest);
        if (m_status.Succeeded())
        {
            m_counts = manifest.counts;
            detail::ResourceCounts found{};
            for (const ResourceRecord& record : m_package.Resources())
            {
                const uint32_t type = static_cast<uint32_t>(record.type);
                if ((record.flags & RequiredResource) == 0)
                {
                    // Optional directory entries do not contribute to the
                    // manifest's required runtime resource graph.
                }
                else if (type > 0 && type < detail::ResourceTypeCount)
                {
                    const uint64_t index = record.id & UINT32_MAX;
                    if ((record.id >> 32) != type || index == 0 || index > m_counts[type]
                        || !detail::SupportedResourceVersion(record.type, record.version)
                        || !detail::SupportedEncoding(record.type, record.encoding) || record.flags != RequiredResource)
                    {
                        m_status = {StatusCode::InvalidData,
                                    "Scene directory identity, schema, encoding or required flag is inconsistent"};
                    }
                    ++found[type];
                }
                else if ((record.flags & RequiredResource) != 0)
                {
                    m_status = {StatusCode::UnsupportedVersion, "Unknown required scene resource"};
                }
            }
            for (size_t type = 0; type < detail::ResourceTypeCount; ++type)
            {
                if (m_counts[type] != found[type] || m_counts[type] > UINT32_MAX || m_counts[type] > m_limits.maxArrayElements)
                {
                    m_status = {StatusCode::InvalidData, "Scene manifest resource counts disagree with the directory"};
                }
            }
            for (ResourceType single :
                 {ResourceType::Scene, ResourceType::Metadata, ResourceType::VertexStream, ResourceType::IndexStream,
                  ResourceType::MaterialVariants, ResourceType::TextureCoordinates, ResourceType::SourceDocument})
            {
                if (m_counts[static_cast<size_t>(single)] != 1)
                {
                    m_status = {StatusCode::InvalidData,
                                "Scene package requires exactly one manifest, provenance and bulk stream"};
                }
            }
            if (m_status.Succeeded())
            {
                for (uint64_t count : found)
                {
                    m_totalResources += count;
                }
                ReportDecoded();
                scene.name               = std::move(manifest.name);
                scene.roots              = std::move(manifest.roots);
                scene.vertexTexCoordSets = std::move(manifest.texCoordSets);
                scene.imageBasedLight    = manifest.imageBasedLight;
            }
        }
        return m_status;
    }

    template <class T> void Array(ResourceType type, std::vector<T>& values)
    {
        const uint64_t count = m_counts[static_cast<size_t>(type)];
        bool           scheduled = false;
        if constexpr (std::is_same_v<T, ImageAsset>)
        {
            scheduled = !m_jobs.empty();
            if (scheduled)
            {
                JoinImages(values);
            }
        }
        if (!scheduled && m_status.Succeeded() && (count > m_budget / sizeof(T) || count > SIZE_MAX / sizeof(T)))
        {
            m_status = {StatusCode::ResourceLimit, "Scene resource records exceed the allocation budget"};
        }
        if (!scheduled && m_status.Succeeded())
        {
            m_budget -= count * sizeof(T);
            values.resize(static_cast<size_t>(count));
            for (uint32_t index = 0; m_status.Succeeded() && index < count; ++index)
            {
                Decode(type, index, values[index]);
                if constexpr (std::is_same_v<T, ImageAsset>)
                {
                    if (m_status.Succeeded() && m_deferredPackage)
                    {
                        DeferImage(index, values[index]);
                    }
                }
            }
        }
    }
    template <class T> void Single(ResourceType type, T& value)
    {
        Decode(type, 0, value);
    }
    void Coordinates(Scene& scene)
    {
        const ResourceRecord* record = m_package.Find(detail::ResourceId(ResourceType::TextureCoordinates));
        if (record != nullptr && record->version == 2)
        {
            uint64_t rows = 0;
            Decode(ResourceType::TextureCoordinates, 0, rows);
            const uint64_t stride = sizeof(VertexTextureCoordinates) + 2 * sizeof(Vec2);
            if (m_status.Succeeded()
                && (rows == 0 || rows != scene.vertices.size() || scene.vertexTexCoordSets.size() != 2
                    || !scene.morphPrimitives.empty() || !scene.skins.empty()))
            {
                m_status = {StatusCode::InvalidData, "Implicit UV rows do not match the scene layout"};
            }
            if (m_status.Succeeded() && m_deferredPackage)
            {
                // Validation charges three runtime copies of the UVs. Account for
                // the implicit rows just as it would for a version-one resource.
                constexpr uint64_t bytesPerVertex = 3 * 2 * sizeof(Vec2);
                if (rows > m_validationGeometryBudget / bytesPerVertex)
                {
                    m_status = {StatusCode::ResourceLimit, "Implicit UV rows exceed the runtime geometry budget"};
                }
                else
                {
                    m_validationGeometryBudget -= rows * bytesPerVertex;
                }
            }
            else if (m_status.Succeeded() && rows > std::min<uint64_t>(m_budget, SIZE_MAX) / stride)
            {
                m_status = {StatusCode::ResourceLimit, "Implicit UV rows exceed the allocation budget"};
            }
            else if (m_status.Succeeded())
            {
                m_budget -= rows * stride;
                scene.vertexTexCoords.resize(static_cast<size_t>(rows));
                for (size_t index = 0; index < rows; ++index)
                {
                    if ((index & 4095u) == 0 && m_cancellation != nullptr && m_cancellation->IsCancelled())
                    {
                        m_status = {StatusCode::Cancelled, "Scene read cancelled"};
                        break;
                    }
                    scene.vertexTexCoords[index] = {scene.vertices[index].uv0, scene.vertices[index].uv1};
                }
            }
        }
        else
        {
            Single(ResourceType::TextureCoordinates, scene.vertexTexCoords);
        }
    }
    void Metadata(Scene& scene)
    {
        detail::SceneMetadata metadata;
        Decode(ResourceType::Metadata, 0, metadata);
        if (m_status.Succeeded())
        {
            scene.sourcePath   = std::move(metadata.sourcePath);
            scene.dependencies = std::move(metadata.dependencies);
            m_metadata         = std::move(metadata.build);
        }
    }
    Status ResultStatus() const
    {
        return m_status;
    }
    Status Validate(const Scene& scene)
    {
        ZENASSET_MEASURE_SCOPE(SceneValidate);
        if (m_imageLimits)
        {
            // All retained graph allocations are now known. Each later lease
            // must fit beside that graph, including its codec and decode scratch.
            m_imageLimits->maxAllocationBytes = m_budget;
            const uint64_t overhead           = sizeof(ImageAsset) + m_package.DirectoryBytes();
            if (overhead > m_budget || m_largestImageLease > m_budget - overhead)
            {
                m_status = {StatusCode::ResourceLimit, "Deferred image lease exceeds the remaining allocation budget"};
            }
        }
        if (m_status.Succeeded() && !detail::FitsValidationBudget(scene, m_budget))
        {
            m_status = {StatusCode::ResourceLimit, "Scene validation scratch exceeds the decoded allocation budget"};
        }
        std::string failure;
        if (m_status.Succeeded())
        {
            const bool valid = m_deferredPackage ? detail::ValidateSceneGraph(scene, m_validationGeometryBudget, &failure)
                                                 : ValidateSceneAsset(scene, m_validationGeometryBudget, &failure);
            if (!valid)
            {
                m_status = {StatusCode::InvalidData, "Invalid scene resource graph: " + failure};
            }
        }
        return m_status;
    }
    BuildMetadata TakeMetadata()
    {
        return std::move(m_metadata);
    }

private:
    struct ImageWork
    {
        Status   status;
        uint64_t retained{0};
        uint64_t peakLease{0};
    };
    ImageWork ReadImages(std::vector<ImageAsset>& values, std::atomic<size_t>& next, uint64_t budget)
    {
        SceneReadLimits limits    = m_limits;
        limits.maxAllocationBytes = budget;
        SceneDecoder worker(m_package, limits, m_cancellation, m_deferredPackage, {}, m_transfers);
        worker.m_imageLimits = m_imageLimits;
        size_t index         = next.fetch_add(1);
        while (worker.m_status.Succeeded() && index < values.size())
        {
            worker.Decode(ResourceType::Image, static_cast<uint32_t>(index), values[index]);
            if (worker.m_status.Succeeded() && worker.m_deferredPackage)
            {
                worker.DeferImage(static_cast<uint32_t>(index), values[index]);
            }
            index = next.fetch_add(1);
        }
        return {std::move(worker.m_status), budget - worker.m_budget, worker.m_largestImageLease};
    }

public:
    void StartImages(std::vector<ImageAsset>& values)
    {
        const uint64_t count = m_counts[static_cast<size_t>(ResourceType::Image)];
        const size_t   slots =
            static_cast<size_t>(std::min<uint64_t>({m_limits.workerCount, std::thread::hardware_concurrency(), count + 1}));
        if (slots > 1 && !m_package.Parts().empty() && count && count <= m_budget / sizeof(ImageAsset)
            && m_budget - count * sizeof(ImageAsset) >= sizeof(SceneReadLimits))
        {
            values.resize(static_cast<size_t>(count));
            m_budget -= count * sizeof(ImageAsset);
            if (m_deferredPackage)
            {
                m_budget      -= sizeof(SceneReadLimits);
                m_imageLimits  = std::make_shared<SceneReadLimits>(m_limits);
            }
            const uint64_t share  = m_budget / slots;
            m_imageReservation    = share * (slots - 1);
            m_budget             -= m_imageReservation;
            m_pool                = std::make_unique<WorkerPool>(static_cast<uint32_t>(slots - 1));
            for (size_t index = 0; index < m_pool->GetSize(); ++index)
            {
                m_jobs.emplace_back(
                    m_pool->Push(&SceneDecoder::ReadImages, this, std::ref(values), std::ref(m_nextImage), share));
            }
        }
    }

private:
    void JoinImages(std::vector<ImageAsset>& values)
    {
        bool     retry    = false;
        uint64_t retained = 0;
        for (std::future<ImageWork>& job : m_jobs)
        {
            const ImageWork result  = job.get();
            retry                  |= result.status.code == StatusCode::ResourceLimit;
            if (m_status.Succeeded() && !result.status.Succeeded() && result.status.code != StatusCode::ResourceLimit)
            {
                m_status = result.status;
            }
            retained            += result.retained;
            m_largestImageLease  = std::max(m_largestImageLease, result.peakLease);
        }
        m_pool.reset();
        m_budget += m_imageReservation;
        if (retry && m_status.Succeeded())
        {
            m_status = {StatusCode::ResourceLimit, "Concurrent image reservations exhausted"};
        }
        else if (m_status.Succeeded())
        {
            m_budget -= retained;
            for (size_t index = 0; index < values.size(); ++index)
            {
                ReportDecoded();
            }
        }
    }
    void ReportDecoded()
    {
        if (m_progress.resourceDecoded != nullptr)
        {
            m_progress.resourceDecoded(m_progress.user, ++m_completedResources, m_totalResources);
        }
    }
    void DeferImage(uint32_t index, ImageAsset& image)
    {
        if (!m_imageLimits && m_budget >= sizeof(SceneReadLimits))
        {
            m_budget      -= sizeof(SceneReadLimits);
            m_imageLimits  = std::make_shared<SceneReadLimits>(m_limits);
        }
        if (!ValidateImagePixels(image.pixels))
        {
            m_status = {StatusCode::InvalidData,
                        "Invalid image payload "
                            + std::string(m_package.PartPath(detail::ResourceId(ResourceType::Image, index)))};
        }
        else if (!m_imageLimits
                 || m_budget < sizeof(PackageImageSource) + sizeof(ImagePixels)
                                   + image.pixels->mips.size() * sizeof(std::vector<uint8_t>))
        {
            m_status = {StatusCode::ResourceLimit, "Deferred image descriptor exceeds allocation budget"};
        }
        else
        {
            const ImagePixels&           resident   = *image.pixels;
            std::shared_ptr<ImagePixels> descriptor = std::make_shared<ImagePixels>();
            descriptor->sourceImageChecksum         = resident.sourceImageChecksum;
            descriptor->width                       = resident.width;
            descriptor->height                      = resident.height;
            descriptor->format                      = resident.format;
            descriptor->arrayLayers                 = resident.arrayLayers;
            descriptor->mips.resize(resident.mips.size());
            uint64_t released = resident.bytes.size();
            for (const std::vector<uint8_t>& mip : resident.mips)
            {
                released += mip.size();
            }
            m_budget                  += released;
            m_budget                  -= sizeof(PackageImageSource);
            descriptor->payloadSource  = std::make_shared<PackageImageSource>(m_deferredPackage, index, m_imageLimits);
            image.pixels               = std::move(descriptor);
        }
    }

    const Package&                   m_package;
    const SceneReadLimits&           m_limits;
    const Cancellation*              m_cancellation;
    uint64_t                         m_budget;
    uint64_t                         m_validationGeometryBudget;
    uint64_t                         m_geometryBytes{0};
    Status                           m_status;
    detail::ResourceCounts           m_counts{};
    BuildMetadata                    m_metadata;
    std::shared_ptr<const Package>   m_deferredPackage;
    std::shared_ptr<SceneReadLimits> m_imageLimits;
    uint64_t                         m_largestImageLease{0};
    SceneReadProgress                m_progress;
    ReadTransfers*                      m_transfers{nullptr};
    uint64_t                         m_completedResources{0};
    uint64_t                         m_totalResources{0};
    uint64_t                            m_imageReservation{0};
    std::atomic<size_t>                 m_nextImage{0};
    std::vector<std::future<ImageWork>> m_jobs;
    std::unique_ptr<WorkerPool>         m_pool;
};
SceneReadResult ReadSceneAttempt(const Package&                 package,
                                 const SceneReadLimits&         limits,
                                 const Cancellation*            cancellation,
                                 std::shared_ptr<const Package> deferredPackage,
                                 SceneReadProgress              progress)
{
    SceneReadResult result;
    const bool      runtimeStorage  = bool(deferredPackage);
    uint64_t        allocationBytes = 0;
    result.status                   = ValidateReadLimits(package, limits, sizeof(Scene), allocationBytes);
    if (cancellation != nullptr && cancellation->IsCancelled())
    {
        result.status = {StatusCode::Cancelled, "Scene read cancelled"};
    }
    else if (result.status.Succeeded())
    {
        SceneReadLimits charged       = limits;
        charged.maxAllocationBytes   -= allocationBytes;
        std::shared_ptr<Scene> scene  = std::make_shared<Scene>();
        ReadTransfers          transfers(package, progress);
        SceneDecoder           decoder(package, charged, cancellation, std::move(deferredPackage), progress,
                                       progress.transfer ? &transfers : nullptr);
        result.status = decoder.ReadManifest(*scene);
        if (result.status.Succeeded())
        {
            decoder.StartImages(scene->images);
            detail::VisitSceneResources(decoder, *scene);
            result.status = decoder.ResultStatus();
        }
        if (result.status.Succeeded())
        {
            if (progress.validating != nullptr)
            {
                progress.validating(progress.user);
            }
            result.status = package.ValidateGeometryPart(cancellation);
            if (result.status.Succeeded())
            {
                result.status = decoder.Validate(*scene);
            }
        }
        if (result.status.Succeeded() && cancellation != nullptr && cancellation->IsCancelled())
        {
            result.status = {StatusCode::Cancelled, "Scene read cancelled"};
        }
        if (result.status.Succeeded())
        {
            if (runtimeStorage && detail::HasRedundantCoordinates(*scene, cancellation))
            {
                std::vector<VertexTextureCoordinates>().swap(scene->vertexTexCoords);
            }
            if (cancellation != nullptr && cancellation->IsCancelled())
            {
                result.status = {StatusCode::Cancelled, "Scene read cancelled"};
            }
            else
            {
                result.scene    = std::move(scene);
                result.metadata = decoder.TakeMetadata();
            }
        }
    }
    return result;
}
SceneReadResult ReadSceneImpl(const Package&                 package,
                              const SceneReadLimits&         limits,
                              const Cancellation*            cancellation,
                              std::shared_ptr<const Package> deferredPackage,
                              SceneReadProgress              progress)
{
    SceneReadResult result = ReadSceneAttempt(package, limits, cancellation, deferredPackage, progress);
    if (result.status.code == StatusCode::ResourceLimit && limits.workerCount > 1 && !package.Parts().empty())
    {
        SceneReadLimits serial = limits;
        serial.workerCount     = 1;
        result                 = ReadSceneAttempt(package, serial, cancellation, std::move(deferredPackage), progress);
    }
    return result;
}
} // namespace

SceneReadResult ReadScene(const Package&         package,
                          const SceneReadLimits& limits,
                          const Cancellation*    cancellation,
                          SceneReadProgress      progress)
{
    return ReadSceneImpl(package, limits, cancellation, {}, progress);
}

SceneReadResult ReadSceneDeferredImages(std::shared_ptr<const Package> package,
                                        const SceneReadLimits&         limits,
                                        const Cancellation*            cancellation,
                                        SceneReadProgress              progress)
{
    SceneReadResult result;
    if (!package)
    {
        result.status = {StatusCode::InvalidArgument, "Deferred image reading requires an open package"};
    }
    else
    {
        result = ReadSceneImpl(*package, limits, cancellation, package, progress);
    }
    return result;
}

SceneReadResult ReadSceneDeferredImages(const std::string&     path,
                                        const SceneReadLimits& limits,
                                        const Cancellation*    cancellation,
                                        SceneReadProgress      progress)
{
    const PackageResult opened = OpenPackage(path, limits.package, cancellation);
    SceneReadResult     result;
    result.status = opened.status;
    if (result.status.Succeeded())
    {
        result = ReadSceneDeferredImages(opened.package, limits, cancellation, progress);
    }
    return result;
}

SceneImageReadResult ReadSceneImage(const Package&         package,
                                    uint32_t               index,
                                    const SceneReadLimits& limits,
                                    const Cancellation*    cancellation)
{
    SceneImageReadResult result;
    uint64_t             allocationBytes = 0;
    result.status                        = ValidateReadLimits(package, limits, sizeof(ImageAsset), allocationBytes);
    if (result.status.Succeeded())
    {
        SceneReadLimits charged     = limits;
        charged.maxAllocationBytes -= allocationBytes;
        SceneDecoder decoder(package, charged, cancellation);
        decoder.Decode(ResourceType::Image, index, result.image);
        result.status = decoder.ResultStatus();
        if (result.status.Succeeded() && !ValidateImagePixels(result.image.pixels))
        {
            result.status = {StatusCode::InvalidData,
                             "Invalid image payload "
                                 + std::string(package.PartPath(detail::ResourceId(ResourceType::Image, index)))};
        }
    }
    if (!result.status.Succeeded())
    {
        result.image = {};
    }
    return result;
}

SceneReadResult ReadScene(const std::string&     path,
                          const SceneReadLimits& limits,
                          const Cancellation*    cancellation,
                          SceneReadProgress      progress)
{
    ZENASSET_MEASURE_SCOPE(Read);
    SceneReadResult     result;
    const PackageResult package = OpenPackage(path, limits.package, cancellation);
    result.status               = package.status;
    if (result.status.Succeeded())
    {
        result = ReadScene(*package.package, limits, cancellation, progress);
    }
    return result;
}
SceneMetadataResult ReadSceneMetadata(const Package& package, const SceneReadLimits& limits, const Cancellation* cancellation)
{
    SceneMetadataResult result;
    uint64_t            allocationBytes = 0;
    result.status                       = ValidateReadLimits(package, limits, sizeof(SceneMetadataResult), allocationBytes);
    if (cancellation != nullptr && cancellation->IsCancelled())
    {
        result.status = {StatusCode::Cancelled, "Scene metadata read cancelled"};
    }
    if (result.status.Succeeded())
    {
        SceneReadLimits charged     = limits;
        charged.maxAllocationBytes -= allocationBytes;
        SceneDecoder          decoder(package, charged, cancellation);
        detail::SceneMetadata metadata;
        decoder.Decode(ResourceType::Metadata, 0, metadata);
        result.status = decoder.ResultStatus();
        if (result.status.Succeeded() && cancellation != nullptr && cancellation->IsCancelled())
        {
            result.status = {StatusCode::Cancelled, "Scene metadata read cancelled"};
        }
        if (result.status.Succeeded())
        {
            result.metadata     = std::move(metadata.build);
            result.sourcePath   = std::move(metadata.sourcePath);
            result.dependencies = std::move(metadata.dependencies);
        }
    }
    return result;
}
SceneMetadataResult ReadSceneMetadata(const std::string& path, const SceneReadLimits& limits, const Cancellation* cancellation)
{
    SceneMetadataResult result;
    const PackageResult package = OpenPackage(path, limits.package, cancellation);
    result.status               = package.status;
    if (result.status.Succeeded())
    {
        result = ReadSceneMetadata(*package.package, limits, cancellation);
    }
    return result;
}
} // namespace zenasset
