#include "SceneValueComparison.h"
#include <ZenAsset/Processor/Cook.h>
#include <ZenAsset/Processor/Import.h>
#include <ZenAsset/Processor/Write.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string_view>

namespace
{
using namespace zenasset;
using Clock = std::chrono::steady_clock;

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

uint32_t Pages(const Scene& scene)
{
    std::vector<bool> used(scene.images.size());
    for (const TextureAsset& texture : scene.textures)
    {
        if (texture.image.IsValid() && texture.image.value < scene.images.size())
        {
            const ImagePixels& pixels = *scene.images[texture.image.value].pixels;
            used[texture.image.value] =
                used[texture.image.value] || pixels.arrayLayers > 1 || texture.atlasRegion != ImageRegion{};
        }
    }
    return static_cast<uint32_t>(std::count(used.begin(), used.end(), true));
}

void Statistics(const Scene& scene)
{
    uint64_t baseBytes = 0, mipBytes = 0;
    uint32_t mapped = 0;
    for (const ImageAsset& image : scene.images)
    {
        if (image.pixels)
        {
            baseBytes += image.pixels->bytes.size();
            for (const std::vector<uint8_t>& level : image.pixels->mips)
            {
                mipBytes += level.size();
            }
        }
    }
    for (const TextureAsset& texture : scene.textures)
    {
        if (texture.image.IsValid())
        {
            mapped += scene.images[texture.image.value].pixels->arrayLayers > 1 || texture.atlasRegion != ImageRegion{};
        }
    }
    std::cout << "\"images\":" << scene.images.size() << ",\"textures\":" << scene.textures.size()
              << ",\"materials\":" << scene.materials.size() << ",\"pages\":" << Pages(scene)
              << ",\"mapped_textures\":" << mapped << ",\"base_vector_bytes\":" << baseBytes
              << ",\"mip_vector_bytes\":" << mipBytes;
}

void Diagnostics(const std::vector<Diagnostic>& diagnostics)
{
    std::cout << "\"diagnostics\":[";
    bool first = true;
    for (const Diagnostic& diagnostic : diagnostics)
    {
        if (!first)
        {
            std::cout << ',';
        }
        first = false;
        std::cout << "{\"severity\":" << uint32_t(diagnostic.severity) << ",\"status\":" << uint32_t(diagnostic.status)
                  << ",\"object\":" << Json(diagnostic.object) << ",\"source\":" << Json(diagnostic.source)
                  << ",\"message\":" << Json(diagnostic.message) << '}';
    }
    std::cout << ']';
}

SceneCookOptions PageOptions(bool array, bool enabled)
{
    SceneCookOptions options;
    options.settings.textures.arrayPages   = array && enabled;
    options.settings.textures.spatialPages = !array && enabled;
    return options;
}

SceneWriteOptions WriteOptions(bool array, bool enabled, bool transport = true)
{
    SceneWriteOptions options;
    options.compression.transport                        = transport ? TransportCodec::Zstandard : TransportCodec::None;
    options.metadata.sourceFormat                        = "gltf";
    options.metadata.profile                             = "desktop";
    options.metadata.buildFingerprint                    = LibraryBuildFingerprint();
    options.metadata.processing                          = PageOptions(array, enabled).settings;
    options.metadata.processing.textures.generateMipmaps = array;
    return options;
}

bool Preserved(const Scene& source, const Scene& candidate)
{
    Scene restored    = candidate;
    restored.images   = source.images;
    restored.textures = source.textures;
    bool valid        = Check(source.textures.size() == candidate.textures.size() && scene_test::Same(source, restored),
                              "Atlas changed non-image scene records");
    for (size_t index = 0; valid && index < source.textures.size(); ++index)
    {
        const TextureAsset& texture   = candidate.textures[index];
        TextureAsset        expected  = source.textures[index];
        expected.image                = texture.image;
        expected.arrayLayer           = texture.arrayLayer;
        expected.atlasRegion          = texture.atlasRegion;
        valid                        &= Check(scene_test::Same(expected, texture), "Atlas changed authored texture fields");
        if (valid && texture.image.IsValid())
        {
            const ImagePixels& a = *source.images[source.textures[index].image.value].pixels;
            const ImagePixels& b = *candidate.images[texture.image.value].pixels;
            if (texture.atlasRegion != ImageRegion{})
            {
                const ImageRegion& region  = texture.atlasRegion;
                valid                     &= Check(region.width == a.width && region.height == a.height && b.mips.size() == 1
                                                       && a.format == b.format && b.arrayLayers == 1,
                                                   "Spatial tile shape changed");
                for (uint32_t y = 0; valid && y < a.height; ++y)
                {
                    const size_t from = size_t(y) * a.width * 4;
                    const size_t to   = (size_t(region.y + y) * b.width + region.x) * 4;
                    valid &=
                        Check(std::equal(a.bytes.begin() + from, a.bytes.begin() + from + a.width * 4, b.bytes.begin() + to),
                              "Spatial tile bytes changed");
                }
            }
            else
            {
                valid &=
                    Check(a.width == b.width && a.height == b.height && a.format == b.format && a.mips.size() == b.mips.size(),
                          "Logical image dimensions or mips changed");
                for (uint32_t level = 0; valid && level < std::max(size_t(1), a.mips.size()); ++level)
                {
                    std::span<const uint8_t> first, second;
                    valid &= Check(GetImageLayerMipBytes(a, source.textures[index].arrayLayer, level, first)
                                       && GetImageLayerMipBytes(b, texture.arrayLayer, level, second)
                                       && std::equal(first.begin(), first.end(), second.begin(), second.end()),
                                   "Logical texture mip bytes changed");
                }
            }
        }
    }
    return valid;
}

bool WriteAndCompare(const Scene& scene, bool array, bool enabled, const std::filesystem::path& path, bool transport = true)
{
    const SceneWriteOptions options = WriteOptions(array, enabled, transport);
    const Status            written = WriteScene(path.string(), scene, options);
    bool                    valid   = Check(written.Succeeded(), "Write failed: " + written.message);
    if (valid)
    {
        const SceneReadResult read = ReadScene(path.string());
        valid                      = Check(read.status.Succeeded() && read.scene && scene_test::Same(scene, *read.scene)
                                               && read.metadata.processing == options.metadata.processing,
                                           "Full scene roundtrip failed");
    }
    return valid;
}

bool PrepareBranch(const std::shared_ptr<const Scene>& imported, bool array, const std::filesystem::path& directory)
{
    SceneCookOptions commonOptions;
    commonOptions.settings.textures.generateMipmaps = array;
    const Clock::time_point begin                   = Clock::now();
    const SceneCookResult   common                  = CookScene(imported, commonOptions);
    const Clock::time_point end                     = Clock::now();
    bool valid = Check(common.status.Succeeded() && common.scene, "Common preparation failed: " + common.status.message);
    const std::string kind = array ? "array" : "spatial";
    if (valid)
    {
        std::cout << "COMMON {\"kind\":" << Json(kind)
                  << ",\"prepare_seconds\":" << std::chrono::duration<double>(end - begin).count() << ',';
        Statistics(*common.scene);
        std::cout << ',';
        Diagnostics(common.diagnostics);
        std::cout << "}\n";
        valid = WriteAndCompare(*common.scene, array, false, directory / (kind + "-input.zenasset"), false);
        if (!array)
        {
            valid &= Check(scene_test::Same(*imported, *common.scene), "Spatial branch changed authored input");
        }
        for (bool enabled : {false, true})
        {
            if (valid)
            {
                const SceneCookResult cooked = CookScene(common.scene, PageOptions(array, enabled));
                const SceneCookResult repeat = CookScene(common.scene, PageOptions(array, enabled));
                valid = Check(cooked.status.Succeeded() && cooked.scene && repeat.status.Succeeded() && repeat.scene,
                              "Atlas preparation failed: " + cooked.status.message);
                if (valid)
                {
                    valid                  &= Preserved(*common.scene, *cooked.scene)
                                           && Check(scene_test::Same(*cooked.scene, *repeat.scene), "Atlas cooking is not deterministic");
                    const std::string mode  = enabled ? "on" : "off";
                    valid &=
                        WriteAndCompare(*cooked.scene, array, enabled, directory / (kind + "-expected-" + mode + ".zenasset"));
                    std::cout << "BRANCH {\"kind\":" << Json(kind) << ",\"mode\":" << Json(mode) << ',';
                    Statistics(*cooked.scene);
                    std::cout << ',';
                    Diagnostics(cooked.diagnostics);
                    std::cout << "}\n";
                }
            }
        }
    }
    return valid;
}

bool Prepare(const std::filesystem::path& source, const std::filesystem::path& directory)
{
    SceneImportRequest request;
    request.path = source.string();
    SceneImportContext context;
    context.workerCount              = 2;
    const Clock::time_point begin    = Clock::now();
    const SceneImportResult imported = ImportScene(request, context);
    const Clock::time_point end      = Clock::now();
    bool valid = Check(imported.status == StatusCode::Success && imported.asset, "Import failed: " + imported.error);
    if (valid)
    {
        std::cout << "IMPORTED {\"import_seconds\":" << std::chrono::duration<double>(end - begin).count() << ',';
        Statistics(*imported.asset);
        std::cout << ',';
        Diagnostics(imported.diagnostics);
        std::cout << "}\n";
        valid = PrepareBranch(imported.asset, true, directory);
        if (valid)
        {
            valid = PrepareBranch(imported.asset, false, directory);
        }
    }
    return valid;
}

bool Measure(bool array, bool enabled, const std::filesystem::path& input, const std::filesystem::path& output)
{
    const Clock::time_point readBegin = Clock::now();
    const SceneReadResult   source    = ReadScene(input.string());
    const Clock::time_point readEnd   = Clock::now();
    bool                    valid     = Check(source.status.Succeeded() && source.scene, "Prepared input read failed");
    if (valid)
    {
        const SceneCookOptions  options   = PageOptions(array, enabled);
        const Clock::time_point cookBegin = Clock::now();
        const SceneCookResult   cooked    = CookScene(source.scene, options);
        const Clock::time_point cookEnd   = Clock::now();
        valid = Check(cooked.status.Succeeded() && cooked.scene && (!enabled || Pages(*cooked.scene) > 0),
                      "Timed case failed or did not create actual pages");
        if (valid)
        {
            const SceneWriteOptions write      = WriteOptions(array, enabled);
            const Clock::time_point writeBegin = Clock::now();
            const Status            written    = WriteScene(output.string(), *cooked.scene, write);
            const Clock::time_point writeEnd   = Clock::now();
            valid                              = Check(written.Succeeded(), "Measured publication failed: " + written.message);
            std::cout << "WORKLOAD_SAMPLE {\"input_read_seconds\":"
                      << std::chrono::duration<double>(readEnd - readBegin).count()
                      << ",\"cook_seconds\":" << std::chrono::duration<double>(cookEnd - cookBegin).count()
                      << ",\"write_seconds\":" << std::chrono::duration<double>(writeEnd - writeBegin).count() << ',';
            Statistics(*cooked.scene);
            std::cout << "}\n";
        }
    }
    return valid;
}

bool Verify(const std::filesystem::path& expected, int count, char** paths)
{
    const SceneReadResult reference = ReadScene(expected.string());
    bool                  valid     = Check(reference.status.Succeeded() && reference.scene, "Reference read failed");
    for (int index = 0; valid && index < count; ++index)
    {
        const SceneReadResult read = ReadScene(paths[index]);
        valid &= Check(read.status.Succeeded() && read.scene && scene_test::Same(*reference.scene, *read.scene)
                           && read.metadata.processing == reference.metadata.processing,
                       "Measured result failed full scene comparison");
    }
    if (valid)
    {
        std::cout << "VERIFIED {\"packages\":" << count << "}\n";
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
        valid = true;
    }
    else if (argc == 4 && std::string_view(argv[1]) == "prepare")
    {
        valid = Prepare(argv[2], argv[3]);
    }
    else if (argc == 6 && std::string_view(argv[1]) == "measure"
             && (std::string_view(argv[2]) == "array" || std::string_view(argv[2]) == "spatial")
             && (std::string_view(argv[3]) == "on" || std::string_view(argv[3]) == "off"))
    {
        valid = Measure(std::string_view(argv[2]) == "array", std::string_view(argv[3]) == "on", argv[4], argv[5]);
    }
    else if (argc >= 4 && std::string_view(argv[1]) == "verify")
    {
        valid = Verify(argv[2], argc - 3, argv + 3);
    }
    else
    {
        std::cerr
            << "Usage: baseline | prepare source directory | measure array/spatial on/off input output | verify expected outputs...\n";
    }
    return valid ? 0 : 1;
}
