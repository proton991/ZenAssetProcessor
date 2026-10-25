#include "ZenAsset/Processor/Write.h"
#include "Diagnostics/StageMeasurements.h"
#include "ZenAsset/Format/Validation.h"
#include "Format/SceneArchive.h"
#include "Format/SceneResources.h"
#include "Format/PackageLayout.h"
#include "Format/FileIO.h"
#include "Writer/Compression.h"
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
    return settings.spatialPages || settings.spatialMaxDimension != defaults.spatialMaxDimension
                || settings.spatialMaxPages != defaults.spatialMaxPages
                || settings.spatialMaxImages != defaults.spatialMaxImages
             ? 8
             : 7;
}

uint32_t ResourceVersion(ResourceType, const ImageAsset& image)
{
    return image.pixels->arrayLayers == 1 ? 1 : 2;
}

uint32_t ResourceVersion(ResourceType, const TextureAsset& texture)
{
    return texture.atlasRegion != ImageRegion{} ? 4 : texture.arrayLayer == 0 ? 2 : 3;
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
    void Metadata(const Scene&)
    {
        counts[static_cast<size_t>(ResourceType::Metadata)] = 1;
    }
};

class SceneEncoder
{
public:
    SceneEncoder(FILE* file, uint64_t count, const SceneWriteOptions& options, const Cancellation* cancellation) :
        m_file(file),
        m_options(options),
        m_cancellation(cancellation),
        m_position(detail::HeaderBytes + count * detail::DirectoryEntryBytes),
        m_bufferBudget(options.limits.maxAllocationBytes - count * sizeof(ResourceRecord))
    {
        m_records.reserve(static_cast<size_t>(count));
    }
    template <class T> void Encode(ResourceType type, uint32_t index, const T& value)
    {
        if (m_status.Succeeded())
        {
            ZENASSET_MEASURE_BEGIN(serialization, Serialize);
            const SceneReadLimits       limits  = detail::ResourceLimits(type, m_options.limits);
            const uint32_t              version = ResourceVersion(type, value);
            detail::SceneArchive<false> sizing({}, nullptr, limits, m_bufferBudget, m_cancellation, version);
            sizing(value);
            m_status = sizing.Finish();
            std::vector<std::byte> bytes;
            if (m_status.Succeeded())
            {
                bytes.resize(sizing.ByteSize());
                detail::SceneArchive<false> archive({}, &bytes, limits, m_bufferBudget, m_cancellation, version);
                archive(value);
                m_status = archive.Finish();
            }
            ZENASSET_MEASURE_END(serialization);
            if (m_status.Succeeded())
            {
                detail::EncodedMeshResource encoded =
                    detail::EncodeMeshResource(type, bytes, m_options.compression.meshoptimizer,
                                               m_bufferBudget - bytes.capacity(), m_options.maxEncoderBytes, m_cancellation);
                m_status = encoded.status;
                if (m_status.Succeeded() && encoded.encoding != 0)
                {
                    bytes = std::move(encoded.bytes);
                }
                if (m_status.Succeeded())
                {
                    detail::CompressedResource compressed =
                        detail::CompressResource(bytes, m_options.compression, m_bufferBudget - bytes.capacity(),
                                                 m_options.maxEncoderBytes, m_cancellation);
                    m_status                                 = compressed.status;
                    const std::span<const std::byte> stored  = compressed.codec == TransportCodec::None
                                                                 ? std::span<const std::byte>(bytes)
                                                                 : std::span<const std::byte>(compressed.bytes);
                    const uint64_t                   padding = (16 - m_position % 16) % 16;
                    if (m_status.Succeeded()
                        && (padding > m_options.limits.package.maxFileBytes - m_position
                            || stored.size() > m_options.limits.package.maxFileBytes - m_position - padding
                            || bytes.size() > m_options.limits.package.maxDecodedBytes - m_decodedBytes))
                    {
                        m_status = {StatusCode::ResourceLimit, "Scene package exceeds its configured byte limits"};
                    }
                    else if (m_status.Succeeded())
                    {
                        const uint8_t zeros[16]{};
                        m_status    = detail::WriteFileBytes(m_file, zeros, padding, m_cancellation);
                        m_position += padding;
                        if (m_status.Succeeded())
                        {
                            ResourceRecord record;
                            record.id           = detail::ResourceId(type, index);
                            record.type         = type;
                            record.version      = version;
                            record.codec        = compressed.codec;
                            record.encoding     = encoded.encoding;
                            record.offset       = m_position;
                            record.storedBytes  = stored.size();
                            record.decodedBytes = bytes.size();
                            ZENASSET_MEASURE_BEGIN(checksum, Checksum);
                            record.checksum = detail::HashBytes(stored.data(), stored.size());
                            ZENASSET_MEASURE_END(checksum);
                            m_status = detail::WriteFileBytes(m_file, stored.data(), stored.size(), m_cancellation);
                            m_records.push_back(record);
                            m_position     += stored.size();
                            m_decodedBytes += bytes.size();
                        }
                    }
                }
            }
        }
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
    void Metadata(const Scene& scene)
    {
        const detail::SceneMetadataView metadata{m_options.metadata, scene.sourcePath, scene.dependencies,
                                                 m_options.compression};
        Encode(ResourceType::Metadata, 0, metadata);
    }
    Status Finish()
    {
        ZENASSET_MEASURE_SCOPE(Serialize);
        if (m_status.Succeeded() && !detail::SeekFile(m_file, detail::HeaderBytes))
        {
            m_status = {StatusCode::IOError, "Cannot seek to scene package directory"};
        }
        kernels::HashState hash{};
        kernels::ResetHash(hash, 0);
        for (const ResourceRecord& record : m_records)
        {
            std::array<uint8_t, detail::DirectoryEntryBytes> entry{};
            detail::Write64(entry.data(), record.id);
            detail::Write32(entry.data() + 8, static_cast<uint32_t>(record.type));
            detail::Write32(entry.data() + 12, record.version);
            detail::Write32(entry.data() + 16, record.flags);
            detail::Write32(entry.data() + 20, static_cast<uint32_t>(record.codec));
            detail::Write64(entry.data() + 24, record.offset);
            detail::Write64(entry.data() + 32, record.storedBytes);
            detail::Write64(entry.data() + 40, record.decodedBytes);
            detail::Write64(entry.data() + 48, record.checksum);
            detail::Write32(entry.data() + 56, record.alignment);
            detail::Write32(entry.data() + 60, record.encoding);
            kernels::UpdateHash(hash, entry.data(), entry.size());
            if (m_status.Succeeded())
            {
                m_status = detail::WriteFileBytes(m_file, entry.data(), entry.size(), m_cancellation);
            }
        }
        std::array<uint8_t, detail::HeaderBytes> header{};
        std::copy(detail::Magic.begin(), detail::Magic.end(), header.begin());
        detail::Write32(header.data() + 8, PackageSchemaVersion);
        detail::Write32(header.data() + 12, detail::ByteOrder);
        detail::Write32(header.data() + 16, detail::HeaderBytes);
        detail::Write32(header.data() + 20, detail::DirectoryEntryBytes);
        detail::Write64(header.data() + 24, detail::HeaderBytes);
        detail::Write64(header.data() + 32, m_records.size());
        detail::Write64(header.data() + 40, m_position);
        detail::Write64(header.data() + 48, kernels::FinishHash(hash));
        detail::Write64(header.data() + 56, detail::HashBytes(header.data(), 56));
        if (m_status.Succeeded() && !detail::SeekFile(m_file, 0))
        {
            m_status = {StatusCode::IOError, "Cannot seek to scene package header"};
        }
        if (m_status.Succeeded())
        {
            m_status = detail::WriteFileBytes(m_file, header.data(), header.size(), m_cancellation);
        }
        return m_status;
    }

private:
    FILE*                       m_file;
    const SceneWriteOptions&    m_options;
    const Cancellation*         m_cancellation;
    uint64_t                    m_position;
    uint64_t                    m_decodedBytes{0};
    uint64_t                    m_bufferBudget;
    std::vector<ResourceRecord> m_records;
    Status                      m_status;
};

detail::FileOpenResult OpenTemporary(const std::filesystem::path& destination, std::filesystem::path& temporary)
{
    static std::atomic<uint64_t> serial{0};
    const int64_t                tick    = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    temporary                            = destination;
    temporary                           += ".tmp." + std::to_string(tick) + "." + std::to_string(serial.fetch_add(1));
    const detail::FileOpenResult result  = detail::OpenFile(temporary, true);
    return result;
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
    if (status.Succeeded() && !ValidateSceneAsset(scene, options.limits.maxRuntimeGeometryBytes, &failure))
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
    std::filesystem::path temporary;
    FILE*                 file = nullptr;
    if (status.Succeeded())
    {
        const detail::FileOpenResult opened = OpenTemporary(destination, temporary);
        status                              = opened.status;
        file                                = opened.file;
    }
    if (status.Succeeded())
    {
        const uint8_t zeros[4096]{};
        uint64_t      remaining = prefixBytes;
        while (status.Succeeded() && remaining > 0)
        {
            const uint64_t transfer  = std::min<uint64_t>(remaining, sizeof(zeros));
            status                   = detail::WriteFileBytes(file, zeros, transfer, cancellation);
            remaining               -= transfer;
        }
        if (status.Succeeded())
        {
            SceneEncoder                    encoder(file, count, options, cancellation);
            const detail::SceneManifestView manifest{scene.name, scene.roots, scene.vertexTexCoordSets, scene.imageBasedLight,
                                                     counter.counts};
            encoder.Encode(ResourceType::Scene, 0, manifest);
            detail::VisitSceneResources(encoder, scene);
            status = encoder.Finish();
        }
        if (status.Succeeded())
        {
            status = detail::FlushFile(file, options.durable);
        }
    }
    if (file != nullptr)
    {
        ZENASSET_MEASURE_BEGIN(closing, FileClose);
        const bool closed = std::fclose(file) == 0;
        ZENASSET_MEASURE_END(closing);
        if (status.Succeeded() && !closed)
        {
            status = {StatusCode::IOError, "Cannot close complete scene package"};
        }
        if (status.Succeeded() && cancellation != nullptr && cancellation->IsCancelled())
        {
            status = {StatusCode::Cancelled, "Scene write cancelled"};
        }
        if (status.Succeeded() && options.beforePublish != nullptr)
        {
            ZENASSET_MEASURE_SCOPE(BeforePublish);
            status = options.beforePublish(options.publicationUser);
        }
        if (status.Succeeded() && cancellation != nullptr && cancellation->IsCancelled())
        {
            status = {StatusCode::Cancelled, "Scene write cancelled"};
        }
        if (status.Succeeded())
        {
            status = detail::ReplaceFileAtomically(temporary, destination, options.durable, options.published);
        }
        if (!status.Succeeded())
        {
            std::error_code removeError;
            std::filesystem::remove(temporary, removeError);
        }
    }
    return status;
}
} // namespace zenasset
