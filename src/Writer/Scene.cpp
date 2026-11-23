#include "ZenAsset/Processor/Write.h"
#include "Diagnostics/StageMeasurements.h"
#include "Format/SceneValidation.h"
#include "Format/SceneArchive.h"
#include "Format/SceneResources.h"
#include "Format/PackageLayout.h"
#include "Format/FileIO.h"
#include "Writer/Compression.h"
#include "Writer/PackageWriter.h"
#include "Writer/ImportImageWriter.h"
#include "Reader/Transport.h"
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

// A staged image owns a reader lease on the directory. Consumers can resolve the
// native part even if cache publication fails or another generation replaces it.
class ImportedImagePart final : public detail::ValidatedImagePayloadSource
{
public:
    ImportedImagePart(const detail::DirectoryParts&          parts,
                      std::shared_ptr<detail::CacheFileLock> pin,
                      const ResourceRecord&                  record,
                      const ImageAsset&                      image,
                      const SceneWriteOptions&               options) :
        m_root(parts.Root()),
        m_path(m_root / detail::PartName(detail::PartKind::Image, record.checksum)),
        m_pin(std::move(pin)),
        m_record(record),
        m_layout(*image.pixels),
        m_name(image.name),
        m_compression(options.compression),
        m_limits(options.limits),
        m_durable(options.durable)
    {}

    bool HasValidatedPixels(const ImagePixels& descriptor) const override
    {
        return m_layout.Matches(descriptor)
            && detail::CheckStoredFile(m_path, m_record.storedBytes, m_record.checksum, nullptr).Succeeded();
    }

    bool Reuse(const detail::DirectoryParts& parts,
               const SceneWriteOptions&      options,
               const ImageAsset&             image,
               uint32_t                      index,
               ResourceRecord&               record,
               const Cancellation*           cancellation) const
    {
        bool reusable =
            parts.Root() == m_root && image.name == m_name && m_layout.Matches(*image.pixels)
            && m_record.id == detail::ResourceId(ResourceType::Image, index)
            && image.pixels->sourceImageChecksum == m_record.sourceImageChecksum
            && !HasProcessingOperations(options.metadata.processing) && options.compression.transport == m_compression.transport
            && options.compression.zstandardLevel == m_compression.zstandardLevel && (!options.durable || m_durable)
            && options.limits.maxArrayElements >= m_limits.maxArrayElements && options.limits.maxStringBytes >= m_name.size()
            && detail::CheckStoredFile(m_path, m_record.storedBytes, m_record.checksum, cancellation).Succeeded();
        if (reusable)
        {
            record = m_record;
        }
        return reusable;
    }

    Status Read(std::shared_ptr<const ImagePixels>& pixels, const Cancellation* cancellation) const override
    {
        return ReadBounded(pixels, cancellation, m_limits.maxAllocationBytes);
    }

    Status ReadBounded(std::shared_ptr<const ImagePixels>& pixels,
                       const Cancellation*                 cancellation,
                       uint64_t                            maxBytes) const override
    {
        uint64_t budget = std::min(maxBytes, m_limits.maxAllocationBytes);
        Status   status;
        pixels.reset();
        if (m_record.decodedBytes > budget || m_record.decodedBytes > m_limits.package.maxChunkBytes)
        {
            status = {StatusCode::ResourceLimit, "Imported image part exceeds read budget"};
        }
        if (status.Succeeded())
        {
            detail::FileOpenResult opened = detail::OpenStoredPart(m_path);
            status                        = opened.status;
            if (opened.file)
            {
                uint64_t length = 0;
                if (!detail::FileLength(opened.file, length) || length != m_record.storedBytes
                    || !detail::SeekFile(opened.file, 0))
                {
                    status = {StatusCode::InvalidData, "Imported image part length changed"};
                }
                if (status.Succeeded())
                {
                    budget            -= m_record.decodedBytes;
                    ResourceData data  = detail::ReadResourceData(
                        opened.file, m_record, std::min(budget, m_limits.package.maxCodecBytes), cancellation);
                    status = data.status;
                    if (status.Succeeded())
                    {
                        ImageAsset                 loaded;
                        detail::SceneArchive<true> archive(data.bytes, nullptr, m_limits, budget, cancellation,
                                                           m_record.version);
                        archive(loaded);
                        status = archive.Finish();
                        if (status.Succeeded() && !ValidateImagePixels(loaded.pixels))
                        {
                            status = {StatusCode::InvalidData, "Invalid imported image part"};
                        }
                        if (status.Succeeded())
                        {
                            pixels = std::move(loaded.pixels);
                        }
                    }
                }
                std::fclose(opened.file);
            }
        }
        return status;
    }

private:
    std::filesystem::path                  m_root, m_path;
    std::shared_ptr<detail::CacheFileLock> m_pin;
    ResourceRecord                         m_record;
    detail::DeferredImageLayout            m_layout;
    std::string                            m_name;
    CompressionSettings                    m_compression;
    SceneReadLimits                        m_limits;
    bool                                   m_durable;
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
        const ImportedImagePart* imported =
            chargeLease ? dynamic_cast<const ImportedImagePart*>(value.pixels->payloadSource.get()) : nullptr;
        payload.partWritten = imported && imported->Reuse(*m_parts, m_options, value, index, payload.record, m_cancellation);
        if (!payload.partWritten)
        {
            payload.status =
                ResolveImagePixels(value.pixels, resident.pixels, m_cancellation, chargeLease ? budget : UINT64_MAX);
        }
        if (chargeLease && resident.pixels && payload.status.Succeeded())
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
        if (!payload.partWritten && payload.status.Succeeded() && !ValidateImagePixels(resident.pixels))
        {
            payload.status = {StatusCode::InvalidData, "Cannot write invalid image payload"};
        }
        if (!payload.partWritten && payload.status.Succeeded())
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
        if (payload.partWritten || (payload.status.Succeeded() && m_parts && m_parts->ReuseImage(payload.record)))
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
detail::ImportImageWriter::ImportImageWriter(const std::filesystem::path& manifest,
                                             const SceneWriteOptions&     options,
                                             const Cancellation*          cancellation) :
    m_options(options),
    m_pin(std::make_shared<CacheFileLock>()),
    m_cancellation(cancellation),
    m_parts(manifest.has_parent_path() ? manifest.parent_path() : ".", options.durable, cancellation)
{
    // The final manifest writer owns public resource counts. Worker-local encoders
    // have no manifest or complete resource count yet.
    m_options.resourceProgress = nullptr;
}

detail::ImportImageWriter::~ImportImageWriter()
{
    if (m_options.partStats)
    {
        *m_options.partStats = m_parts.ImageStats();
    }
}

std::unique_ptr<detail::ImportImageWriter> detail::ImportImageWriter::Open(const SceneImportImageCache& target,
                                                                           size_t                       imageCount,
                                                                           const Cancellation*          cancellation)
{
    std::filesystem::path              manifest;
    std::unique_ptr<ImportImageWriter> writer;
    if (imageCount && target.options.directoryLayout && !HasProcessingOperations(target.options.metadata.processing)
        && ValidateCompressionSettings(target.options.compression).Succeeded()
        && MakePath(target.manifest, manifest).Succeeded())
    {
        writer.reset(new ImportImageWriter(manifest, target.options, cancellation));
        writer->m_parts.SetImageCount(imageCount);
        if (!writer->m_parts.Begin(manifest, writer->m_options).Succeeded()
            || !writer->m_pin->Acquire(writer->m_parts.Root() / ".zen-reader.lock", false, true, true, cancellation)
                    .Succeeded())
        {
            writer.reset();
        }
    }
    return writer;
}

bool detail::ImportImageWriter::Store(uint32_t index, const std::string& name, const std::shared_ptr<ImagePixels>& pixels)
{
    SceneEncoder          encoder(nullptr, 0, m_options, m_cancellation, &m_parts);
    const uint32_t        slots  = std::max(1u, m_options.maxImageWorkers);
    const uint64_t        budget = m_options.limits.maxAllocationBytes / slots;
    const ImageAsset      image{name, pixels};
    SceneEncoder::Payload payload = encoder.SerializeImage(index, image, budget);
    encoder.EncodeImagePart(payload, budget, m_options.maxEncoderBytes / slots);
    if (payload.partWritten)
    {
        pixels->payloadSource = std::make_shared<ImportedImagePart>(m_parts, m_pin, payload.record, image, m_options);
        std::vector<uint8_t>().swap(pixels->bytes);
        for (std::vector<uint8_t>& mip : pixels->mips)
        {
            std::vector<uint8_t>().swap(mip);
        }
    }
    return payload.partWritten;
}

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
