#pragma once
#include "ZenAsset/Processor/Write.h"
#include "Diagnostics/StageMeasurements.h"
#include "Format/SceneArchive.h"
#include "Format/PackageLayout.h"
#include "Format/FileIO.h"
#include "Writer/Compression.h"
#include "Format/MeshEncoding.h"

namespace zenasset::detail
{
class ResourceEncoder
{
public:
    ResourceEncoder(FILE* file, uint64_t count, const SceneWriteOptions& options, const Cancellation* cancellation) :
        m_file(file),
        m_options(options),
        m_cancellation(cancellation),
        m_position(detail::HeaderBytes + count * detail::DirectoryEntryBytes),
        m_bufferBudget(options.limits.maxAllocationBytes - count * sizeof(ResourceRecord))
    {
        m_records.reserve(static_cast<size_t>(count));
    }
    template <class T> void Encode(ResourceType type, uint32_t index, const T& value, uint32_t version = 1)
    {
        if (m_status.Succeeded())
        {
            ZENASSET_MEASURE_BEGIN(serialization, Serialize);
            const SceneReadLimits       limits = detail::ResourceLimits(type, m_options.limits);
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

protected:
    FILE*                       m_file;
    const SceneWriteOptions&    m_options;
    const Cancellation*         m_cancellation;
    uint64_t                    m_position;
    uint64_t                    m_decodedBytes{0};
    uint64_t                    m_bufferBudget;
    std::vector<ResourceRecord> m_records;
    Status                      m_status;
};

using PackageWriterCallback = Status (*)(FILE*, void*);
Status                               WritePackage(const std::string&       path,
                                                  uint64_t                 count,
                                                  const SceneWriteOptions& options,
                                                  const Cancellation*      cancellation,
                                                  PackageWriterCallback    encode,
                                                  void*                    user);
} // namespace zenasset::detail
