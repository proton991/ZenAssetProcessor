#include "ZenAsset/Processor/Write.h"
#include "Diagnostics/StageMeasurements.h"
#include "Format/SceneValidation.h"
#include "Format/SceneArchive.h"
#include "Format/SceneResources.h"
#include "Format/PackageLayout.h"
#include "Format/FileIO.h"
#include "Writer/Compression.h"
#include "Writer/PackageWriter.h"
#include "Format/WorkerPool.h"
#include "Format/MeshEncoding.h"
#include <atomic>
#include "ZenFoundation/Templates/HeapVector.h"
#include <chrono>
#include <filesystem>

namespace zenasset
{
namespace
{
template <class T> uint32_t ResourceVersion(ResourceType type, const T&)
{
    return type == ResourceType::Metadata ? 7 : type == ResourceType::Environment ? 2 : 1;
}

uint32_t ResourceVersion(ResourceType, const detail::SceneMetadataView& metadata)
{
    const TextureSettings& settings = metadata.build.processing.textures;
    const TextureSettings  defaults;
    return settings.collectAlphaStatistics ? 9
         : settings.spatialPages || settings.spatialMaxDimension != defaults.spatialMaxDimension
                   || settings.spatialMaxPages != defaults.spatialMaxPages
                   || settings.spatialMaxImages != defaults.spatialMaxImages
             ? 8
             : 7;
}

uint32_t ResourceVersion(ResourceType, const ImageAsset& image)
{
    return !image.pixels->mips.empty() ? 3 : image.pixels->arrayLayers == 1 ? 1 : 2;
}

uint32_t ResourceVersion(ResourceType, const TextureAsset& texture)
{
    return !texture.alphaMips.empty() ? 5 : texture.atlasRegion != ImageRegion{} ? 4 : texture.arrayLayer == 0 ? 2 : 3;
}

class ResourceCounter
{
public:
    detail::ResourceCounts  counts{};
    template <class T> void Array(ResourceType type, const std::vector<T>& values)
    {
        counts[static_cast<size_t>(type)] = values.size();
    }
    template <class T> void Single(ResourceType type, const T&)
    {
        counts[static_cast<size_t>(type)] = 1;
    }
    void Coordinates(const Scene& scene)
    {
        Single(ResourceType::TextureCoordinates, scene.vertexTexCoords);
    }
    void Metadata(const Scene&)
    {
        counts[static_cast<size_t>(ResourceType::Metadata)] = 1;
    }
};

class SceneEncoder : public detail::ResourceEncoder
{
public:
    using detail::ResourceEncoder::ResourceEncoder;
    Payload SerializeImage(uint32_t index, const ImageAsset& value, uint64_t budget) const
    {
        ImageAsset resident;
        resident.name = value.name;
        Payload payload;
        const bool chargeLease = m_parts && value.pixels && value.pixels->payloadSource;
        payload.status = ResolveImagePixels(value.pixels, resident.pixels, m_cancellation, chargeLease ? budget : UINT64_MAX);
        if (chargeLease && payload.status.Succeeded())
        {
            const ImagePixels& pixels = *resident.pixels;
            uint64_t           lease =
                sizeof(ImagePixels) + pixels.mips.capacity() * sizeof(std::vector<uint8_t>) + pixels.bytes.capacity();
            for (const std::vector<uint8_t>& mip : pixels.mips)
            {
                lease += mip.capacity();
            }
            if (lease > budget)
            {
                payload.status = {StatusCode::ResourceLimit, "Deferred image exceeds the writer allocation limit"};
            }
            else
            {
                budget -= lease;
            }
        }
        if (payload.status.Succeeded() && !ValidateImagePixels(resident.pixels))
        {
            payload.status = {StatusCode::InvalidData, "Cannot write invalid image payload"};
        }
        if (payload.status.Succeeded())
        {
            payload = Serialize(ResourceType::Image, index, resident, ResourceVersion(ResourceType::Image, resident), budget);
        }
        if (payload.status.Succeeded() && m_parts && !HasProcessingOperations(m_options.metadata.processing))
        {
            payload.record.sourceImageChecksum = value.pixels->sourceImageChecksum;
        }
        return payload;
    }

    void EncodeImage(uint32_t index, const ImageAsset& image)
    {
        Payload payload = SerializeImage(index, image, m_bufferBudget);
        EncodeImagePart(payload, m_bufferBudget, m_options.maxEncoderBytes);
        Append(payload);
    }

    void EncodeImagePart(Payload& payload, uint64_t budget, uint64_t encoderBudget) const
    {
        if (payload.status.Succeeded() && m_parts && m_parts->ReuseImage(payload.record))
        {
            payload.partWritten = true;
            m_parts->CountImage(true);
        }
        else
        {
            Compress(payload, budget, encoderBudget);
            if (payload.status.Succeeded() && m_parts)
            {
                payload.status      = m_parts->Image(payload.record, payload.bytes);
                payload.partWritten = payload.status.Succeeded();
                if (payload.partWritten)
                {
                    m_parts->CountImage(false);
                }
            }
        }
        if (payload.partWritten)
        {
            std::vector<std::byte>().swap(payload.bytes);
        }
    }

    void StartDirectoryImages(const std::vector<ImageAsset>& images)
    {
        if (m_parts)
        {
            m_parts->SetImageCount(images.size());
        }
        const size_t slots =
            std::min<size_t>({m_options.maxImageWorkers, std::thread::hardware_concurrency(), images.size() + 1});
        const size_t stateBytes = images.size() * sizeof(Payload);
        if (m_parts && !images.empty() && slots > 1 && stateBytes < m_bufferBudget)
        {
            m_savedBudget    = m_bufferBudget;
            m_bufferBudget   = (m_bufferBudget - stateBytes) / slots;
            m_encoderBudget /= slots;
            m_directoryImages.resize(images.size());
            m_workers = std::make_unique<WorkerPool>(static_cast<uint32_t>(slots - 1));
            for (size_t index = 0; index < m_workers->GetSize(); ++index)
            {
                m_jobs.push_back(
                    m_workers->Push(&SceneEncoder::WriteNextImages, this, std::cref(images), m_bufferBudget, m_encoderBudget));
            }
        }
    }
    void WriteNextImages(const std::vector<ImageAsset>& images, uint64_t budget, uint64_t codecBudget)
    {
        size_t index = m_nextImage.fetch_add(1);
        while (index < images.size())
        {
            Payload payload = SerializeImage(static_cast<uint32_t>(index), images[index], budget);
            EncodeImagePart(payload, budget, codecBudget);
            std::vector<std::byte>().swap(payload.bytes);
            m_directoryImages[index] = std::move(payload);
            index                    = m_nextImage.fetch_add(1);
        }
    }
    void JoinDirectoryImages()
    {
        for (std::future<void>& job : m_jobs)
        {
            job.get();
        }
        m_workers.reset();
        if (m_savedBudget != 0)
        {
            m_bufferBudget  = m_savedBudget - m_directoryImages.size() * sizeof(Payload);
            m_encoderBudget = m_options.maxEncoderBytes;
        }
    }

    static constexpr uint32_t MaxImageJobs = 4;

    bool EncodeImageBatch(WorkerPool& workers, std::span<const ImageAsset> images, uint32_t first)
    {
        // Resolve on the caller. Each worker owns its serialized bytes, and
        // both allocation limits are shared across all outstanding images.
        const uint64_t                                 budget        = m_bufferBudget / images.size();
        const uint64_t                                 encoderBudget = m_options.maxEncoderBytes / images.size();
        std::array<std::future<Payload>, MaxImageJobs> pending;
        for (uint32_t job = 0; job < images.size(); ++job)
        {
            Payload payload = SerializeImage(first + job, images[job], budget);
            pending[job]    = workers.Push([this, payload = std::move(payload), budget, encoderBudget]() mutable {
                EncodeImagePart(payload, budget, encoderBudget);
                return std::move(payload);
            });
        }
        bool                              retrySerial = false;
        std::array<Payload, MaxImageJobs> payloads;
        for (size_t job = 0; job < images.size(); ++job)
        {
            payloads[job]        = pending[job].get();
            const Status& status = payloads[job].status;
            if (status.code == StatusCode::ResourceLimit)
            {
                retrySerial = true;
            }
            else if (!status.Succeeded() && m_status.Succeeded())
            {
                m_status = status;
            }
        }
        for (size_t job = 0; !retrySerial && m_status.Succeeded() && job < images.size(); ++job)
        {
            Append(payloads[job]);
        }
        return !retrySerial;
    }

    void Array(ResourceType, const std::vector<ImageAsset>& images)
    {
        const bool scheduled = m_workers != nullptr;
        if (scheduled)
        {
            JoinDirectoryImages();
            for (size_t index = 0; m_status.Succeeded() && index < images.size(); ++index)
            {
                Payload& job = m_directoryImages[index];
                if (job.status.code == StatusCode::ResourceLimit)
                {
                    EncodeImage(static_cast<uint32_t>(index), images[index]);
                }
                else
                {
                    Append(job);
                }
            }
            m_directoryImages.clear();
        }
        const uint32_t slots = std::min({m_options.maxImageWorkers, MaxImageJobs, static_cast<uint32_t>(images.size()),
                                         std::thread::hardware_concurrency()});
        std::unique_ptr<WorkerPool> workers;
        if (!scheduled && m_status.Succeeded() && slots > 1 && m_options.compression.transport == TransportCodec::Zstandard)
        {
            for (const ImageAsset& image : images)
            {
                if (uint64_t(image.pixels->width) * image.pixels->height >= 512u * 512u)
                {
                    workers = std::make_unique<WorkerPool>(slots);
                    break;
                }
            }
        }
        for (uint32_t first = 0; !scheduled && m_status.Succeeded() && first < images.size();)
        {
            const uint32_t count = workers ? std::min(slots, static_cast<uint32_t>(images.size()) - first) : 1;
            if (count == 1 || !EncodeImageBatch(*workers, std::span(images).subspan(first, count), first))
            {
                // An image that exceeds its share can still use the entire
                // budget once the batch releases its unpublished buffers.
                for (uint32_t job = 0; m_status.Succeeded() && job < count; ++job)
                {
                    EncodeImage(first + job, images[first + job]);
                }
            }
            first += count;
        }
    }
    template <class T> void Encode(ResourceType type, uint32_t index, const T& value)
    {
        detail::ResourceEncoder::Encode(type, index, value, ResourceVersion(type, value));
    }
    template <class T> void Array(ResourceType type, const std::vector<T>& values)
    {
        for (uint32_t index = 0; m_status.Succeeded() && index < values.size(); ++index)
        {
            Encode(type, index, values[index]);
        }
    }
    template <class T> void Single(ResourceType type, const T& value)
    {
        Encode(type, 0, value);
    }
    void Coordinates(const Scene& scene)
    {
        if (m_options.compression.meshoptimizer && detail::HasRedundantCoordinates(scene, m_cancellation))
        {
            detail::ResourceEncoder::Encode(ResourceType::TextureCoordinates, 0, uint64_t(scene.vertices.size()), 2);
        }
        else
        {
            Single(ResourceType::TextureCoordinates, scene.vertexTexCoords);
        }
    }
    void Metadata(const Scene& scene)
    {
        const detail::SceneMetadataView metadata{m_options.metadata, scene.sourcePath, scene.dependencies,
                                                 m_options.compression};
        Encode(ResourceType::Metadata, 0, metadata);
    }

private:
    std::atomic<size_t>                m_nextImage{0};
    uint64_t                           m_savedBudget{0};
    zen::HeapVector<Payload>           m_directoryImages;
    zen::HeapVector<std::future<void>> m_jobs;
    std::unique_ptr<WorkerPool>        m_workers;
};

struct SceneEncodingContext
{
    const Scene&                  scene;
    const SceneWriteOptions&      options;
    const Cancellation*           cancellation;
    const detail::ResourceCounts& counts;
    uint64_t                      count;
    detail::DirectoryParts*       parts;
};

Status EncodeScenePackage(FILE* file, void* user)
{
    const SceneEncodingContext&     context = *static_cast<SceneEncodingContext*>(user);
    SceneEncoder                    encoder(file, context.count, context.options, context.cancellation, context.parts);
    const detail::SceneManifestView manifest{context.scene.name, context.scene.roots, context.scene.vertexTexCoordSets,
                                             context.scene.imageBasedLight, context.counts};
    encoder.StartDirectoryImages(context.scene.images);
    encoder.Encode(ResourceType::Scene, 0, manifest);
    detail::VisitSceneResources(encoder, context.scene);
    return encoder.Finish();
}
struct Migration
{
    const Package&           source;
    const SceneWriteOptions& options;
    detail::DirectoryParts&  parts;
    const Cancellation*      cancellation;
};
Status TransferPackage(FILE* file, void* user)
{
    Migration&              job = *static_cast<Migration*>(user);
    detail::ResourceEncoder encoder(file, job.source.Resources().size(), job.options, job.cancellation, &job.parts);
    Status                  status;
    const uint64_t          available = job.options.limits.maxAllocationBytes - job.source.Resources().size_bytes();
    for (const ResourceRecord& record : job.source.Resources())
    {
        if (status.Succeeded())
        {
            ResourceData data = job.source.ReadStored(record.id, available, job.cancellation);
            status            = data.status;
            if (status.Succeeded())
            {
                detail::ResourceEncoder::Payload payload{record, std::move(data.bytes), {}};
                payload.record.offset = 0;
                encoder.Append(payload);
                status = encoder.ResultStatus();
            }
        }
    }
    if (status.Succeeded())
    {
        status = encoder.Finish();
    }
    return status;
}
} // namespace
Status MigrateSceneDirectory(const std::string&             destination,
                             std::shared_ptr<const Package> source,
                             const SceneWriteOptions&       options,
                             const Cancellation*            cancellation)
{
    Status                status;
    std::filesystem::path path;
    if (options.published != nullptr)
    {
        *options.published = false;
    }
    status = detail::MakePath(destination, path);
    if (!options.directoryLayout || !source || !source->Parts().empty())
    {
        status = {StatusCode::InvalidArgument, "Migration requires a flat package and a directory destination"};
    }
    SceneWriteOptions transfer = options;
    if (status.Succeeded())
    {
        SceneReadResult decoded = ReadSceneDeferredImages(source, options.limits, cancellation);
        status                  = decoded.status;
        if (status.Succeeded() && !options.metadata.buildFingerprint.empty()
            && decoded.metadata.buildFingerprint != options.metadata.buildFingerprint)
        {
            status = {StatusCode::InvalidData, "Flat cache producer does not match the directory variant"};
        }
        if (status.Succeeded())
        {
            // The source directory remains resident throughout the transfer. The
            // writer charges its own table and largest encoded record to the rest.
            transfer.limits.maxAllocationBytes -= source->DirectoryBytes();
            transfer.metadata                   = std::move(decoded.metadata);
            transfer.compression                = transfer.metadata.compression;
        }
    }
    detail::DirectoryParts parts(path.has_parent_path() ? path.parent_path() : ".", options.durable, cancellation);
    if (status.Succeeded())
    {
        status = parts.Begin(path, transfer);
    }
    if (status.Succeeded())
    {
        Migration migration{*source, transfer, parts, cancellation};
        status =
            detail::WritePackage(destination, source->Resources().size(), transfer, cancellation, TransferPackage, &migration);
    }
    return status;
}

Status WriteScene(const std::string&       path,
                  const Scene&             scene,
                  const SceneWriteOptions& options,
                  const Cancellation*      cancellation)
{
    ZENASSET_MEASURE_SCOPE(Write);
    ZENASSET_MEASURE_BEGIN(validation, WriteValidate);
    std::filesystem::path destination;
    if (options.published != nullptr)
    {
        *options.published = false;
    }
    Status      status = detail::MakePath(path, destination);
    std::string failure;
    if (status.Succeeded() && cancellation != nullptr && cancellation->IsCancelled())
    {
        status = {StatusCode::Cancelled, "Scene write cancelled"};
    }
    if (status.Succeeded())
    {
        status = detail::ValidateCompressionSettings(options.compression);
    }
    if (status.Succeeded())
    {
        status = ValidateProcessingSettings(options.metadata.processing);
    }
    if (status.Succeeded() && !detail::FitsValidationBudget(scene, options.limits.maxAllocationBytes))
    {
        status = {StatusCode::ResourceLimit, "Scene validation scratch exceeds the allocation budget"};
    }
    if (status.Succeeded() && !detail::ValidateSceneGraph(scene, options.limits.maxRuntimeGeometryBytes, &failure))
    {
        status = {StatusCode::InvalidData, "Cannot write invalid scene: " + failure};
    }
    ResourceCounter counter;
    counter.counts[static_cast<size_t>(ResourceType::Scene)] = 1;
    detail::VisitSceneResources(counter, scene);
    uint64_t count = 0;
    for (uint64_t value : counter.counts)
    {
        if (value > UINT32_MAX || value > options.limits.maxArrayElements)
        {
            status = {StatusCode::ResourceLimit, "Scene resource count exceeds its configured limit"};
        }
        count += value;
    }
    const uint64_t entryBytes  = options.directoryLayout ? detail::PartDirectoryEntryBytes : detail::DirectoryEntryBytes;
    const uint64_t prefixBytes = detail::HeaderBytes + count * entryBytes;
    if (count > options.limits.package.maxResources || count > options.limits.package.maxDirectoryBytes / sizeof(ResourceRecord)
        || count > options.limits.maxAllocationBytes / sizeof(ResourceRecord) || count > SIZE_MAX / sizeof(ResourceRecord)
        || prefixBytes > options.limits.package.maxFileBytes)
    {
        status = {StatusCode::ResourceLimit, "Scene package directory exceeds its configured limits"};
    }
    ZENASSET_MEASURE_END(validation);
    if (status.Succeeded())
    {
        SceneWriteOptions attempt = options;
        for (unsigned pass = 0; pass < 2; ++pass)
        {
            detail::DirectoryParts parts(destination.has_parent_path() ? destination.parent_path() : ".", options.durable,
                                         cancellation);
            if (options.directoryLayout)
            {
                status = parts.Begin(destination, attempt);
            }
            SceneEncodingContext context{scene,          attempt, cancellation,
                                         counter.counts, count,   options.directoryLayout ? &parts : nullptr};
            if (status.Succeeded())
            {
                status = detail::WritePackage(path, count, attempt, cancellation, EncodeScenePackage, &context);
            }
            if (!attempt.directoryLayout || status.code != StatusCode::ResourceLimit || attempt.maxImageWorkers <= 1)
            {
                break;
            }
            attempt.maxImageWorkers = 1;
            status                  = {};
        }
    }
    return status;
}
} // namespace zenasset
