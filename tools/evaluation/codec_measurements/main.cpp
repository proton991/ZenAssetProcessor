#include "SceneValueComparison.h"
#include <ZenAsset/Processor/Import.h>
#include <ZenAsset/Processor/Write.h>
#include <ZenAsset/Format/Version.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string_view>

namespace
{
using namespace zenasset;
using Clock = std::chrono::steady_clock;

struct Variant
{
    const char*    name;
    bool           mesh;
    TransportCodec transport;
};
constexpr std::array<Variant, 4> Variants{{{"raw", false, TransportCodec::None},
                                           {"zstd", false, TransportCodec::Zstandard},
                                           {"mesh", true, TransportCodec::None},
                                           {"mesh_zstd", true, TransportCodec::Zstandard}}};

bool Check(bool valid, const std::string& message)
{
    if (!valid)
    {
        std::cerr << message << '\n';
    }
    return valid;
}

void Counts(const Scene& scene)
{
    uint64_t imageBytes = 0;
    for (const ImageAsset& image : scene.images)
    {
        if (image.pixels)
        {
            imageBytes += image.pixels->bytes.size();
            for (const std::vector<uint8_t>& mip : image.pixels->mips)
            {
                imageBytes += mip.size();
            }
        }
    }
    std::cout << "\"vertices\":" << scene.vertices.size() << ",\"indices\":" << scene.indices.size()
              << ",\"meshes\":" << scene.meshes.size() << ",\"images\":" << scene.images.size()
              << ",\"image_stored_vector_bytes\":" << imageBytes << ",\"skins\":" << scene.skins.size()
              << ",\"animations\":" << scene.animations.size()
              << ",\"custom_attributes\":" << scene.extraVertexAttributes.size();
}

bool AuditPackage(const std::filesystem::path& path, const Variant& variant)
{
    const PackageResult opened = OpenPackage(path.string());
    bool                valid  = Check(opened.status.Succeeded() && opened.package, "Open failed: " + opened.status.message);
    uint32_t            vertexEncoded = 0, indexEncoded = 0, zstdChunks = 0;
    if (valid)
    {
        std::cout << "CHUNKS {\"variant\":\"" << variant.name << "\",\"resources\":[";
        bool first = true;
        for (const ResourceRecord& record : opened.package->Resources())
        {
            if (!first)
            {
                std::cout << ',';
            }
            first = false;
            std::cout << "{\"id\":" << record.id << ",\"type\":" << uint32_t(record.type) << ",\"version\":" << record.version
                      << ",\"codec\":" << uint32_t(record.codec) << ",\"encoding\":" << record.encoding
                      << ",\"stored_bytes\":" << record.storedBytes << ",\"transport_decoded_bytes\":" << record.decodedBytes
                      << '}';
            vertexEncoded += record.type == ResourceType::VertexStream && record.encoding == 1;
            indexEncoded  += record.type == ResourceType::IndexStream && record.encoding == 2;
            zstdChunks    += record.codec == TransportCodec::Zstandard;
            valid         &= Check(variant.mesh || record.encoding == 0, "Unexpected semantic encoding");
            valid         &= Check(variant.transport != TransportCodec::None || record.codec == TransportCodec::None,
                                   "Unexpected transport compression");
        }
        std::cout << "],\"vertex_encoded\":" << vertexEncoded << ",\"index_encoded\":" << indexEncoded
                  << ",\"zstd_chunks\":" << zstdChunks << "}\n";
        valid &= Check(!variant.mesh || (vertexEncoded > 0 && indexEncoded > 0), "Fixture did not exercise both mesh codecs");
        valid &= Check(variant.transport == TransportCodec::None || zstdChunks > 0, "Fixture did not exercise Zstandard");
    }
    return valid;
}

bool ComparePackage(const Scene& reference, const std::filesystem::path& path, const Variant& variant)
{
    const SceneReadResult read  = ReadScene(path.string());
    bool                  valid = Check(read.status.Succeeded() && read.scene, "Read failed: " + read.status.message);
    if (valid)
    {
        valid &= Check(scene_test::Same(reference, *read.scene), "Full public scene comparison failed");
        valid &= Check(read.metadata.compression.meshoptimizer == variant.mesh
                           && read.metadata.compression.transport == variant.transport
                           && read.metadata.compression.zstandardLevel == 3,
                       "Compression metadata differs");
        valid &= AuditPackage(path, variant);
    }
    return valid;
}

bool Prepare(const std::filesystem::path& source, const std::filesystem::path& directory)
{
    SceneImportRequest request;
    request.path = source.string();
    SceneImportContext context;
    context.workerCount              = 2;
    const SceneImportResult imported = ImportScene(request, context);
    bool valid = Check(imported.status == StatusCode::Success && imported.asset, "Import failed: " + imported.error);
    if (valid)
    {
        std::cout << "SCENE {";
        Counts(*imported.asset);
        std::cout << "}\n";
        for (const Variant& variant : Variants)
        {
            if (valid)
            {
                SceneWriteOptions options;
                options.compression.meshoptimizer   = variant.mesh;
                options.compression.transport       = variant.transport;
                options.metadata.sourceFormat       = imported.format;
                options.metadata.importerVersion    = imported.importerVersion;
                options.metadata.buildFingerprint   = LibraryBuildFingerprint();
                const std::filesystem::path path    = directory / (std::string(variant.name) + ".zenasset");
                const Status                written = WriteScene(path.string(), *imported.asset, options);
                valid                               = Check(written.Succeeded(), "Write failed: " + written.message)
                                                   && ComparePackage(*imported.asset, path, variant);
            }
        }
    }
    return valid;
}

bool Verify(const std::filesystem::path& directory)
{
    const SceneReadResult reference = ReadScene((directory / "raw.zenasset").string());
    bool                  valid     = Check(reference.status.Succeeded() && reference.scene, "Reference read failed");
    if (valid)
    {
        for (const Variant& variant : Variants)
        {
            valid &= ComparePackage(*reference.scene, directory / (std::string(variant.name) + ".zenasset"), variant);
        }
    }
    return valid;
}

bool Measure(const std::filesystem::path& path)
{
    const Clock::time_point start = Clock::now();
    const SceneReadResult   read  = ReadScene(path.string());
    const Clock::time_point end   = Clock::now();
    const bool              valid = Check(read.status.Succeeded() && read.scene, "Read failed: " + read.status.message);
    if (valid)
    {
        std::cout << "SAMPLE {\"read_seconds\":" << std::chrono::duration<double>(end - start).count() << ',';
        Counts(*read.scene);
        std::cout << "}\n";
    }
    return valid;
}
} // namespace

int main(int argc, char** argv)
{
    std::cout << std::setprecision(17);
    std::cout << "SDK {\"fingerprint\":\"" << zenasset::LibraryBuildFingerprint() << "\"}\n";
    bool valid = false;
    if (argc == 2 && std::string_view(argv[1]) == "baseline")
    {
        std::cout << "BASELINE {}\n";
        valid = true;
    }
    else if (argc == 4 && std::string_view(argv[1]) == "prepare")
    {
        valid = Prepare(argv[2], argv[3]);
    }
    else if (argc == 3 && std::string_view(argv[1]) == "verify")
    {
        valid = Verify(argv[2]);
    }
    else if (argc == 3 && std::string_view(argv[1]) == "read")
    {
        valid = Measure(argv[2]);
    }
    else
    {
        std::cerr << "Usage: codec_measurements baseline | prepare source output-directory | verify directory | read package\n";
    }
    return valid ? 0 : 1;
}
