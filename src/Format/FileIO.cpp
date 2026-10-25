#include "Format/FileIO.h"
#include "Diagnostics/StageMeasurements.h"
#include "ZenAsset/Format/Utf8.h"
#include <algorithm>
#include <cerrno>
#include <limits>
#include <type_traits>
#include <cstring>
#include <vector>
#include <sys/stat.h>
#if defined(_WIN32)
#    define NOMINMAX
#    include <Windows.h>
#    include <fcntl.h>
#    include <io.h>
#else
#    include <fcntl.h>
#    include <unistd.h>
#endif

namespace zenasset::detail
{
Status MakePath(std::string_view utf8, std::filesystem::path& path)
{
    Status status;
    if (utf8.empty() || utf8.find('\0') != std::string_view::npos || !IsValidUtf8(utf8))
    {
        status = {StatusCode::InvalidArgument, "A nonempty UTF-8 path without null bytes is required"};
    }
    else
    {
        path = std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
    }
    return status;
}

FileOpenResult OpenFile(const std::filesystem::path& path, bool exclusiveWrite)
{
    ZENASSET_MEASURE_SCOPE(FileOpen);
    FileOpenResult result;
#if defined(_WIN32)
    // Readers retain the old file when an atomic publication replaces its name.
    // Denying writes prevents mutation of the retained file; delete sharing
    // permits same-directory replacement without invalidating existing views.
    const HANDLE handle = CreateFileW(path.c_str(), exclusiveWrite ? GENERIC_WRITE : GENERIC_READ,
                                      exclusiveWrite ? 0 : FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                      exclusiveWrite ? CREATE_NEW : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE)
    {
        const int descriptor =
            _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_BINARY | (exclusiveWrite ? _O_WRONLY : _O_RDONLY));
        if (descriptor == -1)
        {
            CloseHandle(handle);
        }
        else
        {
            result.file = _fdopen(descriptor, exclusiveWrite ? "wb" : "rb");
            if (result.file == nullptr)
            {
                _close(descriptor);
            }
        }
    }
    else if (!exclusiveWrite && (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND))
    {
        result.status = {StatusCode::SourceUnavailable, "Package file is unavailable"};
    }
#else
    const int descriptor = open(path.c_str(), exclusiveWrite ? O_WRONLY | O_CREAT | O_EXCL : O_RDONLY, 0666);
    if (descriptor >= 0)
    {
        result.file = fdopen(descriptor, exclusiveWrite ? "wb" : "rb");
        if (result.file == nullptr)
        {
            close(descriptor);
        }
    }
    else if (!exclusiveWrite && (errno == ENOENT || errno == ENOTDIR))
    {
        result.status = {StatusCode::SourceUnavailable, "Package file is unavailable"};
    }
#endif
    if (result.file == nullptr && result.status.Succeeded())
    {
        result.status = {StatusCode::IOError,
                         exclusiveWrite ? "Could not create package temporary file" : "Could not open package file"};
    }
    return result;
}

bool FileLength(FILE* file, uint64_t& bytes)
{
    ZENASSET_MEASURE_SCOPE(FileInspect);
#if defined(_WIN32)
    struct _stat64 info{};
    const bool     valid = _fstat64(_fileno(file), &info) == 0 && (info.st_mode & _S_IFMT) == _S_IFREG && info.st_size >= 0;
#else
    struct stat info{};
    const bool  valid = fstat(fileno(file), &info) == 0 && S_ISREG(info.st_mode) && info.st_size >= 0;
#endif
    bytes = valid ? static_cast<uint64_t>(info.st_size) : 0;
    return valid;
}

bool SeekFile(FILE* file, uint64_t position)
{
    ZENASSET_MEASURE_SCOPE(FileSeek);
    bool success = position <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    if (success)
    {
#if defined(_WIN32)
        success = _fseeki64(file, static_cast<int64_t>(position), SEEK_SET) == 0;
#else
        success = fseeko(file, static_cast<off_t>(position), SEEK_SET) == 0;
#endif
    }
    return success;
}

template <bool Reading> Status TransferFileBytes(FILE*                                                file,
                                                 std::conditional_t<Reading, uint8_t, const uint8_t>* data,
                                                 uint64_t                                             count,
                                                 const Cancellation*                                  cancellation)
{
    Status status;
    while (status.Succeeded() && count > 0)
    {
        const size_t transfer = static_cast<size_t>(std::min<uint64_t>(count, 4ull << 20));
        if (cancellation != nullptr && cancellation->IsCancelled())
        {
            status = {StatusCode::Cancelled, "Package I/O cancelled"};
        }
        else
        {
            size_t completed = 0;
            if constexpr (Reading)
            {
                completed = std::fread(data, 1, transfer, file);
            }
            else
            {
                completed = std::fwrite(data, 1, transfer, file);
            }
            if constexpr (Reading)
            {
                ZENASSET_MEASURE_ADD(ReadBytes, completed);
            }
            else
            {
                ZENASSET_MEASURE_ADD(WrittenBytes, completed);
            }
            if (completed != transfer)
            {
                status = {StatusCode::IOError,
                          Reading ? "Could not read complete package data" : "Could not write complete package data"};
            }
            else
            {
                data  += transfer;
                count -= transfer;
            }
        }
    }
    return status;
}

Status ReadFileBytes(FILE* file, void* data, uint64_t count, const Cancellation* cancellation)
{
    ZENASSET_MEASURE_SCOPE(FileRead);
    return TransferFileBytes<true>(file, static_cast<uint8_t*>(data), count, cancellation);
}

Status WriteFileBytes(FILE* file, const void* data, uint64_t count, const Cancellation* cancellation)
{
    ZENASSET_MEASURE_SCOPE(FileWrite);
    return TransferFileBytes<false>(file, static_cast<const uint8_t*>(data), count, cancellation);
}

Status FlushFile(FILE* file, bool durable)
{
    ZENASSET_MEASURE_SCOPE(FileFlush);
    bool success = std::fflush(file) == 0;
    if (success && durable)
    {
#if defined(_WIN32)
        success = _commit(_fileno(file)) == 0;
#else
        success = fsync(fileno(file)) == 0;
#endif
    }
    return success ? Status{} : Status{StatusCode::IOError, "Could not flush package file"};
}

Status ReplaceFileAtomically(const std::filesystem::path& temporary,
                             const std::filesystem::path& destination,
                             bool                         durable,
                             bool*                        published)
{
    ZENASSET_MEASURE_SCOPE(Publish);
    Status status;
#if defined(_WIN32)
    std::error_code             pathError;
    const std::filesystem::path fullDestination = std::filesystem::absolute(destination, pathError);
    const size_t                pathBytes       = fullDestination.native().size() * sizeof(wchar_t);
    HANDLE                      handle          = INVALID_HANDLE_VALUE;
    if (pathError || pathBytes > 65534)
    {
        status = {StatusCode::InvalidArgument, "Package destination is not a supported absolute path"};
    }
    else
    {
        handle = CreateFileW(temporary.c_str(), DELETE | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | (durable ? FILE_FLAG_WRITE_THROUGH : 0), nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            status = {StatusCode::IOError, "Could not open package temporary file for publication"};
        }
    }
    if (status.Succeeded())
    {
        const size_t          storageBytes = sizeof(FILE_RENAME_INFO) + pathBytes;
        std::vector<uint64_t> storage((storageBytes + sizeof(uint64_t) - 1) / sizeof(uint64_t), 0);
        static_assert(alignof(FILE_RENAME_INFO) <= alignof(uint64_t));
        FILE_RENAME_INFO* information = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
        information->Flags            = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
        information->FileNameLength   = static_cast<DWORD>(pathBytes);
        std::memcpy(information->FileName, fullDestination.c_str(), pathBytes);
        if (!SetFileInformationByHandle(handle, FileRenameInfoEx, information, static_cast<DWORD>(storageBytes)))
        {
            status = {StatusCode::IOError, "Could not atomically replace package destination (Windows error "
                                               + std::to_string(GetLastError()) + ")"};
        }
        else
        {
            if (published != nullptr)
            {
                *published = true;
            }
            if (durable && !FlushFileBuffers(handle))
            {
                status = {StatusCode::IOError, "Package published, but file durability could not be confirmed"};
            }
        }
    }
    if (handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(handle);
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error)
    {
        status = {StatusCode::IOError, "Could not atomically replace package destination"};
    }
    else
    {
        if (published != nullptr)
        {
            *published = true;
        }
        if (durable)
        {
            const std::filesystem::path directory  = destination.has_parent_path() ? destination.parent_path() : ".";
            const int                   descriptor = open(directory.c_str(), O_RDONLY);
            bool                        flushed    = descriptor >= 0;
            if (flushed)
            {
                flushed = fsync(descriptor) == 0;
                flushed = close(descriptor) == 0 && flushed;
            }
            if (!flushed)
            {
                status = {StatusCode::IOError, "Package published, but directory durability could not be confirmed"};
            }
        }
    }
#endif
    return status;
}
} // namespace zenasset::detail
