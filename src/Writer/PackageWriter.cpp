#include "Writer/PackageWriter.h"
#include <atomic>
#include <chrono>
#include <filesystem>

namespace zenasset::detail
{
namespace
{
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

void ResourceEncoder::Compress(Payload& payload, uint64_t budget, uint64_t encoderBudget) const
{
    if (payload.status.Succeeded())
    {
        payload.record.decodedBytes   = payload.bytes.size();
        CompressedResource compressed = CompressResource(payload.bytes, m_options.compression,
                                                         budget - payload.bytes.capacity(), encoderBudget, m_cancellation);
        payload.status                = std::move(compressed.status);
        if (compressed.codec != TransportCodec::None)
        {
            payload.record.codec = compressed.codec;
            payload.bytes        = std::move(compressed.bytes);
        }
        if (payload.status.Succeeded())
        {
            ZENASSET_MEASURE_SCOPE(Checksum);
            payload.record.storedBytes = payload.bytes.size();
            payload.record.checksum    = HashBytes(payload.bytes.data(), payload.bytes.size());
        }
    }
}

void ResourceEncoder::Append(Payload& payload)
{
    m_status                = std::move(payload.status);
    ResourceRecord& record  = payload.record;
    const uint64_t  padding = (16 - m_position % 16) % 16;
    if (m_status.Succeeded()
        && (padding > m_options.limits.package.maxFileBytes - m_position
            || record.storedBytes > m_options.limits.package.maxFileBytes - m_position - padding
            || record.decodedBytes > m_options.limits.package.maxDecodedBytes - m_decodedBytes))
    {
        m_status = {StatusCode::ResourceLimit, "Scene package exceeds its configured byte limits"};
    }
    if (m_status.Succeeded())
    {
        const uint8_t zeros[16]{};
        m_status = WriteFileBytes(m_file, zeros, padding, m_cancellation);
    }
    if (m_status.Succeeded())
    {
        record.offset = m_position + padding;
        m_status      = WriteFileBytes(m_file, payload.bytes.data(), payload.bytes.size(), m_cancellation);
    }
    if (m_status.Succeeded())
    {
        m_position      = record.offset + record.storedBytes;
        m_decodedBytes += record.decodedBytes;
        m_records.push_back(record);
        ZENASSET_MEASURE_ADD(TransportEncodedResources, record.codec != TransportCodec::None ? 1 : 0);
        ZENASSET_MEASURE_ADD(TransportEncodeAttempts,
                             m_options.compression.transport == TransportCodec::Zstandard && record.decodedBytes >= 64 ? 1 : 0);
        if (m_options.resourceProgress != nullptr)
        {
            m_options.resourceProgress(m_options.progressUser, m_records.size(), m_resourceCount);
        }
    }
}

Status WritePackage(const std::string&       path,
                    uint64_t                 count,
                    const SceneWriteOptions& options,
                    const Cancellation*      cancellation,
                    PackageWriterCallback    encode,
                    void*                    user)
{
    std::filesystem::path destination;
    Status                status = MakePath(path, destination);
    if (options.published != nullptr)
    {
        *options.published = false;
    }
    const uint64_t prefixBytes = HeaderBytes + count * DirectoryEntryBytes;
    if (count > options.limits.package.maxResources || count > options.limits.package.maxDirectoryBytes / sizeof(ResourceRecord)
        || count > options.limits.maxAllocationBytes / sizeof(ResourceRecord) || count > SIZE_MAX / sizeof(ResourceRecord)
        || count > (UINT64_MAX - HeaderBytes) / DirectoryEntryBytes || prefixBytes > options.limits.package.maxFileBytes)
    {
        status = {StatusCode::ResourceLimit, "Package directory exceeds configured limits"};
    }
    if (status.Succeeded() && cancellation != nullptr && cancellation->IsCancelled())
    {
        status = {StatusCode::Cancelled, "Package write cancelled"};
    }
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
            status = encode(file, user);
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
} // namespace zenasset::detail
