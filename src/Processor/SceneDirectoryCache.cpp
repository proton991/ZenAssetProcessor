#include "Processor/BuildInternal.h"
#include "Importers/AssetHash.h"
#include "Writer/DirectoryParts.h"
#include "ZenFoundation/Templates/HeapVector.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>

namespace zenasset
{
namespace
{
std::string Utf8Path(const std::filesystem::path& path)
{
    const std::u8string utf8 = path.generic_u8string();
    return std::string(utf8.begin(), utf8.end());
}
std::string ModelStem(const std::filesystem::path& source)
{
    std::string stem = Utf8Path(source.stem());
    for (char& ch : stem)
    {
        if (static_cast<unsigned char>(ch) < 32 || std::string_view("<>:\"/\\|?*").find(ch) != std::string_view::npos)
        {
            ch = '_';
        }
    }
    while (!stem.empty() && (stem.back() == '.' || stem.back() == ' '))
    {
        stem.pop_back();
    }
    std::string base = stem.substr(0, stem.find('.'));
    std::transform(base.begin(), base.end(), base.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    if (base.empty() || base == "CON" || base == "PRN" || base == "AUX" || base == "NUL"
        || (base.size() == 4 && (base.starts_with("COM") || base.starts_with("LPT"))
            && std::isdigit(static_cast<unsigned char>(base.back()))))
    {
        stem.insert(0, "_");
    }
    return stem;
}
std::string OwnerJson(const std::string& identity)
{
    std::ostringstream text;
    text << "{\"source\":\"";
    for (unsigned char ch : identity)
    {
        if (ch < 32)
        {
            text << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << unsigned(ch);
        }
        else
        {
            if (ch == '"' || ch == '\\')
            {
                text << '\\';
            }
            text << static_cast<char>(ch);
        }
    }
    text << "\"}\n";
    return text.str();
}
bool MatchesOwner(const std::filesystem::path& path, const std::string& owner)
{
    std::error_code error;
    bool            matches = std::filesystem::file_size(path, error) == owner.size() && !error;
    if (matches)
    {
        std::ifstream input(path, std::ios::binary);
        std::string   value(owner.size(), '\0');
        matches = input.read(value.data(), static_cast<std::streamsize>(value.size())) && value == owner;
    }
    return matches;
}
} // namespace
Status RecordDirectoryInputs(const Scene& scene, BuildMetadata& metadata)
{
    AssetFileStamp              stamp;
    std::error_code             error;
    const std::filesystem::path source = std::filesystem::canonical(std::filesystem::u8path(scene.sourcePath), error);
    Status                      status;
    if (error || !StampAssetFile(Utf8Path(source), stamp))
    {
        status = {StatusCode::SourceUnavailable, "Cannot identify imported source"};
    }
    else
    {
        metadata.inputs = {{Utf8Path(source), true, stamp.size, stamp.modified, scene.sourceContentHash}};
        metadata.inputs.insert(metadata.inputs.end(), scene.dependencies.begin(), scene.dependencies.end());
    }
    return status;
}
Status VerifyDirectoryInputs(const BuildMetadata& metadata, const Cancellation* cancellation)
{
    SceneImportControl           control({const_cast<Cancellation*>(cancellation), &detail::PollImport});
    std::vector<SceneDependency> inputs = metadata.inputs;
    return detail::CheckInputs(inputs, false, false, control);
}
Status SceneDirectoryManifest(const std::string&         cacheRoot,
                              const std::string&         source,
                              const BuildMetadata&       metadata,
                              const CompressionSettings& compression,
                              std::string&               manifestPath)
{
    std::error_code       error;
    std::filesystem::path root, input;
    Status                status = detail::MakePath(cacheRoot, root);
    if (status.Succeeded())
    {
        status = detail::MakePath(source, input);
    }
    if (status.Succeeded())
    {
        input = std::filesystem::canonical(input, error);
        if (!error)
        {
            std::filesystem::create_directories(root, error);
        }
        if (error)
        {
            status = {StatusCode::IOError, "Cannot resolve cache root or source: " + error.message()};
        }
    }
    detail::CacheFileLock ownership;
    if (status.Succeeded())
    {
        status = ownership.Acquire(root / ".zen-owners.lock", true, true, true, nullptr);
    }
    if (status.Succeeded())
    {
        const std::string identity = Utf8Path(input);
        const std::string owner    = OwnerJson(identity);
        const std::string stem     = ModelStem(input);
        const std::string suffix   = "-" + DigestText(detail::HashBytes(identity.data(), identity.size()));
        SceneBuildJob     job;
        job.sourceSettings = metadata.settings;
        job.processing     = metadata.processing;
        job.profile        = metadata.profile;
        job.compression    = compression;
        const std::string settings =
            detail::BuildSettingsKey(job, metadata.sourceFormat, metadata.importerVersion) + metadata.buildFingerprint;
        const std::string filename =
            metadata.profile + "-" + DigestText(detail::HashBytes(settings.data(), settings.size())) + ".zenasset";
        if (metadata.profile != "reference" && metadata.profile != "desktop")
        {
            status = {StatusCode::InvalidArgument, "Unknown scene directory profile"};
        }
        bool claimed = false;
        for (unsigned index = 0; status.Succeeded() && !claimed && index < 2; ++index)
        {
            const std::filesystem::path folder  = root / std::filesystem::u8path(index == 0 ? stem : stem + suffix);
            const std::filesystem::path receipt = folder / "source.json";
            std::filesystem::create_directories(folder, error);
            if (!error)
            {
                const std::filesystem::path actual   = std::filesystem::canonical(folder, error);
                const std::filesystem::path expected = std::filesystem::canonical(root, error) / folder.filename();
#if defined(_WIN32)
                const bool redirected = _wcsicmp(actual.c_str(), expected.c_str()) != 0;
#else
                const bool redirected = actual != expected;
#endif
                if (!error && redirected)
                {
                    status = {StatusCode::InvalidData, "Redirected model directory"};
                }
            }
            if (status.Succeeded() && !error && !std::filesystem::exists(receipt, error))
            {
                std::filesystem::path  temporary;
                detail::FileOpenResult opened = detail::OpenTemporary(receipt, temporary);
                status                        = opened.status;
                if (opened.file != nullptr)
                {
                    status = detail::WriteFileBytes(opened.file, owner.data(), owner.size(), nullptr);
                    if (std::fclose(opened.file) != 0)
                    {
                        status = {StatusCode::IOError, "Cannot finish cache directory ownership file"};
                    }
                    if (status.Succeeded())
                    {
                        status = detail::ReplaceFileAtomically(temporary, receipt, true);
                    }
                    std::error_code cleanup;
                    std::filesystem::remove(temporary, cleanup);
                }
            }
            if (error)
            {
                status = {StatusCode::IOError, "Cannot claim cache directory: " + error.message()};
            }
            else if (status.Succeeded() && MatchesOwner(receipt, owner))
            {
                claimed      = true;
                manifestPath = Utf8Path(folder / filename);
            }
        }
        if (status.Succeeded() && !claimed)
        {
            status = {StatusCode::InvalidData, "Both model directory names are owned by other sources"};
        }
    }
    return status;
}
namespace
{
struct CacheEntry
{
    std::filesystem::path           path;
    std::filesystem::file_time_type time;
};
bool CollectModel(const std::filesystem::path& model, const CacheEntry& drop, uint64_t& removed)
{
    detail::CacheFileLock publisher;
    detail::CacheFileLock readers;
    bool                  safe = publisher.Acquire(model / ".zen-publish.lock", true, true, false, nullptr).Succeeded();
    std::set<std::filesystem::path>        live;
    std::error_code                        error;
    zen::HeapVector<std::filesystem::path> stale;
    for (std::filesystem::directory_iterator it(model, error), end; safe && !error && it != end; it.increment(error))
    {
        if (it->path().extension() == ".zenasset" && it->path() != drop.path)
        {
            // Read the direct index under the publisher lock. The reader gate is
            // acquired after collecting references, before any unlink.
            const PackageResult manifest = OpenPackage(Utf8Path(it->path()));
            safe                         = manifest.status.Succeeded();
            if (safe)
            {
                for (const PackagePart& part : manifest.package->Parts())
                {
                    live.insert(model / part.relativePath);
                }
            }
        }
        else if (Utf8Path(it->path().filename()).find(".zenasset.tmp.") != std::string::npos)
        {
            stale.push_back(it->path());
        }
    }
    for (const char* folder : {"geometry", "textures"})
    {
        const std::filesystem::path sub = model / folder;
        if (safe && !error && std::filesystem::is_directory(sub, error) && std::filesystem::canonical(sub, error) == sub)
        {
            for (std::filesystem::directory_iterator it(sub, error), end; !error && it != end; it.increment(error))
            {
                const std::string name = Utf8Path(it->path().filename());
                if ((name.starts_with("mesh-") || name.starts_with("image-") || name.starts_with("mesh.zenpart.tmp."))
                    && (it->path().extension() == ".zenpart" || name.find(".zenpart.tmp.") != std::string::npos)
                    && !live.contains(it->path()))
                {
                    stale.push_back(it->path());
                }
            }
        }
    }
    safe = safe && readers.Acquire(model / ".zen-reader.lock", true, true, false, nullptr).Succeeded();
    if (safe && !error && !drop.path.empty())
    {
        // A lookup or concurrent publication can refresh this variant after the LRU scan.
        safe = std::filesystem::last_write_time(drop.path, error) == drop.time;
    }
    if (safe && !error && !drop.path.empty())
    {
        const uint64_t size = std::filesystem::file_size(drop.path, error);
        if (!error && std::filesystem::remove(drop.path, error))
        {
            removed += size;
        }
    }
    for (const std::filesystem::path& file : stale)
    {
        if (safe && !error)
        {
            const uint64_t size = std::filesystem::file_size(file, error);
            if (!error && std::filesystem::remove(file, error))
            {
                removed += size;
            }
        }
    }
    return safe && !error;
}
} // namespace
bool TryCollectUnusedSceneParts(const std::string& manifest)
{
    std::filesystem::path path;
    std::error_code       error;
    bool                  collected = false;
    if (detail::MakePath(manifest, path).Succeeded())
    {
        const std::filesystem::path model = std::filesystem::canonical(path.parent_path(), error);
        if (!error && std::filesystem::is_regular_file(model / "source.json", error))
        {
            uint64_t removed = 0;
            collected        = CollectModel(model, {}, removed);
        }
    }
    return collected;
}

Status TrimSceneDirectoryCache(const std::string& cacheRoot, uint64_t byteBudget, const std::string& keep)
{
    std::error_code                       error;
    const std::filesystem::path           root = std::filesystem::weakly_canonical(std::filesystem::u8path(cacheRoot), error);
    const std::filesystem::path           kept = std::filesystem::weakly_canonical(std::filesystem::u8path(keep), error);
    zen::HeapVector<CacheEntry>           entries;
    uint64_t                              total = 0;
    const std::filesystem::file_time_type stale = std::filesystem::file_time_type::clock::now() - std::chrono::hours(1);
    for (std::filesystem::recursive_directory_iterator it(root, error), end; !error && it != end; it.increment(error))
    {
        const std::filesystem::path path = it->path();
        if (it->is_directory(error))
        {
            if (std::filesystem::canonical(path, error) != path
                || (it.depth() == 0 && !std::filesystem::exists(path / "source.json", error))
                || (it.depth() == 1 && path.filename() != "geometry" && path.filename() != "textures") || it.depth() > 1)
            {
                it.disable_recursion_pending();
            }
            else if (it.depth() == 0)
            {
                uint64_t removed = 0;
                CollectModel(path, {}, removed);
            }
        }
        else if (!error && path.extension() == ".zenasset" && it.depth() <= 1)
        {
            entries.push_back({path, it->last_write_time(error)});
            total += it->file_size(error);
        }
        else if (!error && it.depth() == 2 && path.extension() == ".zenpart")
        {
            total += it->file_size(error);
        }
        else if (!error && it.depth() == 0 && it->last_write_time(error) < stale)
        {
            const std::string name = Utf8Path(path.filename());
            if (name.find(".zenasset.tmp.") != std::string::npos || name.find(".zenasset.tmp-") != std::string::npos)
            {
                std::filesystem::remove(path, error);
            }
        }
    }
    std::sort(entries.begin(), entries.end(), [](const CacheEntry& a, const CacheEntry& b) { return a.time < b.time; });
    for (const CacheEntry& entry : entries)
    {
        if (!error && total > byteBudget && entry.path != kept)
        {
            uint64_t removed = 0;
            if (entry.path.parent_path() == root)
            {
                const uint64_t size = std::filesystem::file_size(entry.path, error);
                if (!error && std::filesystem::remove(entry.path, error))
                {
                    removed = size;
                }
            }
            else
            {
                CollectModel(entry.path.parent_path(), entry, removed);
            }
            total -= std::min(total, removed);
        }
    }
    return error ? Status{StatusCode::IOError, "Scene cache cleanup: " + error.message()} : Status{};
}

} // namespace zenasset
