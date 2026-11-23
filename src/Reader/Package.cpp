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
#include <map>

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
    return type >= ResourceType::Scene && type <= ResourceType::Assembly;
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

struct ResourceFile
{
    FILE*              file{nullptr};
    uint64_t           nextOffset{UINT64_MAX};
    uint64_t           length{0};
    uint64_t           lastEnd{0};
    mutable std::mutex mutex;
    // With the cursor lock held, consume alignment padding from stdio's read
    // buffer. Seeking for every small adjacent record would discard read-ahead.
    Status BeginRead(uint64_t offset, const Cancellation* cancellation)
    {
        Status  status;
        uint8_t padding[15];
        if (nextOffset <= offset && offset - nextOffset <= sizeof(padding))
        {
            status = detail::ReadFileBytes(file, padding, offset - nextOffset, cancellation);
        }
        else if (!detail::SeekFile(file, offset))
        {
            status = {StatusCode::IOError, "Could not seek to package resource"};
        }
        nextOffset = UINT64_MAX;
        return status;
    }
    ~ResourceFile()
    {
        if (file != nullptr)
        {
            ZENASSET_MEASURE_SCOPE(FileClose);
            std::fclose(file);
        }
    }
};

struct Package::State
{
    detail::CacheFileLock                      lease;
    ResourceFile                               manifest;
    std::vector<std::unique_ptr<ResourceFile>> files;
    std::vector<PackagePart>                   parts;
    std::vector<size_t>                        locations;
    std::vector<ResourceRecord>                resources;
    size_t                                     geometry{SIZE_MAX};
    uint64_t                                   geometryDigest{0};
    uint64_t                                   maxCodecBytes{0};
    uint64_t                                   largestChunk{0};
    uint64_t                                   decodedBytes{0};
    uint64_t                                   directoryBytes{0};
    PackageIdentity                            identity;
    ResourceFile&                              File(size_t index)
    {
        return locations.empty() || locations[index] == SIZE_MAX ? manifest : *files[locations[index]];
    }
    Status OpenPart(const std::filesystem::path&   root,
                    detail::PartKind               kind,
                    uint64_t                       digest,
                    const ReadLimits&              limits,
                    std::map<std::string, size_t>& indices,
                    uint64_t&                      fileBytes,
                    size_t&                        location)
    {
        Status            status;
        const std::string name     = detail::PartName(kind, digest);
        const auto        existing = indices.find(name);
        if (existing != indices.end())
        {
            location = existing->second;
        }
        else if (kind == detail::PartKind::Geometry && geometry != SIZE_MAX)
        {
            status = {StatusCode::InvalidData, "Multiple geometry parts in one manifest"};
        }
        else if (sizeof(ResourceFile) + sizeof(PackagePart) + sizeof(size_t) + name.size()
                 > limits.maxDirectoryBytes - directoryBytes)
        {
            status = {StatusCode::ResourceLimit, "Scene part descriptors exceed directory budget"};
        }
        else
        {
            directoryBytes += sizeof(ResourceFile) + sizeof(PackagePart) + sizeof(size_t) + name.size();
            std::error_code             error;
            const std::filesystem::path resolved = std::filesystem::canonical(root / name, error);
            if (error || resolved != root / name)
            {
                status = {StatusCode::InvalidData, "Missing or redirected scene part: " + name};
            }
            else
            {
                std::unique_ptr<ResourceFile> part   = std::make_unique<ResourceFile>();
                detail::FileOpenResult        opened = detail::OpenFile(resolved);
                part->file                           = opened.file;
                status                               = opened.status;
                if (status.Succeeded() && !detail::FileLength(opened.file, part->length))
                {
                    status = {StatusCode::IOError, "Cannot size scene part"};
                }
                if (status.Succeeded() && part->length > limits.maxFileBytes - fileBytes)
                {
                    status = {StatusCode::ResourceLimit, "Scene parts exceed total file limit"};
                }
                if (status.Succeeded())
                {
                    location = parts.size();
                    indices.emplace(name, location);
                    fileBytes += part->length;
                    if (kind == detail::PartKind::Geometry)
                    {
                        geometry       = location;
                        geometryDigest = digest;
                    }
                    parts.push_back({name, part->length});
                    files.push_back(std::move(part));
                }
                else
                {
                    status.message += ": " + name;
                }
            }
        }
        return status;
    }
};

Package::Package(std::unique_ptr<State> state) : m_state(std::move(state)) {}
Package::~Package() = default;

PackageIdentity Package::Identity() const
{
    return m_state->identity;
}

std::span<const ResourceRecord> Package::Resources() const
{
    return m_state->resources;
}

std::span<const PackagePart> Package::Parts() const
{
    return m_state->parts;
}

uint64_t Package::DirectoryBytes() const
{
    return m_state->directoryBytes;
}

bool Package::FitsReadLimits(const ReadLimits& limits) const
{
    return m_state->identity.fileBytes <= limits.maxFileBytes && m_state->decodedBytes <= limits.maxDecodedBytes
        && m_state->largestChunk <= limits.maxChunkBytes && m_state->resources.size() <= limits.maxResources
        && m_state->directoryBytes <= limits.maxDirectoryBytes;
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

std::string_view Package::PartPath(uint64_t id) const
{
    std::string_view      path;
    const ResourceRecord* record = Find(id);
    if (record && !m_state->locations.empty())
    {
        const size_t location = m_state->locations[static_cast<size_t>(record - m_state->resources.data())];
        if (location != SIZE_MAX)
        {
            path = m_state->parts[location].relativePath;
        }
    }
    return path;
}

ResourceData Package::Read(uint64_t            id,
                           const Cancellation* cancellation,
                           uint64_t            maxScratchBytes,
                           TransferProgress    progress) const
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
        ResourceFile&               source = m_state->File(static_cast<size_t>(record - m_state->resources.data()));
        std::lock_guard<std::mutex> lock(source.mutex);
        if (IsCancelled(cancellation))
        {
            result.status = {StatusCode::Cancelled, "Package read cancelled"};
        }
        else
        {
            result.status = source.BeginRead(record->offset, cancellation);
        }
        if (result.status.Succeeded())
        {
            result = detail::ReadResourceData(source.file, *record, std::min(maxScratchBytes, m_state->maxCodecBytes),
                                              cancellation, progress);
            if (result.status.Succeeded())
            {
                source.nextOffset = record->offset + record->storedBytes;
            }
        }
    }
    if (!result.status.Succeeded())
    {
        if (record != nullptr && !m_state->locations.empty())
        {
            const size_t location = m_state->locations[static_cast<size_t>(record - m_state->resources.data())];
            if (location != SIZE_MAX)
            {
                result.status.message += " in " + m_state->parts[location].relativePath;
            }
        }
        std::vector<std::byte>().swap(result.bytes);
    }
    return result;
}

ResourceData Package::ReadStored(uint64_t id, uint64_t maxBytes, const Cancellation* cancellation) const
{
    ResourceData          result;
    const ResourceRecord* record = Find(id);
    if (record == nullptr)
    {
        result.status = {StatusCode::InvalidArgument, "Missing package resource"};
    }
    else if (record->storedBytes > maxBytes || record->storedBytes > SIZE_MAX)
    {
        result.status = {StatusCode::ResourceLimit, "Stored resource exceeds transfer budget"};
    }
    else
    {
        ResourceFile&               source = m_state->File(static_cast<size_t>(record - m_state->resources.data()));
        std::lock_guard<std::mutex> lock(source.mutex);
        result.status = source.BeginRead(record->offset, cancellation);
        if (result.status.Succeeded())
        {
            result.bytes.resize(static_cast<size_t>(record->storedBytes));
            result.status = detail::ReadFileBytes(source.file, result.bytes.data(), result.bytes.size(), cancellation);
            if (result.status.Succeeded() && detail::HashBytes(result.bytes.data(), result.bytes.size()) != record->checksum)
            {
                result.status = {StatusCode::InvalidData, "Stored resource checksum mismatch"};
            }
        }
    }
    return result;
}

Status Package::ValidateGeometryPart(const Cancellation* cancellation) const
{
    Status status;
    if (m_state->geometry != SIZE_MAX)
    {
        ResourceFile&               source = *m_state->files[m_state->geometry];
        std::lock_guard<std::mutex> lock(source.mutex);
        status = source.BeginRead(0, cancellation);
        if (status.Succeeded())
        {
            status = detail::VerifyFileBytes(source.file, source.length, m_state->geometryDigest, cancellation);
        }
        if (!status.Succeeded())
        {
            status.message += " in " + m_state->parts[m_state->geometry].relativePath;
        }
    }
    return status;
}

Status Package::ValidatePayloads(const Cancellation* cancellation) const
{
    Status status = IsCancelled(cancellation) ? Status{StatusCode::Cancelled, "Package validation cancelled"} : Status{};
    for (size_t index = 0; status.Succeeded() && index < m_state->resources.size(); ++index)
    {
        const ResourceRecord& record = m_state->resources[index];
        ResourceFile&               source = m_state->File(index);
        std::lock_guard<std::mutex> lock(source.mutex);
        status = source.BeginRead(record.offset, cancellation);
        if (status.Succeeded())
        {
            status = detail::VerifyFileBytes(source.file, record.storedBytes, record.checksum, cancellation);
        }
        if (status.Succeeded())
        {
            source.nextOffset = record.offset + record.storedBytes;
        }
        else if (!m_state->locations.empty() && m_state->locations[index] != SIZE_MAX)
        {
            status.message += " in " + m_state->parts[m_state->locations[index]].relativePath;
        }
    }
    if (status.Succeeded())
    {
        status = ValidateGeometryPart(cancellation);
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
        result.status = state->lease.Acquire(filePath.parent_path() / ".zen-reader.lock", false, false, true, cancellation);
    }
    if (result.status.Succeeded())
    {
        const detail::FileOpenResult opened = detail::OpenFile(filePath);
        result.status                       = opened.status;
        state->manifest.file                = opened.file;
    }
    if (result.status.Succeeded())
    {
        if (!detail::FileLength(state->manifest.file, fileBytes))
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
        result.status = detail::ReadFileBytes(state->manifest.file, header.data(), header.size(), cancellation);
    }
    const uint64_t resourceCount = detail::Read64(header.data() + 32);
    const uint32_t entryBytes    = detail::Read32(header.data() + 20);
    const bool     directory     = entryBytes == detail::PartDirectoryEntryBytes;
    const uint64_t recordCost =
        directory ? std::max<uint64_t>(sizeof(ResourceRecord) + sizeof(size_t), entryBytes) : sizeof(ResourceRecord);
    if (result.status.Succeeded())
    {
        if (!std::equal(detail::Magic.begin(), detail::Magic.end(), header.begin())
            || detail::Read32(header.data() + 12) != detail::ByteOrder
            || detail::Read32(header.data() + 16) != detail::HeaderBytes
            || (!directory && entryBytes != detail::DirectoryEntryBytes)
            || detail::Read64(header.data() + 24) != detail::HeaderBytes || detail::Read64(header.data() + 40) != fileBytes
            || detail::HashBytes(header.data(), 56) != detail::Read64(header.data() + 56))
        {
            result.status = {StatusCode::InvalidData, "Package header, size, or checksum is invalid"};
        }
        else if (detail::Read32(header.data() + 8) != PackageSchemaVersion)
        {
            result.status = {StatusCode::UnsupportedVersion, "Package schema version is unsupported"};
        }
        else if (resourceCount > limits.maxResources || resourceCount > limits.maxDirectoryBytes / recordCost
                 || resourceCount > std::numeric_limits<size_t>::max() / sizeof(ResourceRecord))
        {
            result.status = {StatusCode::ResourceLimit, "Package directory exceeds the resource limit"};
        }
        else if (resourceCount > (fileBytes - detail::HeaderBytes) / entryBytes)
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
        state->manifest.length    = fileBytes;
        state->manifest.lastEnd   = detail::HeaderBytes + resourceCount * entryBytes;
        state->directoryBytes     = resourceCount * recordCost;
        std::map<std::string, size_t> partIndices;
        std::filesystem::path         root;
        if (directory)
        {
            state->locations.reserve(static_cast<size_t>(resourceCount));
            std::error_code error;
            root = std::filesystem::canonical(filePath.has_parent_path() ? filePath.parent_path() : ".", error);
            if (error)
            {
                result.status = {StatusCode::IOError, "Cannot resolve manifest directory"};
            }
        }
        uint64_t remainingDecoded = limits.maxDecodedBytes;
        for (uint64_t index = 0; result.status.Succeeded() && index < resourceCount; ++index)
        {
            std::array<uint8_t, detail::PartDirectoryEntryBytes> entry{};
            result.status = detail::ReadFileBytes(state->manifest.file, entry.data(), entryBytes, cancellation);
            if (result.status.Succeeded())
            {
                kernels::UpdateHash(directoryHash, entry.data(), entryBytes);
                ResourceRecord record = DecodeEntry(entry.data());
                if (directory)
                {
                    record.canonicalChecksum   = detail::Read64(entry.data() + 72);
                    record.sourceImageChecksum = detail::Read64(entry.data() + 80);
                }
                ResourceFile* backing  = &state->manifest;
                size_t        location = SIZE_MAX;
                if (directory)
                {
                    const detail::PartKind kind   = detail::ResourcePart(record.type);
                    const uint64_t         digest = detail::Read64(entry.data() + detail::DirectoryEntryBytes);
                    if ((kind == detail::PartKind::Manifest && digest != 0)
                        || (kind == detail::PartKind::Image && (digest != record.checksum || record.offset != 0)))
                    {
                        result.status = {StatusCode::InvalidData, "Invalid directory part identity"};
                    }
                    else if (kind != detail::PartKind::Manifest)
                    {
                        result.status = state->OpenPart(root, kind, digest, limits, partIndices, fileBytes, location);
                        if (result.status.Succeeded())
                        {
                            backing = state->files[location].get();
                        }
                    }
                    state->locations.push_back(location);
                }
                if (result.status.Succeeded())
                {
                    const bool image = directory && record.type == ResourceType::Image;
                    result.status =
                        CheckEntry(record, previousId, image ? 0 : backing->lastEnd, backing->length, remainingDecoded, limits);
                    if (image && record.storedBytes != backing->length)
                    {
                        result.status = {StatusCode::InvalidData,
                                         "Image part extent mismatch: " + state->parts[location].relativePath};
                    }
                    else if (!result.status.Succeeded() && location != SIZE_MAX)
                    {
                        result.status.message += " in " + state->parts[location].relativePath;
                    }
                }
                if (result.status.Succeeded())
                {
                    state->resources.push_back(record);
                    state->largestChunk  = std::max({state->largestChunk, record.storedBytes, record.decodedBytes});
                    previousId           = record.id;
                    backing->lastEnd      = record.offset + record.storedBytes;
                    remainingDecoded    -= record.decodedBytes;
                }
            }
        }
        for (size_t index = 0; result.status.Succeeded() && index < state->files.size(); ++index)
        {
            if (state->files[index]->lastEnd != state->files[index]->length)
            {
                result.status = {StatusCode::InvalidData, "Scene part extent mismatch: " + state->parts[index].relativePath};
            }
        }
        state->decodedBytes = limits.maxDecodedBytes - remainingDecoded;
        state->manifest.nextOffset = detail::HeaderBytes + resourceCount * entryBytes;
        if (result.status.Succeeded()
            && (kernels::FinishHash(directoryHash) != detail::Read64(header.data() + 48)
                || state->manifest.lastEnd != state->manifest.length))
        {
            result.status = {StatusCode::InvalidData, "Package directory checksum or final extent is invalid"};
        }
    }
    if (result.status.Succeeded())
    {
        state->identity = {fileBytes, detail::Read64(header.data() + 56)};
        result.package  = std::shared_ptr<const Package>(new Package(std::move(state)));
    }
    return result;
}
} // namespace zenasset
