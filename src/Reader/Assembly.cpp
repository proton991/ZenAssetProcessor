#include "ZenAsset/Reader/Assembly.h"
#include "Format/AssemblySchema.h"
#include "Format/SceneArchive.h"
#include "Format/SceneResources.h"
#include "Format/FileIO.h"
#include <filesystem>

namespace zenasset
{
AssemblyReadResult ReadAssembly(const Package& package, const SceneReadLimits& limits, const Cancellation* cancellation)
{
    AssemblyReadResult    result;
    const ResourceRecord* record    = package.Find(detail::ResourceId(ResourceType::Assembly));
    uint64_t              remaining = limits.maxAllocationBytes;
    const uint64_t        overhead  = sizeof(SceneAssembly) + sizeof(ResourceRecord);
    if (cancellation != nullptr && cancellation->IsCancelled())
    {
        result.status = {StatusCode::Cancelled, "Assembly read cancelled"};
    }
    else if (package.Resources().size() != 1 || record == nullptr || record->type != ResourceType::Assembly
             || record->version != 1 || record->encoding != 0 || record->flags != RequiredResource)
    {
        result.status = {StatusCode::InvalidData, "Expected one required native assembly resource"};
    }
    else if (package.Identity().fileBytes > limits.package.maxFileBytes || limits.package.maxResources < 1
             || limits.package.maxDirectoryBytes < sizeof(ResourceRecord)
             || record->decodedBytes > limits.package.maxDecodedBytes || record->decodedBytes > limits.package.maxChunkBytes
             || record->storedBytes > limits.package.maxChunkBytes || overhead > remaining
             || record->decodedBytes > remaining - overhead)
    {
        result.status = {StatusCode::ResourceLimit, "Assembly resource exceeds configured limits"};
    }
    if (result.status.Succeeded())
    {
        remaining               -= overhead + record->decodedBytes;
        const ResourceData data  = package.Read(record->id, cancellation, std::min(remaining, limits.package.maxCodecBytes));
        result.status            = data.status;
        if (result.status.Succeeded())
        {
            std::shared_ptr<SceneAssembly> assembly = std::make_shared<SceneAssembly>();
            detail::SceneArchive<true>     archive(data.bytes, nullptr, limits, remaining, cancellation);
            archive(*assembly);
            result.status = archive.Finish();
            if (result.status.Succeeded() && !detail::AssemblyValidationFits(*assembly, remaining))
            {
                result.status = {StatusCode::ResourceLimit, "Assembly validation exceeds allocation budget"};
            }
            if (result.status.Succeeded())
            {
                result.status = ValidateAssembly(*assembly);
            }
            if (result.status.Succeeded() && cancellation != nullptr && cancellation->IsCancelled())
            {
                result.status = {StatusCode::Cancelled, "Assembly read cancelled"};
            }
            if (result.status.Succeeded())
            {
                result.assembly = std::move(assembly);
            }
        }
    }
    return result;
}

AssemblyReadResult ReadAssembly(const std::string& path, const SceneReadLimits& limits, const Cancellation* cancellation)
{
    AssemblyReadResult  result;
    const PackageResult package = OpenPackage(path, limits.package, cancellation);
    result.status               = package.status;
    if (result.status.Succeeded())
    {
        result = ReadAssembly(*package.package, limits, cancellation);
    }
    return result;
}

Status VerifyAssemblyDependencies(const std::string&   path,
                                  const SceneAssembly& assembly,
                                  const ReadLimits&    limits,
                                  const Cancellation*  cancellation)
{
    Status                status = ValidateAssembly(assembly);
    std::filesystem::path manifest;
    if (status.Succeeded())
    {
        status = detail::MakePath(path, manifest);
    }
    std::error_code       error;
    std::filesystem::path directory;
    if (status.Succeeded())
    {
        const std::filesystem::path absolute = std::filesystem::absolute(manifest, error);
        if (!error)
        {
            directory = std::filesystem::weakly_canonical(absolute.parent_path(), error);
        }
        if (error)
        {
            status = {StatusCode::IOError, "Cannot resolve assembly directory"};
        }
    }
    for (const AssemblyModel& model : assembly.models)
    {
        if (status.Succeeded() && cancellation != nullptr && cancellation->IsCancelled())
        {
            status = {StatusCode::Cancelled, "Assembly dependency verification cancelled"};
        }
        if (status.Succeeded())
        {
            std::filesystem::path relative;
            status = detail::MakePath(model.path, relative);
            std::filesystem::path resolved;
            if (status.Succeeded())
            {
                resolved = std::filesystem::weakly_canonical(directory / relative, error);
                if (error)
                {
                    status = {StatusCode::IOError, "Cannot resolve assembly model path"};
                }
                else
                {
                    std::filesystem::path::const_iterator child = resolved.begin();
                    for (std::filesystem::path::const_iterator parent = directory.begin(); parent != directory.end(); ++parent)
                    {
                        if (child == resolved.end() || *child != *parent)
                        {
                            status = {StatusCode::InvalidData, "Assembly dependency escapes its directory"};
                            break;
                        }
                        ++child;
                    }
                }
            }
            if (status.Succeeded())
            {
                const std::u8string utf8       = resolved.u8string();
                const PackageResult dependency = OpenPackage(std::string(utf8.begin(), utf8.end()), limits, cancellation);
                status                         = dependency.status;
                if (status.Succeeded()
                    && (dependency.package->Identity() != model.identity
                        || dependency.package->Find(detail::ResourceId(ResourceType::Scene)) == nullptr))
                {
                    status = {StatusCode::InvalidData, "Assembly model package identity or scene resource does not match"};
                }
            }
        }
    }
    return status;
}
} // namespace zenasset
