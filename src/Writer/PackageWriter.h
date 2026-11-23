#pragma once
#include "ZenAsset/Processor/Write.h"
#include "Diagnostics/StageMeasurements.h"
#include "Format/SceneArchive.h"
#include "Format/PackageLayout.h"
#include "Format/FileIO.h"
#include "Writer/Compression.h"
#include "Writer/DirectoryParts.h"
#include <set>
#include "Format/MeshEncoding.h"

namespace zenasset::detail
{
class ResourceEncoder
{
public:
    ResourceEncoder(FILE*                    file,
                    uint64_t                 count,
                    const SceneWriteOptions& options,
                    const Cancellation*      cancellation,
                    DirectoryParts*          parts = nullptr) :
        m_file(file),
        m_options(options),
        m_cancellation(cancellation),
        m_resourceCount(count),
        m_parts(parts),
        m_entryBytes(parts ? PartDirectoryEntryBytes : DirectoryEntryBytes),
        m_position(detail::HeaderBytes + count * m_entryBytes),
        m_totalStored(m_position),
        m_bufferBudget(options.limits.maxAllocationBytes - count * sizeof(ResourceRecord)),
        m_encoderBudget(options.maxEncoderBytes)
    {
        m_records.reserve(static_cast<size_t>(count));
        if (m_options.resourceProgress != nullptr)
        {
            m_options.resourceProgress(m_options.progressUser, 0, m_resourceCount);
        }
    }
    struct Payload
    {
        ResourceRecord         record;
        std::vector<std::byte> bytes;
        Status                 status;
        bool                   partWritten{false};
    };

    template <class T>
    Payload Serialize(ResourceType type, uint32_t index, const T& value, uint32_t version, uint64_t budget) const
    {
        ZENASSET_MEASURE_SCOPE(Serialize);
        Payload payload;
        payload.record.type          = type;
        payload.record.id            = detail::ResourceId(type, index);
        payload.record.version       = version;
        const SceneReadLimits limits = detail::ResourceLimits(type, m_options.limits);
        SceneArchive<false>   sizing({}, nullptr, limits, budget, m_cancellation, version);
        sizing(value);
        payload.status = sizing.Finish();
        if (payload.status.Succeeded())
        {
            payload.bytes.resize(sizing.ByteSize());
            SceneArchive<false> archive({}, &payload.bytes, limits, budget, m_cancellation, version);
            archive(value);
            payload.status = archive.Finish();
            if (m_parts != nullptr && ResourcePart(type) != PartKind::Manifest && payload.status.Succeeded())
            {
                payload.record.canonicalChecksum = HashBytes(payload.bytes.data(), payload.bytes.size());
            }
        }
        return payload;
    }

    template <class T> void Encode(ResourceType type, uint32_t index, const T& value, uint32_t version = 1)
    {
        if (m_status.Succeeded())
        {
            Payload payload = Serialize(type, index, value, version, m_bufferBudget);
            payload.partWritten = payload.status.Succeeded() && m_parts != nullptr && ResourcePart(type) == PartKind::Geometry
                               && m_parts->ReuseGeometry(payload.record);
            if (!payload.partWritten && payload.status.Succeeded())
            {
                EncodedMeshResource encoded =
                    EncodeMeshResource(type, payload.bytes, m_options.compression.meshoptimizer,
                                       m_bufferBudget - payload.bytes.capacity(), m_encoderBudget, m_cancellation);
                payload.status = encoded.status;
                if (encoded.encoding != 0)
                {
                    payload.record.encoding = encoded.encoding;
                    payload.bytes           = std::move(encoded.bytes);
                }
            }
            if (!payload.partWritten)
            {
                Compress(payload, m_bufferBudget, m_encoderBudget);
            }
            Append(payload);
        }
    }

    void Compress(Payload& payload, uint64_t budget, uint64_t encoderBudget) const;
    void Append(Payload& payload);

    const Status& ResultStatus() const
    {
        return m_status;
    }
    Status Finish()
    {
        ZENASSET_MEASURE_SCOPE(Serialize);
        if (m_status.Succeeded() && m_parts != nullptr)
        {
            m_status = m_parts->Finish();
        }
        if (m_status.Succeeded() && !detail::SeekFile(m_file, detail::HeaderBytes))
        {
            m_status = {StatusCode::IOError, "Cannot seek to scene package directory"};
        }
        kernels::HashState hash{};
        kernels::ResetHash(hash, 0);
        for (const ResourceRecord& record : m_records)
        {
            std::array<uint8_t, PartDirectoryEntryBytes> entry{};
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
            if (m_parts != nullptr)
            {
                const PartKind kind   = ResourcePart(record.type);
                const uint64_t digest = kind == PartKind::Geometry ? m_parts->GeometryDigest()
                                      : kind == PartKind::Image    ? record.checksum
                                                                   : 0;
                detail::Write64(entry.data() + DirectoryEntryBytes, digest);
                detail::Write64(entry.data() + 72, record.canonicalChecksum);
                detail::Write64(entry.data() + 80, record.sourceImageChecksum);
            }
            kernels::UpdateHash(hash, entry.data(), m_entryBytes);
            if (m_status.Succeeded())
            {
                m_status = detail::WriteFileBytes(m_file, entry.data(), m_entryBytes, m_cancellation);
            }
        }
        std::array<uint8_t, detail::HeaderBytes> header{};
        std::copy(detail::Magic.begin(), detail::Magic.end(), header.begin());
        detail::Write32(header.data() + 8, PackageSchemaVersion);
        detail::Write32(header.data() + 12, detail::ByteOrder);
        detail::Write32(header.data() + 16, detail::HeaderBytes);
        detail::Write32(header.data() + 20, m_entryBytes);
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
    uint64_t                    m_resourceCount;
    DirectoryParts*             m_parts;
    uint32_t                    m_entryBytes;
    uint64_t                    m_position;
    uint64_t                    m_totalStored;
    uint64_t                    m_decodedBytes{0};
    uint64_t                    m_bufferBudget;
    uint64_t                    m_encoderBudget;
    std::set<uint64_t>          m_imageDigests;
    uint64_t                    m_geometryEnd{0};
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
