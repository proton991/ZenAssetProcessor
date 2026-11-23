#pragma once
#include "Format/FileIO.h"
#include <atomic>
#include <mutex>
#include "ZenAsset/Processor/Write.h"
#include "Format/PackageLayout.h"

namespace zenasset::detail
{
FileOpenResult OpenStoredPart(const std::filesystem::path& path);
Status CheckStoredFile(const std::filesystem::path& path, uint64_t size, uint64_t digest, const Cancellation* cancellation);
FileOpenResult OpenTemporary(const std::filesystem::path& destination, std::filesystem::path& temporary);

class DirectoryParts
{
public:
    DirectoryParts(const std::filesystem::path& root, bool durable, const Cancellation* cancellation);
    ~DirectoryParts();
    ScenePartWriteStats          ImageStats() const;
    Status   Begin(const std::filesystem::path& manifest, const SceneWriteOptions& options);
    bool     ReuseGeometry(ResourceRecord& record);
    bool     ReuseImage(ResourceRecord& record) const;
    Status   Image(const ResourceRecord& record, std::span<const std::byte> bytes) const;
    Status   Geometry(ResourceRecord& record, std::span<const std::byte> bytes);
    Status   Finish();
    uint64_t GeometryDigest() const;
    const std::filesystem::path& Root() const
    {
        return m_root;
    }
    void     CountImage(bool reused) const;
    void     SetImageCount(uint64_t count);

private:
    static void                    ReportBytes(void* user, uint64_t bytes);
    void                           ReportJob(bool geometry) const;
    void                           (*m_progress)(void*, const SceneTransferState&){nullptr};
    void*                          m_progressUser{nullptr};
    mutable std::mutex             m_progressMutex;
    mutable SceneTransferState     m_state;
    TransferProgress               m_transfer;
    CacheFileLock                  m_publication;
    std::filesystem::path          m_root;
    Status                         StartGeometry();
    uint64_t                       m_oldSize{0};
    uint64_t                       m_oldDigest{0};
    std::filesystem::path          m_oldGeometry;
    mutable std::atomic<uint64_t>  m_encoded{0}, m_reused{0}, m_written{0};
    ScenePartWriteStats*           m_stats{nullptr};
    bool                           m_durable;
    const Cancellation*            m_cancellation;
    std::filesystem::path          m_temporary;
    FILE*                          m_geometry{nullptr};
    uint64_t                       m_position{0};
    kernels::HashState             m_hash{};
    std::shared_ptr<const Package> m_previous;
};
} // namespace zenasset::detail
