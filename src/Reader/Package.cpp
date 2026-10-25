#include "ZenAsset/Reader/Package.h"
#include "Diagnostics/StageMeasurements.h"
#include "Format/PackageLayout.h"
#include "Format/FileIO.h"
#include "Reader/Transport.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <mutex>

namespace zenasset
{
namespace
{
bool IsCancelled(const Cancellation* cancellation)
{
    return cancellation != nullptr && cancellation->IsCancelled();
}

bool KnownType(ResourceType type)
{
    return type >= ResourceType::Scene && type <= ResourceType::SourceDocument;
}

ResourceRecord DecodeEntry(const uint8_t* bytes)
{
    ResourceRecord result;
    result.id           = detail::Read64(bytes);
    result.type         = static_cast<ResourceType>(detail::Read32(bytes + 8));
    result.version      = detail::Read32(bytes + 12);
    result.flags        = detail::Read32(bytes + 16);
    result.codec        = static_cast<TransportCodec>(detail::Read32(bytes + 20));
    result.offset       = detail::Read64(bytes + 24);
    result.storedBytes  = detail::Read64(bytes + 32);
    result.decodedBytes = detail::Read64(bytes + 40);
    result.checksum     = detail::Read64(bytes + 48);
    result.alignment    = detail::Read32(bytes + 56);
    result.encoding     = detail::Read32(bytes + 60);
    return result;
}

Status CheckEntry(const ResourceRecord& record,
                  uint64_t              previousId,
                  uint64_t              previousEnd,
                  uint64_t              fileBytes,
                  uint64_t              remainingDecoded,
                  const ReadLimits&     limits)
{
    Status     status;
    const bool required = (record.flags & RequiredResource) != 0;
    const bool aligned  = record.alignment != 0 && record.alignment <= 4096 && (record.alignment & (record.alignment - 1)) == 0
                       && record.offset % record.alignment == 0;
    if (record.id <= previousId || (record.flags & ~RequiredResource) != 0 || !aligned || record.offset < previousEnd
        || record.offset > fileBytes || record.storedBytes > fileBytes - record.offset)
    {
        status = {StatusCode::InvalidData, "Invalid package resource ID, flags, alignment, or byte range"};
    }
    else if (record.decodedBytes > remainingDecoded || record.decodedBytes > limits.maxChunkBytes
             || record.storedBytes > limits.maxChunkBytes || record.decodedBytes > std::numeric_limits<size_t>::max())
    {
        status = {StatusCode::ResourceLimit, "Package decoded data exceeds the configured limits"};
    }
    else if (required && (!KnownType(record.type) || !detail::SupportedResourceVersion(record.type, record.version)))
    {
        status = {StatusCode::UnsupportedVersion, "Required package resource type or version is unsupported"};
    }
    else if (required && record.codec != TransportCodec::None && record.codec != TransportCodec::Zstandard)
    {
        status = {StatusCode::UnsupportedCodec, "Required package transport codec is unsupported"};
    }
    else if (record.codec == TransportCodec::None && record.storedBytes != record.decodedBytes)
    {
        status = {StatusCode::InvalidData, "Raw package resource lengths disagree"};
    }
    return status;
}
} // namespace

struct Package::State
{
    FILE*                       file{nullptr};
    std::vector<ResourceRecord> resources;
    uint64_t                    maxCodecBytes{0};
    mutable std::mutex          mutex;

    ~State()
    {
        if (file != nullptr)
        {
            ZENASSET_MEASURE_SCOPE(FileClose);
            std::fclose(file);
        }
    }
};

Package::Package(std::unique_ptr<State> state) : m_state(std::move(state)) {}
Package::~Package() = default;

std::span<const ResourceRecord> Package::Resources() const
{
    return m_state->resources;
}

const ResourceRecord* Package::Find(uint64_t id) const
{
    const ResourceRecord*                             result = nullptr;
    const std::vector<ResourceRecord>::const_iterator found =
        std::lower_bound(m_state->resources.begin(), m_state->resources.end(), id,
                         [](const ResourceRecord& record, uint64_t value) { return record.id < value; });
    if (found != m_state->resources.end() && found->id == id)
    {
        result = &*found;
    }
    return result;
}

ResourceData Package::Read(uint64_t id, const Cancellation* cancellation, uint64_t maxScratchBytes) const
{
    ZENASSET_MEASURE_SCOPE(PackageRead);
    ResourceData          result;
    const ResourceRecord* record = Find(id);
    if (IsCancelled(cancellation))
    {
        result.status = {StatusCode::Cancelled, "Package read cancelled"};
    }
    else if (record == nullptr)
    {
        result.status = {StatusCode::InvalidArgument, "Package resource ID was not found"};
    }
    else if (!KnownType(record->type) || !detail::SupportedResourceVersion(record->type, record->version))
    {
        result.status = {StatusCode::UnsupportedVersion, "Package resource type or version is unsupported"};
    }
    else if (record->codec != TransportCodec::None && record->codec != TransportCodec::Zstandard)
    {
        result.status = {StatusCode::UnsupportedCodec, "Package transport codec is unsupported"};
    }
    else
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        if (IsCancelled(cancellation))
        {
            result.status = {StatusCode::Cancelled, "Package read cancelled"};
        }
        else if (!detail::SeekFile(m_state->file, record->offset))
        {
            result.status = {StatusCode::IOError, "Could not seek to package resource"};
        }
        else
        {
            result.bytes.resize(static_cast<size_t>(record->decodedBytes));
            if (record->codec == TransportCodec::Zstandard)
            {
                result.status = detail::ReadZstandardResource(m_state->file, *record, result.bytes,
                                                              std::min(maxScratchBytes, m_state->maxCodecBytes), cancellation);
            }
            else
            {
                result.status = detail::ReadFileBytes(m_state->file, result.bytes.data(), result.bytes.size(), cancellation);
                ZENASSET_MEASURE_BEGIN(checksum, Checksum);
                if (result.status.Succeeded()
                    && detail::HashBytes(result.bytes.data(), result.bytes.size()) != record->checksum)
                {
                    result.status = {StatusCode::InvalidData, "Package resource checksum does not match"};
                }
                ZENASSET_MEASURE_END(checksum);
            }
        }
    }
    if (!result.status.Succeeded())
    {
        std::vector<std::byte>().swap(result.bytes);
    }
    return result;
}

Status Package::ValidatePayloads(const Cancellation* cancellation) const
{
    Status status = IsCancelled(cancellation) ? Status{StatusCode::Cancelled, "Package validation cancelled"} : Status{};
    std::array<uint8_t, 65536>  buffer{};
    std::lock_guard<std::mutex> lock(m_state->mutex);
    for (size_t index = 0; status.Succeeded() && index < m_state->resources.size(); ++index)
    {
        const ResourceRecord& record = m_state->resources[index];
        kernels::HashState    hash{};
        kernels::ResetHash(hash, 0);
        if (IsCancelled(cancellation))
        {
            status = {StatusCode::Cancelled, "Package validation cancelled"};
        }
        else if (!detail::SeekFile(m_state->file, record.offset))
        {
            status = {StatusCode::IOError, "Could not seek to package resource"};
        }
        uint64_t remaining = record.storedBytes;
        while (status.Succeeded() && remaining > 0)
        {
            const size_t count = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
            status             = detail::ReadFileBytes(m_state->file, buffer.data(), count, cancellation);
            if (status.Succeeded())
            {
                kernels::UpdateHash(hash, buffer.data(), count);
                remaining -= count;
            }
        }
        if (status.Succeeded() && kernels::FinishHash(hash) != record.checksum)
        {
            status = {StatusCode::InvalidData, "Package resource checksum does not match"};
        }
    }
    return status;
}

PackageResult OpenPackage(const std::string& path, const ReadLimits& limits, const Cancellation* cancellation)
{
    ZENASSET_MEASURE_SCOPE(PackageOpen);
    PackageResult                   result;
    std::unique_ptr<Package::State> state(new Package::State);
    state->maxCodecBytes = limits.maxCodecBytes;
    std::filesystem::path filePath;
    uint64_t              fileBytes = 0;
    if (IsCancelled(cancellation))
    {
        result.status = {StatusCode::Cancelled, "Package open cancelled"};
    }
    else
    {
        result.status = detail::MakePath(path, filePath);
    }
    if (result.status.Succeeded())
    {
        const detail::FileOpenResult opened = detail::OpenFile(filePath);
        result.status                       = opened.status;
        state->file                         = opened.file;
    }
    if (result.status.Succeeded())
    {
        if (!detail::FileLength(state->file, fileBytes))
        {
            result.status = {StatusCode::IOError, "Could not determine package file size"};
        }
        else if (fileBytes < detail::HeaderBytes)
        {
            result.status = {StatusCode::InvalidData, "Package header is truncated"};
        }
        else if (fileBytes > limits.maxFileBytes)
        {
            result.status = {StatusCode::ResourceLimit, "Package file exceeds the configured size limit"};
        }
    }

    std::array<uint8_t, detail::HeaderBytes> header{};
    if (result.status.Succeeded())
    {
        result.status = detail::ReadFileBytes(state->file, header.data(), header.size(), cancellation);
    }
    const uint64_t resourceCount = detail::Read64(header.data() + 32);
    if (result.status.Succeeded())
    {
        if (!std::equal(detail::Magic.begin(), detail::Magic.end(), header.begin())
            || detail::Read32(header.data() + 12) != detail::ByteOrder
            || detail::Read32(header.data() + 16) != detail::HeaderBytes
            || detail::Read32(header.data() + 20) != detail::DirectoryEntryBytes
            || detail::Read64(header.data() + 24) != detail::HeaderBytes || detail::Read64(header.data() + 40) != fileBytes
            || detail::HashBytes(header.data(), 56) != detail::Read64(header.data() + 56))
        {
            result.status = {StatusCode::InvalidData, "Package header, size, or checksum is invalid"};
        }
        else if (detail::Read32(header.data() + 8) != PackageSchemaVersion)
        {
            result.status = {StatusCode::UnsupportedVersion, "Package schema version is unsupported"};
        }
        else if (resourceCount > limits.maxResources || resourceCount > limits.maxDirectoryBytes / sizeof(ResourceRecord)
                 || resourceCount > std::numeric_limits<size_t>::max() / sizeof(ResourceRecord))
        {
            result.status = {StatusCode::ResourceLimit, "Package directory exceeds the resource limit"};
        }
        else if (resourceCount > (fileBytes - detail::HeaderBytes) / detail::DirectoryEntryBytes)
        {
            result.status = {StatusCode::InvalidData, "Package directory is truncated"};
        }
    }
    if (result.status.Succeeded())
    {
        state->resources.reserve(static_cast<size_t>(resourceCount));
        kernels::HashState directoryHash{};
        kernels::ResetHash(directoryHash, 0);
        uint64_t previousId       = 0;
        uint64_t previousEnd      = detail::HeaderBytes + resourceCount * detail::DirectoryEntryBytes;
        uint64_t remainingDecoded = limits.maxDecodedBytes;
        for (uint64_t index = 0; result.status.Succeeded() && index < resourceCount; ++index)
        {
            std::array<uint8_t, detail::DirectoryEntryBytes> entry{};
            result.status = detail::ReadFileBytes(state->file, entry.data(), entry.size(), cancellation);
            if (result.status.Succeeded())
            {
                kernels::UpdateHash(directoryHash, entry.data(), entry.size());
                const ResourceRecord record = DecodeEntry(entry.data());
                result.status               = CheckEntry(record, previousId, previousEnd, fileBytes, remainingDecoded, limits);
                if (result.status.Succeeded())
                {
                    state->resources.push_back(record);
                    previousId        = record.id;
                    previousEnd       = record.offset + record.storedBytes;
                    remainingDecoded -= record.decodedBytes;
                }
            }
        }
        if (result.status.Succeeded()
            && (kernels::FinishHash(directoryHash) != detail::Read64(header.data() + 48) || previousEnd != fileBytes))
        {
            result.status = {StatusCode::InvalidData, "Package directory checksum or final extent is invalid"};
        }
    }
    if (result.status.Succeeded())
    {
        result.package = std::shared_ptr<const Package>(new Package(std::move(state)));
    }
    return result;
}
} // namespace zenasset
