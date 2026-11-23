#pragma once
#include "ZenAsset/Format/Status.h"
#include <cstdio>
#include <filesystem>
#include <string_view>

namespace zenasset::detail
{
struct FileOpenResult
{
    Status status;
    // Ownership transfers to the caller, which closes it with fclose.
    FILE* file{nullptr};
};

class CacheFileLock
{
public:
    CacheFileLock()                                = default;
    CacheFileLock(const CacheFileLock&)            = delete;
    CacheFileLock& operator=(const CacheFileLock&) = delete;
    ~CacheFileLock();
    Status Acquire(const std::filesystem::path& path, bool exclusive, bool create, bool wait, const Cancellation* cancellation);

private:
    intptr_t m_handle{-1};
};
Status         MakePath(std::string_view utf8, std::filesystem::path& path);
FileOpenResult OpenFile(const std::filesystem::path& path, bool exclusiveWrite = false);
FileOpenResult OpenStoredPart(const std::filesystem::path& path, StatusCode pathError = StatusCode::IOError);
bool           FileLength(FILE* file, uint64_t& bytes);
bool           SeekFile(FILE* file, uint64_t position);
Status ReadFileBytes(FILE* file, void* data, uint64_t count, const Cancellation* cancellation, TransferProgress progress = {});
Status VerifyFileBytes(FILE* file, uint64_t size, uint64_t digest, const Cancellation* cancellation);
Status WriteFileBytes(FILE*               file,
                      const void*         data,
                      uint64_t            count,
                      const Cancellation* cancellation,
                      TransferProgress    progress = {});
Status         FlushFile(FILE* file, bool durable);
// Both paths must be on the same volume. No cross-volume copy fallback is used.
Status ReplaceFileAtomically(const std::filesystem::path& temporary,
                             const std::filesystem::path& destination,
                             bool                         durable,
                             bool*                        published = nullptr);
} // namespace zenasset::detail
