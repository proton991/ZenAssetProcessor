#include "ZenAsset/Processor/Write.h"
#include "ZenAsset/Processor/Import.h"
#include "Format/SceneArchive.h"
#include "Format/PackageLayout.h"
#include "Writer/Compression.h"
#include "SceneValueComparison.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace
{
using namespace zenasset;
uint32_t checks = 0;
bool     Check(bool condition, const char* message)
{
    ++checks;
    if (!condition)
    {
        std::fprintf(stderr, "%s\n", message);
    }
    return condition;
}

bool CancelAfterPolls(void* user)
{
    uint32_t& polls = *static_cast<uint32_t*>(user);
    ++polls;
    return polls >= 4;
}

std::vector<std::byte> ReadFile(const std::filesystem::path& path)
{
    std::ifstream           stream(path, std::ios::binary);
    const std::vector<char> characters((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    std::vector<std::byte>  result(characters.size());
    for (size_t index = 0; index < result.size(); ++index)
    {
        result[index] = std::byte(characters[index]);
    }
    return result;
}

bool SaveFile(const std::filesystem::path& path, const std::vector<std::byte>& bytes)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    return stream.good();
}

bool CheckLegacyMetadata(const std::filesystem::path& path,
                         const Scene&                 scene,
                         const SceneWriteOptions&     options,
                         uint32_t                     version)
{
    // Replace only the current metadata with its earlier representation,
    // preserving resource IDs, all other payloads, and the existing offsets.
    bool                        valid      = true;
    std::vector<std::byte>      bytes      = ReadFile(path);
    const std::filesystem::path legacyPath = path.string() + ".v" + std::to_string(version) + ".zenasset";
    const detail::SceneMetadata metadata{options.metadata, scene.sourcePath, scene.dependencies};
    uint64_t                    budget = options.limits.maxAllocationBytes;
    detail::SceneArchive<false> sizing({}, nullptr, options.limits, budget, nullptr, version);
    sizing(metadata);
    std::vector<std::byte>      legacy(sizing.ByteSize());
    detail::SceneArchive<false> archive({}, &legacy, options.limits, budget, nullptr, version);
    archive(metadata);
    valid               &= Check(sizing.Finish().Succeeded() && archive.Finish().Succeeded(), "create legacy metadata fixture");
    uint8_t*       file  = reinterpret_cast<uint8_t*>(bytes.data());
    const uint64_t count = detail::Read64(file + 32);
    bool           found = false;
    for (uint64_t index = 0; valid && index < count; ++index)
    {
        uint8_t* entry = file + detail::HeaderBytes + index * detail::DirectoryEntryBytes;
        if (detail::Read32(entry + 8) == static_cast<uint32_t>(ResourceType::Metadata))
        {
            found                  = true;
            const uint64_t offset  = detail::Read64(entry + 24);
            const uint64_t stored  = detail::Read64(entry + 32);
            valid                 &= Check(legacy.size() < stored, "earlier metadata fits in current payload storage");
            if (valid)
            {
                const uint64_t addedBytes = stored - legacy.size();
                std::memcpy(file + offset, legacy.data(), legacy.size());
                std::memset(file + offset + legacy.size(), 0, static_cast<size_t>(addedBytes));
                detail::Write32(entry + 12, version);
                detail::Write64(entry + 32, legacy.size());
                detail::Write64(entry + 40, legacy.size());
                detail::Write64(entry + 48, detail::HashBytes(legacy.data(), legacy.size()));
            }
        }
    }
    valid &= Check(found, "metadata record found in compatibility fixture");
    if (valid)
    {
        detail::Write64(file + 48, detail::HashBytes(file + detail::HeaderBytes, count * detail::DirectoryEntryBytes));
        detail::Write64(file + 56, detail::HashBytes(file, 56));
        valid                      &= Check(SaveFile(legacyPath, bytes), "publish legacy metadata fixture");
        const SceneReadResult read  = ReadScene(legacyPath.string());
        valid &= Check(read.status.Succeeded() && read.scene && scene_test::Same(scene, *read.scene)
                           && read.metadata.compression.transport == TransportCodec::None
                           && read.metadata.compression.zstandardLevel == 3 && read.metadata.processing == ProcessingSettings{},
                       "earlier metadata remains readable with raw compression defaults");
        const SceneMetadataResult metadataRead = ReadSceneMetadata(legacyPath.string());
        valid &= Check(metadataRead.status.Succeeded() && metadataRead.metadata.compression.transport == TransportCodec::None,
                       "metadata-only read supports earlier records");
    }
    std::error_code error;
    std::filesystem::remove(legacyPath, error);
    return valid;
}

bool CheckCompression()
{
    bool                         valid = true;
    const std::vector<std::byte> repeated(1ull << 20, std::byte{42});
    CompressionSettings          settings;
    settings.transport                = TransportCodec::Zstandard;
    detail::CompressedResource result = detail::CompressResource(repeated, settings, 512ull << 20, 256ull << 20, nullptr);
    valid &= Check(result.status.Succeeded() && result.codec == TransportCodec::Zstandard && !result.bytes.empty()
                       && result.bytes.size() < repeated.size() / 100,
                   "repeated data uses bounded zstd encoding");
    for (int32_t level : {1, 19})
    {
        settings.zstandardLevel  = level;
        result                   = detail::CompressResource(repeated, settings, 512ull << 20, 256ull << 20, nullptr);
        valid                   &= Check(result.status.Succeeded() && result.codec == TransportCodec::Zstandard,
                                         "supported endpoint compression level succeeds");
    }
    for (int32_t level : {0, -1, 20, INT32_MAX})
    {
        settings.zstandardLevel  = level;
        result                   = detail::CompressResource(repeated, settings, 512ull << 20, 256ull << 20, nullptr);
        valid                   &= Check(result.status.code == StatusCode::InvalidArgument && result.bytes.empty(),
                                         "unsupported compression level rejected before allocation");
    }
    settings.zstandardLevel = 3;
    result                  = detail::CompressResource(repeated, settings, 512ull << 20, 1, nullptr);
    valid &= Check(result.status.code == StatusCode::ResourceLimit && result.bytes.empty(), "encoder workspace limit enforced");
    result  = detail::CompressResource(repeated, settings, repeated.size(), 256ull << 20, nullptr);
    valid  &= Check(result.status.code == StatusCode::ResourceLimit && result.bytes.empty(),
                    "combined output plus workspace budget enforced");
    const std::vector<std::byte> tiny(8, std::byte{42});
    result  = detail::CompressResource(tiny, settings, 0, 0, nullptr);
    valid  &= Check(result.status.Succeeded() && result.codec == TransportCodec::None && result.bytes.empty(),
                    "small chunks use raw bytes without codec allocations");
    std::vector<std::byte> noise(1ull << 20);
    uint32_t               random = 0x91c3219b;
    for (std::byte& byte : noise)
    {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        byte    = std::byte(random & 255);
    }
    result              = detail::CompressResource(noise, settings, 512ull << 20, 256ull << 20, nullptr);
    valid              &= Check(result.status.Succeeded() && result.codec == TransportCodec::None && result.bytes.empty(),
                                "incompressible chunks fall back to raw and release output");
    uint32_t     polls  = 0;
    Cancellation cancellation(CancelAfterPolls, &polls);
    result              = detail::CompressResource(repeated, settings, 512ull << 20, 256ull << 20, &cancellation);
    valid              &= Check(result.status.code == StatusCode::Cancelled && result.bytes.empty() && polls >= 4,
                                "streaming compression observes cancellation between input slices");
    settings.transport  = TransportCodec::None;
    result              = detail::CompressResource(repeated, settings, 0, 0, nullptr);
    valid              &= Check(result.status.Succeeded() && result.codec == TransportCodec::None && result.bytes.empty(),
                                "raw policy needs no encoder workspace");
    settings.transport  = static_cast<TransportCodec>(99);
    result              = detail::CompressResource(repeated, settings, 512ull << 20, 256ull << 20, nullptr);
    valid              &= Check(result.status.code == StatusCode::UnsupportedCodec, "unknown codec rejected");
    return valid;
}

bool CheckScene(const std::filesystem::path& path)
{
    bool  valid = true;
    Scene scene;
    scene.name           = "compressed package";
    scene.sourceDocument = std::string(1ull << 20, ' ');
    SceneWriteOptions options;
    options.metadata.buildFingerprint = "compression-test";
    options.compression.transport     = TransportCodec::Zstandard;
    // The duplicated metadata value must not override the active writer policy.
    options.metadata.compression.transport  = TransportCodec::None;
    const Status status                     = WriteScene(path.string(), scene, options);
    valid                                  &= Check(status.Succeeded(), status.message.c_str());
    if (status.Succeeded())
    {
        const PackageResult package  = OpenPackage(path.string());
        valid                       &= Check(package.status.Succeeded(), package.status.message.c_str());
        if (package.package)
        {
            uint32_t compressed = 0;
            uint32_t raw        = 0;
            for (const ResourceRecord& resource : package.package->Resources())
            {
                compressed += resource.codec == TransportCodec::Zstandard ? 1 : 0;
                raw        += resource.codec == TransportCodec::None ? 1 : 0;
                valid &=
                    Check(resource.storedBytes <= resource.decodedBytes, "stored chunks are never larger than decoded bytes");
            }
            valid                      &= Check(compressed > 0 && raw > 0, "package mixes compressed and raw fallback chunks");
            const SceneReadResult read  = ReadScene(*package.package);
            valid                      &= Check(read.status.Succeeded() && read.scene && scene_test::Same(scene, *read.scene),
                                                "compressed package round trip preserves every scene field");
            valid                      &= Check(read.metadata.compression.transport == TransportCodec::Zstandard
                                                    && read.metadata.compression.zstandardLevel == 3,
                                                "metadata records authoritative requested compression settings");
        }
        const std::vector<std::byte> published = ReadFile(path);
        SceneWriteOptions            rejected  = options;
        rejected.compression.zstandardLevel    = 20;
        valid &=
            Check(WriteScene(path.string(), scene, rejected).code == StatusCode::InvalidArgument && ReadFile(path) == published,
                  "invalid options preserve prior published package");
        rejected                 = options;
        rejected.maxEncoderBytes = 1;
        valid &=
            Check(WriteScene(path.string(), scene, rejected).code == StatusCode::ResourceLimit && ReadFile(path) == published,
                  "workspace exhaustion preserves prior package");
        rejected                           = options;
        rejected.limits.maxAllocationBytes = 2ull << 20;
        valid &=
            Check(WriteScene(path.string(), scene, rejected).code == StatusCode::ResourceLimit && ReadFile(path) == published,
                  "combined allocation exhaustion preserves prior package");
        rejected                             = options;
        rejected.limits.package.maxFileBytes = published.size();
        valid                 &= Check(WriteScene(path.string(), scene, rejected).Succeeded(),
                                       "file size budget measures compressed storage instead of raw payload size");
        const Status repeated  = WriteScene(path.string(), scene, options);
        valid                 &= Check(repeated.Succeeded() && ReadFile(path) == published,
                                       "same input and options produce byte-identical compressed packages");
        Cancellation cancelled;
        cancelled.Cancel();
        valid &= Check(WriteScene(path.string(), scene, options, &cancelled).code == StatusCode::Cancelled
                           && ReadFile(path) == published,
                       "cancelled write preserves prior package");
        options.compression.transport = TransportCodec::None;
        valid &= Check(WriteScene(path.string(), scene, options).Succeeded(), "raw default remains supported");
        const PackageResult rawPackage  = OpenPackage(path.string());
        valid                          &= Check(rawPackage.status.Succeeded(), "raw package remains readable");
        if (rawPackage.package)
        {
            for (const ResourceRecord& record : rawPackage.package->Resources())
            {
                valid &= Check(record.codec == TransportCodec::None && record.storedBytes == record.decodedBytes,
                               "raw policy emits no compressed chunks");
            }
        }
        valid &= CheckLegacyMetadata(path, scene, options, 1);
        valid &= CheckLegacyMetadata(path, scene, options, 2);
        valid &= CheckLegacyMetadata(path, scene, options, 3);
        valid &= CheckLegacyMetadata(path, scene, options, 4);
        valid &= CheckLegacyMetadata(path, scene, options, 5);
    }
    return valid;
}

bool CheckImportedScene(const std::filesystem::path& path, const char* source)
{
    bool               valid = true;
    SceneImportContext context;
    context.workerCount               = 2;
    const SceneImportResult imported  = ImportScene({source}, context);
    valid                            &= Check(bool(imported.asset), "import compressed round-trip fixture");
    if (imported.asset)
    {
        SceneWriteOptions options;
        options.compression.transport  = TransportCodec::Zstandard;
        const Status written           = WriteScene(path.string(), *imported.asset, options);
        valid                         &= Check(written.Succeeded(), written.message.c_str());
        if (written.Succeeded())
        {
            const SceneReadResult read = ReadScene(path.string());
            valid &= Check(read.status.Succeeded() && read.scene && scene_test::Same(*imported.asset, *read.scene),
                           "compressed imported fixture retains all canonical records");
        }
    }
    return valid;
}
} // namespace

int main(int argc, char** argv)
{
    bool                        valid = CheckCompression();
    const int64_t               tick  = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("zenasset-compression-" + std::to_string(tick) + ".zenasset");
    valid &= CheckScene(path);
    for (int index = 1; index < argc; ++index)
    {
        valid &= CheckImportedScene(path, argv[index]);
    }
    std::error_code error;
    std::filesystem::remove(path, error);
    std::printf("Compression writer: %u checks, %s\n", checks, valid ? "passed" : "failed");
    return valid ? 0 : 1;
}
