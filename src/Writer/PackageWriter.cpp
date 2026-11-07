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
