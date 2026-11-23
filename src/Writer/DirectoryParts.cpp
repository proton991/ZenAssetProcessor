#include "Writer/DirectoryParts.h"
#include <charconv>

namespace zenasset::detail
{
// Part paths are rooted in the canonical model directory. A staged descriptor
// must enforce the same containment rule as the published package reader.
FileOpenResult OpenStoredPart(const std::filesystem::path& path)
{
    FileOpenResult              result;
    std::error_code             error;
    const std::filesystem::path resolved = std::filesystem::canonical(path, error);
    if (error == std::errc::no_such_file_or_directory)
    {
        result.status = {StatusCode::SourceUnavailable, "Scene part does not exist"};
    }
    else if (error || resolved != path)
    {
        result.status = {StatusCode::IOError, "Cannot open missing or redirected scene part"};
    }
    else
    {
        result = OpenFile(resolved);
    }
    return result;
}
Status CheckStoredFile(const std::filesystem::path& path, uint64_t size, uint64_t digest, const Cancellation* cancellation)
{
    FileOpenResult opened = OpenStoredPart(path);
    if (opened.file != nullptr)
    {
        uint64_t actual = 0;
        if (!FileLength(opened.file, actual) || actual != size)
        {
            opened.status = {StatusCode::InvalidData, "Part length changed"};
        }
        if (opened.status.Succeeded())
        {
            opened.status = VerifyFileBytes(opened.file, size, digest, cancellation);
        }
        std::fclose(opened.file);
    }
    return opened.status;
}
namespace
{
Status ClosePart(FILE*& file, bool durable)
{
    Status    status = FlushFile(file, durable);
    const int closed = std::fclose(file);
    file             = nullptr;
    if (closed != 0 && status.Succeeded())
    {
        status = {StatusCode::IOError, "Could not close scene part"};
    }
    return status;
}
void RemoveTemporary(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::remove(path, error);
}
Status PublishPart(const std::filesystem::path& temporary,
                   const std::filesystem::path& path,
                   uint64_t                     size,
                   uint64_t                     digest,
                   bool                         durable,
                   const Cancellation*          cancellation)
{
    Status status = CheckStoredFile(temporary, size, digest, cancellation);
    if (status.Succeeded())
    {
        status = ReplaceFileAtomically(temporary, path, durable);
    }
    return status;
}
} // namespace
DirectoryParts::DirectoryParts(const std::filesystem::path& root, bool durable, const Cancellation* cancellation) :
    m_root(root), m_durable(durable), m_cancellation(cancellation)
{
    kernels::ResetHash(m_hash, 0);
}
DirectoryParts::~DirectoryParts()
{
    if (m_geometry != nullptr)
    {
        std::fclose(m_geometry);
    }
    if (!m_temporary.empty())
    {
        RemoveTemporary(m_temporary);
    }
}
Status DirectoryParts::Begin(const std::filesystem::path& manifest, const SceneWriteOptions& options)
{
    m_stats        = options.partStats;
    m_progress     = options.partProgress;
    m_progressUser = options.progressUser;
    m_transfer     = {this, m_progress ? &DirectoryParts::ReportBytes : nullptr};
    std::error_code folderError;
    std::filesystem::create_directories(m_root, folderError);
    Status        status = folderError ? Status{StatusCode::IOError, "Cannot create scene directory"}
                                       : m_publication.Acquire(m_root / ".zen-publish.lock", true, true, true, m_cancellation);
    CacheFileLock lease;
    if (status.Succeeded())
    {
        status = lease.Acquire(m_root / ".zen-reader.lock", false, true, true, m_cancellation);
    }
    const std::u8string utf8 = manifest.generic_u8string();
    const PackageResult old  = OpenPackage(std::string(utf8.begin(), utf8.end()), options.limits.package, m_cancellation);
    if (old.status.Succeeded() && !old.package->Parts().empty())
    {
        const SceneMetadataResult metadata = ReadSceneMetadata(*old.package, options.limits, m_cancellation);
        if (metadata.status.Succeeded()
            && metadata.metadata.buildFingerprint
                   == (options.metadata.buildFingerprint.empty() ? LibraryBuildFingerprint()
                                                                 : std::string_view(options.metadata.buildFingerprint))
            && metadata.metadata.compression.transport == options.compression.transport
            && metadata.metadata.compression.meshoptimizer == options.compression.meshoptimizer
            && metadata.metadata.compression.zstandardLevel == options.compression.zstandardLevel)
        {
            m_previous = old.package;
            for (const PackagePart& part : m_previous->Parts())
            {
                if (part.relativePath.starts_with("geometry/mesh-"))
                {
                    uint64_t                     digest = 0;
                    const char*                  first  = part.relativePath.data() + sizeof("geometry/mesh-") - 1;
                    const std::from_chars_result parsed = std::from_chars(first, first + 16, digest, 16);
                    if (parsed.ec == std::errc{}
                        && CheckStoredFile(m_root / part.relativePath, part.storedBytes, digest, m_cancellation).Succeeded())
                    {
                        m_oldGeometry = m_root / part.relativePath;
                        m_oldDigest   = digest;
                        m_oldSize     = part.storedBytes;
                    }
                }
            }
        }
    }
    const std::filesystem::path root = std::filesystem::canonical(m_root, folderError);
    if (!folderError)
    {
        m_root = root;
    }
    for (const char* folder : {"geometry", "textures"})
    {
        if (!status.Succeeded())
        {
            break;
        }
        const std::filesystem::path path = root / folder;
        std::error_code             error;
        std::filesystem::create_directories(path, error);
        if (folderError || error || std::filesystem::canonical(path, error) != path || error)
        {
            status = {StatusCode::IOError, "Cannot create contained scene part directory"};
        }
    }
    return status;
}
Status DirectoryParts::StartGeometry()
{
    Status status;
    if (m_geometry == nullptr)
    {
        FileOpenResult opened = OpenTemporary(m_root / "geometry" / "mesh.zenpart", m_temporary);
        status                = opened.status;
        m_geometry            = opened.file;
        if (!status.Succeeded())
        {
            m_temporary.clear();
        }
        else if (m_position > 0)
        {
            FileOpenResult previous = OpenFile(m_oldGeometry);
            status                  = previous.status;
            std::array<uint8_t, 65536> bytes{};
            uint64_t                   remaining = m_position;
            while (status.Succeeded() && remaining > 0)
            {
                const size_t count = static_cast<size_t>(std::min<uint64_t>(remaining, bytes.size()));
                status             = ReadFileBytes(previous.file, bytes.data(), count, m_cancellation);
                if (status.Succeeded())
                {
                    status = WriteFileBytes(m_geometry, bytes.data(), count, m_cancellation, m_transfer);
                    kernels::UpdateHash(m_hash, bytes.data(), count);
                }
                remaining -= count;
            }
            if (previous.file != nullptr)
            {
                std::fclose(previous.file);
            }
        }
    }
    return status;
}
Status DirectoryParts::Geometry(ResourceRecord& record, std::span<const std::byte> bytes)
{
    const size_t  padding = (16 - m_position % 16) % 16;
    const uint8_t zeros[16]{};
    Status        status = StartGeometry();
    if (status.Succeeded())
    {
        status = WriteFileBytes(m_geometry, zeros, padding, m_cancellation, m_transfer);
    }
    if (status.Succeeded())
    {
        status = WriteFileBytes(m_geometry, bytes.data(), bytes.size(), m_cancellation, m_transfer);
        kernels::UpdateHash(m_hash, zeros, padding);
        kernels::UpdateHash(m_hash, bytes.data(), bytes.size());
        record.offset = m_position + padding;
        m_position    = record.offset + bytes.size();
    }
    return status;
}
bool DirectoryParts::ReuseGeometry(ResourceRecord& record)
{
    const ResourceRecord* prior = m_previous ? m_previous->Find(record.id) : nullptr;
    const bool            reuse = !m_oldGeometry.empty() && !m_geometry && prior && prior->version == record.version
                               && prior->offset == m_position + (16 - m_position % 16) % 16
                               && prior->canonicalChecksum == record.canonicalChecksum;
    if (reuse)
    {
        record     = *prior;
        m_position = record.offset + record.storedBytes;
    }
    return reuse;
}
bool DirectoryParts::ReuseImage(ResourceRecord& record) const
{
    const ResourceRecord* prior = m_previous ? m_previous->Find(record.id) : nullptr;
    const bool reuse = prior && prior->canonicalChecksum == record.canonicalChecksum && prior->version == record.version
                    && CheckStoredFile(m_root / PartName(PartKind::Image, prior->checksum), prior->storedBytes, prior->checksum,
                                       m_cancellation)
                           .Succeeded();
    if (reuse)
    {
        const uint64_t source      = record.sourceImageChecksum;
        record                     = *prior;
        record.sourceImageChecksum = source;
    }
    return reuse;
}
Status DirectoryParts::Image(const ResourceRecord& record, std::span<const std::byte> bytes) const
{
    const std::filesystem::path path   = m_root / PartName(PartKind::Image, record.checksum);
    Status                      status = CheckStoredFile(path, bytes.size(), record.checksum, m_cancellation);
    if (status.code == StatusCode::SourceUnavailable || status.code == StatusCode::InvalidData)
    {
        std::filesystem::path temporary;
        FileOpenResult        opened = OpenTemporary(path, temporary);
        status                       = opened.status;
        if (opened.file != nullptr)
        {
            status = WriteFileBytes(opened.file, bytes.data(), bytes.size(), m_cancellation, m_transfer);
            m_written.fetch_add(bytes.size(), std::memory_order_relaxed);
            Status closed = ClosePart(opened.file, m_durable);
            if (status.Succeeded())
            {
                status = closed;
            }
            if (status.Succeeded())
            {
                status = PublishPart(temporary, path, bytes.size(), record.checksum, m_durable, m_cancellation);
            }
            RemoveTemporary(temporary);
        }
    }
    return status;
}
void DirectoryParts::SetImageCount(uint64_t count)
{
    m_state.totalImages = count;
}
void DirectoryParts::ReportBytes(void* user, uint64_t bytes)
{
    DirectoryParts&             self = *static_cast<DirectoryParts*>(user);
    std::lock_guard<std::mutex> lock(self.m_progressMutex);
    self.m_state.completedBytes += bytes;
    if (bytes >= (1u << 20))
    {
        self.m_progress(self.m_progressUser, self.m_state);
    }
}
void DirectoryParts::ReportJob(bool geometry) const
{
    if (m_progress)
    {
        std::lock_guard<std::mutex> lock(m_progressMutex);
        if (geometry)
        {
            m_state.geometryComplete = true;
        }
        else
        {
            ++m_state.completedImages;
        }
        m_progress(m_progressUser, m_state);
    }
}
void DirectoryParts::CountImage(bool reused) const
{
    (reused ? m_reused : m_encoded).fetch_add(1, std::memory_order_relaxed);
    ReportJob(false);
}
uint64_t DirectoryParts::GeometryDigest() const
{
    return !m_geometry && m_temporary.empty() && m_position == m_oldSize ? m_oldDigest : kernels::FinishHash(m_hash);
}
ScenePartWriteStats DirectoryParts::ImageStats() const
{
    return {m_encoded.load(), m_reused.load(), m_written.load(), false};
}
Status DirectoryParts::Finish()
{
    const bool reused = !m_oldGeometry.empty() && !m_geometry && m_position == m_oldSize;
    Status     status;
    if (!reused)
    {
        status = StartGeometry();
    }
    const uint64_t              digest = GeometryDigest();
    const std::filesystem::path path   = m_root / PartName(PartKind::Geometry, digest);
    if (status.Succeeded() && !reused)
    {
        status = ClosePart(m_geometry, m_durable);
    }
    if (status.Succeeded() && !reused)
    {
        status = CheckStoredFile(path, m_position, digest, m_cancellation);
        if (status.code == StatusCode::SourceUnavailable || status.code == StatusCode::InvalidData)
        {
            status = PublishPart(m_temporary, path, m_position, digest, m_durable, m_cancellation);
        }
    }
    if (status.Succeeded())
    {
        ReportJob(true);
    }
    if (m_stats && status.Succeeded())
    {
        *m_stats                   = ImageStats();
        m_stats->partBytesWritten += reused ? 0 : m_position;
        m_stats->geometryReused    = reused;
    }
    return status;
}
} // namespace zenasset::detail
