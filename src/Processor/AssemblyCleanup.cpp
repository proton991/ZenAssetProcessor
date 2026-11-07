#include "ZenAsset/Processor/Assembly.h"
#include "ZenAsset/Reader/Assembly.h"
#include "Processor/BuildInternal.h"
#include "Importers/AssetPaths.h"
#include "Importers/AssetHash.h"
#include "Format/FileIO.h"
#include <algorithm>
#include <chrono>
#include <set>
#if defined(_WIN32)
#    define NOMINMAX
#    include <Windows.h>
#endif

namespace zenasset
{
namespace
{
struct PathLess
{
    bool operator()(const std::filesystem::path& a, const std::filesystem::path& b) const
    {
#if defined(_WIN32)
        std::filesystem::path left  = a.lexically_normal();
        std::filesystem::path right = b.lexically_normal();
        left.make_preferred();
        right.make_preferred();
        const bool less = CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_LESS_THAN;
#else
        const bool less = a < b;
#endif
        return less;
    }
};
using PathSet = std::set<std::filesystem::path, PathLess>;

struct CleanupEntry
{
    std::filesystem::path           path;
    uint64_t                        bytes{0};
    std::filesystem::file_time_type modified;
    bool                            directory{false};
    bool                            operator==(const CleanupEntry&) const = default;
};

Status OrdinaryPath(const std::filesystem::path& path)
{
    Status                             status;
    std::error_code                    error;
    const std::filesystem::file_status state = std::filesystem::symlink_status(path, error);
    if (error || std::filesystem::is_symlink(state))
    {
        status = {StatusCode::IOError, "Cleanup rejects unreadable paths and symbolic links: " + PathToUtf8(path)};
    }
#if defined(_WIN32)
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (status.Succeeded() && (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0))
    {
        status = {StatusCode::InvalidArgument, "Cleanup rejects reparse points: " + PathToUtf8(path)};
    }
#endif
    return status;
}

Status RootPath(const std::string& text, std::filesystem::path& root)
{
    Status          status = detail::MakePath(text, root);
    std::error_code error;
    if (status.Succeeded())
    {
        root = std::filesystem::absolute(root, error);
        if (error)
        {
            status = {StatusCode::IOError, "Cannot resolve cleanup root"};
        }
    }
    std::filesystem::path prefix = root.root_path();
    for (const std::filesystem::path& part : root.relative_path())
    {
        prefix /= part;
        if (status.Succeeded())
        {
            status = OrdinaryPath(prefix);
        }
    }
    if (status.Succeeded())
    {
        root = std::filesystem::canonical(root, error);
        if (error || !std::filesystem::is_directory(root, error) || error)
        {
            status = {StatusCode::InvalidArgument, "Cleanup root must be an existing ordinary directory"};
        }
    }
    return status;
}

bool EntryLess(const CleanupEntry& a, const CleanupEntry& b)
{
    return a.path < b.path;
}
Status Inventory(const std::filesystem::path&  root,
                 const AssemblyCleanupOptions& options,
                 const Cancellation*           cancellation,
                 std::vector<CleanupEntry>&    entries)
{
    Status                                        status = OrdinaryPath(root);
    std::error_code                               error;
    std::filesystem::recursive_directory_iterator iterator;
    if (status.Succeeded())
    {
        iterator = std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::none, error);
        if (error)
        {
            status = {StatusCode::IOError, "Cannot enumerate cleanup root"};
        }
    }
    const std::filesystem::recursive_directory_iterator end;
    uint64_t                                            planned = 0;
    while (status.Succeeded() && iterator != end)
    {
        if (detail::Cancelled(cancellation))
        {
            status = {StatusCode::Cancelled, "Cleanup scan cancelled"};
        }
        CleanupEntry entry;
        entry.path = iterator->path();
        if (status.Succeeded())
        {
            status = OrdinaryPath(entry.path);
        }
        if (status.Succeeded())
        {
            entry.directory = iterator->is_directory(error);
            if (error)
            {
                status = {StatusCode::IOError, "Cannot inspect cleanup entry"};
            }
            else if (!entry.directory)
            {
                if (!iterator->is_regular_file(error) || error)
                {
                    status = {StatusCode::InvalidArgument, "Cleanup rejects special filesystem entries"};
                }
                if (status.Succeeded())
                {
                    entry.bytes = iterator->file_size(error);
                }
                if (!error && status.Succeeded())
                {
                    entry.modified = iterator->last_write_time(error);
                }
                if (error)
                {
                    status = {StatusCode::IOError, "Cannot inspect cleanup file"};
                }
            }
        }
        const uint64_t bytes = sizeof(CleanupEntry) + PathToUtf8(entry.path).size() * 4;
        // Two inventory snapshots plus path/reference/result records share the planning budget.
        if (status.Succeeded() && (entries.size() >= options.maxEntries || bytes > options.maxPlanningBytes / 8 - planned))
        {
            status = {StatusCode::ResourceLimit, "Cleanup inventory exceeds planning budget"};
        }
        if (status.Succeeded())
        {
            planned += bytes;
            entries.push_back(std::move(entry));
            iterator.increment(error);
            if (error)
            {
                status = {StatusCode::IOError, "Cannot continue cleanup enumeration"};
            }
        }
    }
    std::sort(entries.begin(), entries.end(), EntryLess);
    return status;
}

bool Extension(const std::filesystem::path& path, const char* wanted)
{
    std::string extension = PathToUtf8(path.extension());
    for (char& c : extension)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c + ('a' - 'A'));
        }
    }
    return extension == wanted;
}

bool Hex(const std::string& value)
{
    bool valid = value.size() == 16;
    for (char c : value)
    {
        valid = valid && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
    return valid;
}

bool IdentityName(const std::filesystem::path& path, const PackageIdentity& identity, const BuildMetadata& metadata)
{
    std::string       stem    = PathToUtf8(path.stem());
    const std::string suffix  = "-" + metadata.cacheKey + "-" + DigestText(identity.headerHash);
    bool              matches = Hex(metadata.cacheKey) && stem.ends_with(suffix);
    if (!matches && stem.size() > 17 && stem[stem.size() - 17] == '-' && Hex(stem.substr(stem.size() - 16)))
    {
        stem.resize(stem.size() - 17);
        matches = Hex(metadata.cacheKey) && stem.ends_with(suffix);
    }
    const std::string key   = matches ? stem.substr(0, stem.size() - suffix.size()) : std::string();
    bool              valid = matches && !key.empty() && key.size() <= 128 && !metadata.buildFingerprint.empty();
    for (unsigned char c : key)
    {
        valid = valid && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_');
    }
    return valid;
}

Status ProtectSource(const std::string& text, PathSet& protectedPaths, uint64_t& planned, uint64_t limit)
{
    Status status;
    if (!text.empty())
    {
        const std::filesystem::path path = std::filesystem::u8path(text).lexically_normal();
        if (path.is_absolute() && !protectedPaths.contains(path))
        {
            const uint64_t bytes = sizeof(std::filesystem::path) + text.size() * 4 + 128;
            if (planned > limit || bytes > limit - planned)
            {
                status = {StatusCode::ResourceLimit, "Cleanup source provenance exceeds planning budget"};
            }
            else
            {
                protectedPaths.insert(path);
                planned += bytes;
            }
        }
    }
    return status;
}
Status ProtectSources(const SceneMetadataResult& metadata, PathSet& protectedPaths, uint64_t& planned, uint64_t limit)
{
    Status status = ProtectSource(metadata.sourcePath, protectedPaths, planned, limit);
    for (const SceneDependency& input : metadata.metadata.inputs)
    {
        if (status.Succeeded())
        {
            status = ProtectSource(input.path, protectedPaths, planned, limit);
        }
    }
    return status;
}

struct CleanupCandidate
{
    std::filesystem::path path;
    PackageIdentity       identity;
    uint64_t              bytes;
};

struct RetainedAssembly
{
    std::filesystem::path path;
    PackageIdentity       identity;
};

Status Retained(const std::vector<CleanupEntry>& entries,
                const AssemblyCleanupOptions&    options,
                const Cancellation*              cancellation,
                PathSet&                         references,
                std::vector<RetainedAssembly>&   manifests,
                PathSet&                         protectedPaths,
                uint64_t&                        sourcePlanning)
{
    Status          status;
    uint64_t        planning  = 0;
    SceneReadLimits limits    = options.limits;
    limits.maxAllocationBytes = std::min(limits.maxAllocationBytes, options.maxPlanningBytes / 4);
    for (const CleanupEntry& entry : entries)
    {
        if (status.Succeeded() && !entry.directory && Extension(entry.path, ".zenscene"))
        {
            const PackageResult package = OpenPackage(PathToUtf8(entry.path), limits.package, cancellation);
            status                      = package.status;
            AssemblyReadResult assembly;
            if (status.Succeeded())
            {
                assembly = ReadAssembly(*package.package, limits, cancellation);
                status   = assembly.status;
            }
            if (status.Succeeded())
            {
                status = VerifyAssemblyDependencies(PathToUtf8(entry.path), *assembly.assembly, limits.package, cancellation);
            }
            if (status.Succeeded())
            {
                for (const AssemblyModel& model : assembly.assembly->models)
                {
                    if (status.Succeeded())
                    {
                        const std::filesystem::path path  = entry.path.parent_path() / std::filesystem::u8path(model.path);
                        const uint64_t              bytes = sizeof(std::filesystem::path) + PathToUtf8(path).size() * 4 + 128;
                        if (bytes > options.maxPlanningBytes / 8 - planning)
                        {
                            status = {StatusCode::ResourceLimit, "Cleanup reference budget exceeded"};
                        }
                        else if (!references.contains(path))
                        {
                            const PackageResult dependency = OpenPackage(PathToUtf8(path), limits.package, cancellation);
                            status                         = dependency.status;
                            if (status.Succeeded())
                            {
                                status = dependency.package->ValidatePayloads(cancellation);
                            }
                            if (status.Succeeded())
                            {
                                const SceneMetadataResult metadata =
                                    ReadSceneMetadata(*dependency.package, limits, cancellation);
                                status = metadata.status;
                                if (status.Succeeded())
                                {
                                    status =
                                        ProtectSources(metadata, protectedPaths, sourcePlanning, options.maxPlanningBytes / 8);
                                }
                            }
                            if (status.Succeeded())
                            {
                                references.insert(path);
                                planning += bytes;
                            }
                        }
                    }
                }
                if (status.Succeeded())
                {
                    manifests.push_back({entry.path, package.package->Identity()});
                }
            }
        }
    }
    if (status.Succeeded() && manifests.empty())
    {
        status = {StatusCode::InvalidArgument, "Cleanup requires at least one valid retained assembly"};
    }
    return status;
}

Status Unchanged(const std::filesystem::path&         root,
                 const AssemblyCleanupOptions&        options,
                 const Cancellation*                  cancellation,
                 const std::vector<CleanupEntry>&     expected,
                 const std::vector<RetainedAssembly>& manifests)
{
    std::vector<CleanupEntry> current;
    Status                    status = Inventory(root, options, cancellation, current);
    if (status.Succeeded() && current != expected)
    {
        status = {StatusCode::SourceUnavailable, "Cleanup directory changed during verification"};
    }
    for (const RetainedAssembly& manifest : manifests)
    {
        if (status.Succeeded())
        {
            const PackageResult package = OpenPackage(PathToUtf8(manifest.path), options.limits.package, cancellation);
            status                      = package.status;
            if (status.Succeeded() && package.package->Identity() != manifest.identity)
            {
                status = {StatusCode::SourceUnavailable, "Retained assembly changed during cleanup"};
            }
        }
    }
    return status;
}

AssemblyCleanupResult Clean(const std::string& text, const AssemblyCleanupOptions& options, const Cancellation* cancellation)
{
    AssemblyCleanupResult result;
    result.dryRun = options.dryRun;
    std::filesystem::path root;
    result.status = RootPath(text, root);
    if (result.status.Succeeded()
        && (options.maxEntries == 0 || options.maxEntries > 1000000 || options.maxPlanningBytes < 16384))
    {
        result.status = {StatusCode::ResourceLimit, "Invalid cleanup planning limits"};
    }
    std::vector<CleanupEntry>     entries;
    PathSet                       references;
    PathSet                       protectedPaths;
    uint64_t                      sourcePlanning = 0;
    std::vector<CleanupCandidate> candidates;
    std::vector<RetainedAssembly> manifests;
    if (result.status.Succeeded())
    {
        result.status = Inventory(root, options, cancellation, entries);
    }
    if (result.status.Succeeded())
    {
        result.status = Retained(entries, options, cancellation, references, manifests, protectedPaths, sourcePlanning);
    }
    SceneReadLimits limits    = options.limits;
    limits.maxAllocationBytes = std::min(limits.maxAllocationBytes, options.maxPlanningBytes / 4);
    for (const CleanupEntry& entry : entries)
    {
        if (result.status.Succeeded() && !entry.directory && entry.path.parent_path() == root / "models"
            && Extension(entry.path, ".zenasset") && !references.contains(entry.path))
        {
            const PackageResult package = OpenPackage(PathToUtf8(entry.path), limits.package, cancellation);
            if (package.status.Succeeded())
            {
                const SceneMetadataResult metadata = ReadSceneMetadata(*package.package, limits, cancellation);
                if (metadata.status.code == StatusCode::Cancelled)
                {
                    result.status = metadata.status;
                }
                if (metadata.status.Succeeded())
                {
                    result.status = ProtectSources(metadata, protectedPaths, sourcePlanning, options.maxPlanningBytes / 8);
                }
                if (result.status.Succeeded() && metadata.status.Succeeded()
                    && IdentityName(entry.path, package.package->Identity(), metadata.metadata))
                {
                    result.status = package.package->ValidatePayloads(cancellation);
                    if (result.status.Succeeded())
                    {
                        candidates.push_back({entry.path, package.package->Identity(), entry.bytes});
                    }
                }
            }
            else if (package.status.code == StatusCode::Cancelled)
            {
                result.status = package.status;
            }
            // Unknown or damaged unreferenced files cannot prove ownership; retain them.
        }
    }
    if (result.status.Succeeded())
    {
        result.status = Unchanged(root, options, cancellation, entries, manifests);
    }
    if (result.status.Succeeded())
    {
        for (const CleanupCandidate& candidate : candidates)
        {
            if (!protectedPaths.contains(candidate.path))
            {
                result.candidates.push_back(PathToUtf8(candidate.path));
                result.candidateBytes += candidate.bytes;
            }
        }
    }
    if (result.status.Succeeded() && !options.dryRun)
    {
        for (const std::string& path : result.candidates)
        {
            if (result.status.Succeeded())
            {
                result.status = Unchanged(root, options, cancellation, entries, manifests);
            }
            if (result.status.Succeeded())
            {
                const std::filesystem::path candidate = std::filesystem::u8path(path);
                PackageResult               current   = OpenPackage(path, limits.package, cancellation);
                result.status                         = current.status;
                for (const CleanupCandidate& expected : candidates)
                {
                    if (expected.path == candidate && result.status.Succeeded()
                        && current.package->Identity() != expected.identity)
                    {
                        result.status = {StatusCode::SourceUnavailable, "Cleanup candidate identity changed"};
                    }
                }
                if (result.status.Succeeded())
                {
                    result.status = current.package->ValidatePayloads(cancellation);
                }
                current.package.reset();
                std::error_code error;
                if (result.status.Succeeded() && (!std::filesystem::remove(candidate, error) || error))
                {
                    result.status = {StatusCode::IOError, "Cannot remove unreferenced model package"};
                }
                else if (result.status.Succeeded())
                {
                    result.deleted.push_back(path);
                    std::vector<CleanupEntry>::iterator found = entries.begin();
                    while (found != entries.end() && found->path != candidate)
                    {
                        ++found;
                    }
                    if (found != entries.end())
                    {
                        entries.erase(found);
                    }
                }
            }
        }
    }
    return result;
}
} // namespace

AssemblyCleanupResult CleanupAssemblies(const std::string&            root,
                                        const AssemblyCleanupOptions& options,
                                        const Cancellation*           cancellation)
{
    AssemblyCleanupResult result;
    bool                  acquired = false;
    while (!acquired && !detail::Cancelled(cancellation))
    {
        acquired = detail::AssemblyBuildGate().try_lock_for(std::chrono::milliseconds(25));
    }
    if (acquired)
    {
        result = Clean(root, options, cancellation);
        detail::AssemblyBuildGate().unlock();
    }
    else
    {
        result.status = {StatusCode::Cancelled, "Cleanup cancelled while waiting for assembly build"};
    }
    return result;
}
} // namespace zenasset
