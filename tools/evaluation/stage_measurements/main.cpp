#include "Codecs/BlockDecoding.h"
#include "Diagnostics/StageMeasurements.h"
#include <algorithm>
#include <cmath>
#include "Fixture.h"
#include "SceneValueComparison.h"
#include <ZenAsset/Processor/Cook.h>
#include <ZenAsset/Processor/Import.h>
#include <ZenAsset/Processor/Write.h>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <string_view>

namespace
{
using namespace zenasset;
namespace measurement       = zenasset::detail::measurement;
using Clock                 = std::chrono::steady_clock;
constexpr bool Instrumented = ZENASSET_STAGE_PIPELINE_INSTRUMENTED != 0;

bool Check(bool valid, const std::string& message)
{
    if (!valid)
    {
        std::cerr << message << '\n';
    }
    return valid;
}
std::string Json(const std::string& value)
{
    std::string    output = "\"";
    constexpr char hex[]  = "0123456789abcdef";
    for (unsigned char c : value)
    {
        if (c == '"' || c == '\\')
        {
            output += '\\';
            output += char(c);
        }
        else if (c < 32)
        {
            output += "\\u00";
            output += hex[c >> 4];
            output += hex[c & 15];
        }
        else
        {
            output += char(c);
        }
    }
    output += '"';
    return output;
}
void Diagnostics(const std::vector<Diagnostic>& diagnostics)
{
    std::cout << "DIAGNOSTICS [";
    bool first = true;
    for (const Diagnostic& diagnostic : diagnostics)
    {
        if (!first)
        {
            std::cout << ',';
        }
        first = false;
        std::cout << "{\"severity\":" << uint32_t(diagnostic.severity) << ",\"status\":" << uint32_t(diagnostic.status)
                  << ",\"source\":" << Json(diagnostic.source) << ",\"object\":" << Json(diagnostic.object)
                  << ",\"message\":" << Json(diagnostic.message) << '}';
    }
    std::cout << "]\n";
}
void Metrics(const measurement::Statistics& statistics)
{
    std::cout << "METRICS {\"instrumented\":" << (Instrumented ? "true" : "false") << ",\"stages\":{";
    for (size_t index = 0; index < statistics.stages.size(); ++index)
    {
        if (index != 0)
        {
            std::cout << ',';
        }
        const measurement::Interval& interval = statistics.stages[index];
        std::cout << '"' << measurement::StageNames[index] << "\":{\"calls\":" << interval.calls
                  << ",\"inclusive_ns\":" << interval.inclusiveNs << ",\"exclusive_ns\":" << interval.exclusiveNs << '}';
    }
    std::cout << "},\"counters\":{";
    for (size_t index = 0; index < statistics.counters.size(); ++index)
    {
        if (index != 0)
        {
            std::cout << ',';
        }
        std::cout << '"' << measurement::CounterNames[index] << "\":" << statistics.counters[index];
    }
    std::cout << "}}\n";
}
void SceneCounts(const Scene& scene)
{
    uint32_t bc4 = 0, bc5 = 0, bc7 = 0, hdr = 0;
    for (const ImageAsset& image : scene.images)
    {
        bc4 += image.pixels->format == PixelFormat::BC4Unorm;
        bc5 += image.pixels->format == PixelFormat::BC5Unorm;
        bc7 += image.pixels->format == PixelFormat::BC7Unorm || image.pixels->format == PixelFormat::BC7Srgb;
    }
    for (const ImageBasedLightAsset& light : scene.imageBasedLights)
    {
        hdr += light.specular && light.specular->format == PixelFormat::BC6HUfloat;
    }
    std::cout << "COUNTS {\"vertices\":" << scene.vertices.size() << ",\"indices\":" << scene.indices.size()
              << ",\"images\":" << scene.images.size() << ",\"bc4_images\":" << bc4 << ",\"bc5_images\":" << bc5
              << ",\"bc7_images\":" << bc7 << ",\"bc6h_cubes\":" << hdr << "}\n";
}
SceneCookOptions CookOptions(bool textures)
{
    SceneCookOptions options;
    options.settings.geometry.optimizeVertexFetch     = true;
    options.settings.textures.generateMipmaps         = textures;
    options.settings.textures.compressBC7             = textures;
    options.settings.textures.compressBC5Normals      = textures;
    options.settings.textures.compressBC4Occlusion    = textures;
    options.settings.textures.compressBC6HEnvironment = textures;
    return options;
}
SceneWriteOptions WriteOptions(const BuildMetadata& metadata, std::string_view profile)
{
    SceneWriteOptions options;
    options.metadata                  = metadata;
    options.metadata.buildFingerprint = LibraryBuildFingerprint();
    options.compression.transport =
        profile == "zstd" || profile == "mesh_zstd" ? TransportCodec::Zstandard : TransportCodec::None;
    options.compression.meshoptimizer = profile == "mesh" || profile == "mesh_zstd";
    return options;
}
bool Source(const std::string& source, const std::string& output)
{
    SceneImportRequest request;
    request.path    = source;
    request.control = std::make_shared<SceneImportControl>();
    SceneImportContext context;
    context.workerCount = 1;
    SceneImportResult       imported;
    measurement::Statistics statistics;
    const Clock::time_point begin = Clock::now();
    {
        measurement::Binding binding(statistics);
        imported = ImportScene(request, context);
    }
    const Clock::time_point end = Clock::now();
    bool valid = Check(imported.status == StatusCode::Success && imported.asset, "Import failed: " + imported.error);
    if (valid)
    {
        BuildMetadata metadata;
        metadata.sourceFormat                  = imported.format;
        metadata.importerVersion               = imported.importerVersion;
        metadata.settings                      = request.settings;
        metadata.diagnostics                   = imported.diagnostics;
        const Status written                   = WriteScene(output, *imported.asset, WriteOptions(metadata, "raw"));
        valid                                  = Check(written.Succeeded(), "Import result write failed: " + written.message);
        const SceneImportFileProgress progress = request.control->GetFileProgress();
        std::cout << "SAMPLE {\"source_seconds\":" << std::chrono::duration<double>(end - begin).count()
                  << ",\"processed_files\":" << progress.processedFiles << ",\"total_files\":" << progress.totalFiles
                  << ",\"total_known\":" << (progress.totalKnown ? "true" : "false") << "}\n";
        SceneCounts(*imported.asset);
    }
    Diagnostics(imported.diagnostics);
    Metrics(statistics);
    return valid;
}
bool Fixture(const std::string& output)
{
    const std::shared_ptr<Scene> scene = MakeStageFixture();
    BuildMetadata                metadata;
    metadata.sourceFormat = "original-stage-fixture-v1";
    const Status written  = WriteScene(output, *scene, WriteOptions(metadata, "raw"));
    return Check(written.Succeeded(), "Original fixture write failed: " + written.message);
}
bool CookWrite(bool textures, std::string_view profile, const std::string& input, const std::string& output)
{
    const SceneReadResult source = ReadScene(input);
    bool                  valid  = Check(source.status.Succeeded() && source.scene, "Canonical input read failed");
    if (valid)
    {
        const SceneCookOptions  options = CookOptions(textures);
        measurement::Statistics statistics;
        SceneCookResult         cooked;
        Status                  written;
        Clock::time_point       cookBegin, cookEnd, writeBegin, writeEnd;
        {
            measurement::Binding binding(statistics);
            cookBegin = Clock::now();
            cooked    = CookScene(source.scene, options);
            cookEnd   = Clock::now();
            if (cooked.status.Succeeded() && cooked.scene)
            {
                BuildMetadata metadata        = source.metadata;
                metadata.profile              = "desktop";
                metadata.processing           = options.settings;
                metadata.diagnostics          = cooked.diagnostics;
                const SceneWriteOptions write = WriteOptions(metadata, profile);
                writeBegin                    = Clock::now();
                written                       = WriteScene(output, *cooked.scene, write);
                writeEnd                      = Clock::now();
            }
        }
        valid = Check(cooked.status.Succeeded() && cooked.scene && written.Succeeded(),
                      "Cook/write failed: " + cooked.status.message + " " + written.message);
        if (valid)
        {
            std::cout << "SAMPLE {\"cook_seconds\":" << std::chrono::duration<double>(cookEnd - cookBegin).count()
                      << ",\"write_seconds\":" << std::chrono::duration<double>(writeEnd - writeBegin).count() << "}\n";
            SceneCounts(*cooked.scene);
        }
        Diagnostics(cooked.diagnostics);
        Metrics(statistics);
    }
    return valid;
}
bool Read(const std::string& path, const std::string& snapshot = {})
{
    measurement::Statistics statistics;
    SceneReadResult         result;
    const Clock::time_point begin = Clock::now();
    {
        measurement::Binding binding(statistics);
        result = ReadScene(path);
    }
    const Clock::time_point end   = Clock::now();
    bool                    valid = Check(result.status.Succeeded() && result.scene, "Read failed: " + result.status.message);
    if (valid)
    {
        std::cout << "SAMPLE {\"read_seconds\":" << std::chrono::duration<double>(end - begin).count() << "}\n";
        SceneCounts(*result.scene);
        Diagnostics(result.metadata.diagnostics);
    }
    Metrics(statistics);
    if (valid && !snapshot.empty())
    {
        // Untimed canonical snapshot exposes each build's decoded values to
        // the independent verifier, without reference storage in timed reads.
        const Status written = WriteScene(snapshot, *result.scene, WriteOptions(result.metadata, "raw"));
        valid                = Check(written.Succeeded(), "Decoded snapshot write failed: " + written.message);
    }
    return valid;
}
bool SameDiagnostics(const std::vector<Diagnostic>& first, const std::vector<Diagnostic>& second)
{
    bool equal = first.size() == second.size();
    for (size_t index = 0; equal && index < first.size(); ++index)
    {
        const Diagnostic& a = first[index];
        const Diagnostic& b = second[index];
        equal               = a.severity == b.severity && a.status == b.status && a.source == b.source && a.object == b.object
                           && a.message == b.message;
    }
    return equal;
}
bool SameMetadata(const BuildMetadata& a, const BuildMetadata& b)
{
    return a.sourceFormat == b.sourceFormat && a.importerVersion == b.importerVersion
        && a.processorVersion == b.processorVersion && a.buildFingerprint == b.buildFingerprint && a.profile == b.profile
        && a.settings.sceneIndex == b.settings.sceneIndex
        && scene_test::Same(a.settings.animationBake, b.settings.animationBake)
        && a.settings.fallbackMissingNamedUVSets == b.settings.fallbackMissingNamedUVSets && a.cacheKey == b.cacheKey
        && scene_test::Same(a.inputs, b.inputs) && SameDiagnostics(a.diagnostics, b.diagnostics)
        && a.compression.transport == b.compression.transport && a.compression.meshoptimizer == b.compression.meshoptimizer
        && a.compression.zstandardLevel == b.compression.zstandardLevel && a.processing == b.processing;
}
bool EnvironmentDifference(const std::string& firstPath, const std::string& secondPath)
{
    const SceneReadResult first  = ReadScene(firstPath);
    const SceneReadResult second = ReadScene(secondPath);
    bool                  valid  = Check(first.status.Succeeded() && first.scene && second.status.Succeeded() && second.scene,
                                         "Comparison package read failed");
    if (valid)
    {
        Scene rest               = *second.scene;
        rest.imageBasedLights    = first.scene->imageBasedLights;
        const bool restEqual     = scene_test::Same(*first.scene, rest);
        uint64_t   changedBlocks = 0, blocks = 0;
        double     maximumDecodedDifference = 0;
        valid                               = first.scene->imageBasedLights.size() == second.scene->imageBasedLights.size();
        for (size_t light = 0; valid && light < first.scene->imageBasedLights.size(); ++light)
        {
            const std::shared_ptr<const EnvironmentCubePixels>& a = first.scene->imageBasedLights[light].specular;
            const std::shared_ptr<const EnvironmentCubePixels>& b = second.scene->imageBasedLights[light].specular;
            valid = a && b && a->format == b->format && a->size == b->size && a->mipLevels == b->mipLevels
                 && a->blockMipFaces.size() == b->blockMipFaces.size();
            for (size_t face = 0; valid && face < a->blockMipFaces.size(); ++face)
            {
                const std::vector<uint8_t>& x = a->blockMipFaces[face];
                const std::vector<uint8_t>& y = b->blockMipFaces[face];
                valid                         = x.size() == y.size() && x.size() % 16 == 0;
                for (size_t offset = 0; valid && offset < x.size(); offset += 16)
                {
                    ++blocks;
                    const bool changed  = !std::equal(x.begin() + offset, x.begin() + offset + 16, y.begin() + offset);
                    changedBlocks      += changed;
                    std::array<Vec4, 16> decodedA{}, decodedB{};
                    valid =
                        detail::DecodeBlock(a->format, {reinterpret_cast<const std::byte*>(x.data() + offset), 16}, decodedA)
                            .Succeeded()
                        && detail::DecodeBlock(b->format, {reinterpret_cast<const std::byte*>(y.data() + offset), 16}, decodedB)
                               .Succeeded();
                    for (size_t pixel = 0; valid && pixel < decodedA.size(); ++pixel)
                    {
                        maximumDecodedDifference =
                            std::max({maximumDecodedDifference, double(std::abs(decodedA[pixel].x - decodedB[pixel].x)),
                                      double(std::abs(decodedA[pixel].y - decodedB[pixel].y)),
                                      double(std::abs(decodedA[pixel].z - decodedB[pixel].z))});
                    }
                }
            }
        }
        std::cout << "ENVIRONMENT_DIFFERENCE {\"metadata_equal\":" << SameMetadata(first.metadata, second.metadata)
                  << ",\"rest_scene_equal\":" << restEqual << ",\"valid_blocks\":" << valid << ",\"blocks\":" << blocks
                  << ",\"changed_blocks\":" << changedBlocks
                  << ",\"maximum_decoded_rgb_difference\":" << maximumDecodedDifference << "}\n";
    }
    return valid;
}
bool Verify(const std::string& expected, int count, char** paths)
{
    const SceneReadResult reference = ReadScene(expected);
    bool                  valid     = Check(reference.status.Succeeded() && reference.scene, "Expected package read failed");
    for (int index = 0; valid && index < count; ++index)
    {
        const SceneReadResult actual = ReadScene(paths[index]);
        valid = Check(actual.status.Succeeded() && actual.scene && scene_test::Same(*reference.scene, *actual.scene)
                          && SameMetadata(reference.metadata, actual.metadata),
                      "Full scene/metadata comparison failed: " + std::string(paths[index]));
    }
    if (valid)
    {
        std::cout << "VERIFIED {\"packages\":" << count << "}\n";
    }
    return valid;
}
bool Audit(const std::string& path)
{
    const PackageResult result = OpenPackage(path);
    bool                valid  = Check(result.status.Succeeded() && result.package, "Package directory read failed");
    if (valid)
    {
        uint64_t zstd = 0, mesh = 0, stored = 0, decoded = 0;
        for (const ResourceRecord& record : result.package->Resources())
        {
            zstd    += record.codec == TransportCodec::Zstandard;
            mesh    += record.encoding != 0;
            stored  += record.storedBytes;
            decoded += record.decodedBytes;
        }
        valid = Check(result.package->ValidatePayloads().Succeeded(), "Stored payload audit failed");
        std::cout << "CHUNKS {\"resources\":" << result.package->Resources().size() << ",\"zstd_resources\":" << zstd
                  << ",\"mesh_resources\":" << mesh << ",\"stored_bytes\":" << stored
                  << ",\"transport_decoded_bytes\":" << decoded << "}\n";
    }
    return valid;
}
bool IsProfile(std::string_view profile)
{
    return profile == "raw" || profile == "zstd" || profile == "mesh" || profile == "mesh_zstd";
}
} // namespace

int main(int argc, char** argv)
{
    std::cout << std::setprecision(17);
    std::cout << "SDK {\"fingerprint\":\"" << zenasset::LibraryBuildFingerprint() << "\"}\n";
    bool valid = false;
    if (argc == 2 && std::string_view(argv[1]) == "baseline")
    {
        valid = true;
    }
    else if (argc == 4 && std::string_view(argv[1]) == "source")
    {
        valid = Source(argv[2], argv[3]);
    }
    else if (argc == 3 && std::string_view(argv[1]) == "fixture")
    {
        valid = Fixture(argv[2]);
    }
    else if (argc == 6 && std::string_view(argv[1]) == "cook-write"
             && (std::string_view(argv[2]) == "geometry" || std::string_view(argv[2]) == "textures") && IsProfile(argv[3]))
    {
        // Argument count is checked before doing any source or package work.
        valid = CookWrite(std::string_view(argv[2]) == "textures", argv[3], argv[4], argv[5]);
    }
    else if (argc == 3 && std::string_view(argv[1]) == "read")
    {
        valid = Read(argv[2]);
    }
    else if (argc == 4 && std::string_view(argv[1]) == "read-snapshot")
    {
        valid = Read(argv[2], argv[3]);
    }
    else if (argc == 3 && std::string_view(argv[1]) == "audit")
    {
        valid = Audit(argv[2]);
    }
    else if (argc == 4 && std::string_view(argv[1]) == "environment-difference")
    {
        valid = EnvironmentDifference(argv[2], argv[3]);
    }
    else if (argc >= 4 && std::string_view(argv[1]) == "verify")
    {
        valid = Verify(argv[2], argc - 3, argv + 3);
    }
    else
    {
        std::cerr
            << "Usage: baseline | source input output | fixture output | cook-write geometry/textures raw/zstd/mesh/mesh_zstd input output | read package | read-snapshot package output | audit package | environment-difference first second | verify expected outputs...\n";
    }
    return valid ? 0 : 1;
}
