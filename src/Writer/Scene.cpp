#include "ZenAsset/Processor/Write.h"
#include "Diagnostics/StageMeasurements.h"
#include "Format/SceneValidation.h"
#include "Format/SceneArchive.h"
#include "Format/SceneResources.h"
#include "Format/PackageLayout.h"
#include "Format/FileIO.h"
#include "Writer/Compression.h"
#include "Writer/PackageWriter.h"
#include "Processor/WorkerPool.h"
#include "Format/MeshEncoding.h"
#include <atomic>
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
        payload.status = ResolveImagePixels(value.pixels, resident.pixels, m_cancellation);
        if (payload.status.Succeeded() && !ValidateImagePixels(resident.pixels))
        {
            payload.status = {StatusCode::InvalidData, "Cannot write invalid image payload"};
        }
        if (payload.status.Succeeded())
        {
            payload = Serialize(ResourceType::Image, index, resident, ResourceVersion(ResourceType::Image, resident), budget);
        }
        return payload;
    }

    void EncodeImage(uint32_t index, const ImageAsset& image)
    {
        Payload payload = SerializeImage(index, image, m_bufferBudget);
        Compress(payload, m_bufferBudget, m_options.maxEncoderBytes);
        Append(payload);
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
                Compress(payload, budget, encoderBudget);
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
        const uint32_t slots = std::min({m_options.maxImageWorkers, MaxImageJobs, static_cast<uint32_t>(images.size()),
                                         std::thread::hardware_concurrency()});
        std::unique_ptr<WorkerPool> workers;
        if (m_status.Succeeded() && slots > 1 && m_options.compression.transport == TransportCodec::Zstandard)
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
        for (uint32_t first = 0; m_status.Succeeded() && first < images.size();)
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
};

struct SceneEncodingContext
{
    const Scene&                  scene;
    const SceneWriteOptions&      options;
    const Cancellation*           cancellation;
    const detail::ResourceCounts& counts;
    uint64_t                      count;
};

Status EncodeScenePackage(FILE* file, void* user)
{
    const SceneEncodingContext&     context = *static_cast<SceneEncodingContext*>(user);
    SceneEncoder                    encoder(file, context.count, context.options, context.cancellation);
    const detail::SceneManifestView manifest{context.scene.name, context.scene.roots, context.scene.vertexTexCoordSets,
                                             context.scene.imageBasedLight, context.counts};
    encoder.Encode(ResourceType::Scene, 0, manifest);
    detail::VisitSceneResources(encoder, context.scene);
    return encoder.Finish();
}
} // namespace

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
    const uint64_t prefixBytes = detail::HeaderBytes + count * detail::DirectoryEntryBytes;
    if (count > options.limits.package.maxResources || count > options.limits.package.maxDirectoryBytes / sizeof(ResourceRecord)
        || count > options.limits.maxAllocationBytes / sizeof(ResourceRecord) || count > SIZE_MAX / sizeof(ResourceRecord)
        || prefixBytes > options.limits.package.maxFileBytes)
    {
        status = {StatusCode::ResourceLimit, "Scene package directory exceeds its configured limits"};
    }
    ZENASSET_MEASURE_END(validation);
    if (status.Succeeded())
    {
        SceneEncodingContext context{scene, options, cancellation, counter.counts, count};
        status = detail::WritePackage(path, count, options, cancellation, EncodeScenePackage, &context);
    }
    return status;
}
} // namespace zenasset
