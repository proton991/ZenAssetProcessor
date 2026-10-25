#include "SceneValueComparison.h"
#include <ZenAsset/Format/Validation.h>
#include <ZenAsset/Format/Version.h>
#include <ZenAsset/Processor/Cook.h>
#include <ZenAsset/Processor/Write.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string_view>

namespace
{
using namespace zenasset;
using Clock                   = std::chrono::steady_clock;
constexpr uint32_t ImageCount = 36;
struct Scenario
{
    bool     array{false};
    uint32_t size{0};
};

bool Check(bool valid, const std::string& text)
{
    if (!valid)
    {
        std::cerr << text << '\n';
    }
    return valid;
}

bool ParseScenario(std::string_view name, std::string_view size, Scenario& scenario)
{
    scenario.array                      = name == "array";
    const std::from_chars_result parsed = std::from_chars(size.data(), size.data() + size.size(), scenario.size);
    return Check((name == "array" || name == "spatial") && parsed.ec == std::errc{} && parsed.ptr == size.data() + size.size()
                     && (scenario.size == 128 || scenario.size == 512),
                 "Expected array/spatial and size 128/512");
}

SceneCookOptions Options(const Scenario& scenario, bool enabled)
{
    SceneCookOptions options;
    options.settings.textures.arrayMaxLayers = 12;
    options.settings.textures.arrayPages     = enabled && scenario.array;
    options.settings.textures.spatialPages   = enabled && !scenario.array;
    return options;
}

SceneWriteOptions WriteOptions(const SceneCookOptions& options)
{
    SceneWriteOptions write;
    write.compression.transport     = TransportCodec::Zstandard;
    write.metadata.profile          = "desktop";
    write.metadata.sourceFormat     = "procedural-atlas-v1";
    write.metadata.buildFingerprint = LibraryBuildFingerprint();
    write.metadata.processing       = options.settings;
    return write;
}

void Statistics(const Scene& scene)
{
    uint64_t bytes = 0, mipBytes = 0;
    uint32_t arrayPages = 0, spatialBindings = 0;
    for (const ImageAsset& image : scene.images)
    {
        if (image.pixels)
        {
            bytes      += image.pixels->bytes.size();
            arrayPages += image.pixels->arrayLayers > 1;
            for (const std::vector<uint8_t>& level : image.pixels->mips)
            {
                mipBytes += level.size();
            }
        }
    }
    for (const TextureAsset& texture : scene.textures)
    {
        spatialBindings += texture.atlasRegion != ImageRegion{};
    }
    std::cout << "\"images\":" << scene.images.size() << ",\"textures\":" << scene.textures.size()
              << ",\"materials\":" << scene.materials.size() << ",\"array_pages\":" << arrayPages
              << ",\"spatial_bindings\":" << spatialBindings << ",\"base_vector_bytes\":" << bytes
              << ",\"mip_vector_bytes\":" << mipBytes;
}

std::string Recipe(const Scenario& scenario)
{
    std::ostringstream text;
    text << "{\"generator\":\"original atlas_measurements fixture v1\",\"images\":" << ImageCount
         << ",\"width\":" << scenario.size << ",\"height\":" << scenario.size << ",\"mips\":\""
         << (scenario.array ? "complete authored chain" : "single authored level")
         << "\",\"storage\":\"RGBA8Srgb\",\"sampler\":\"clamp/linear\","
            "\"pixel_formula\":\"R=(x/4+17*image+13*level)%256; G=(y/4+29*image+3*level)%256; "
            "B=((x/16 XOR y/16)*17+11*image+7*level)%256; A=255\","
            "\"geometry\":\"36 materials/primitives sharing one triangle, two mesh instances\","
            "\"uv\":\"inside [0,1]; alternate bindings have scale 0.5 and offsets (0.25,0.1)\"}\n";
    return text.str();
}

std::shared_ptr<Scene> Fixture(const Scenario& scenario, const std::filesystem::path& recipePath)
{
    std::shared_ptr<Scene> scene = std::make_shared<Scene>();
    scene->name                  = "original-atlas-resource-workload-v1";
    scene->sourcePath            = recipePath.string();
    scene->sourceDocument        = Recipe(scenario);
    scene->vertexTexCoordSets    = {0};
    scene->vertices.resize(3);
    scene->vertices[1].pos = Vec4(1, 0, 0, 1);
    scene->vertices[2].pos = Vec4(0, 1, 0, 1);
    scene->vertices[0].uv0 = Vec2(0.1f, 0.2f);
    scene->vertices[1].uv0 = Vec2(0.9f, 0.2f);
    scene->vertices[2].uv0 = Vec2(0.4f, 0.9f);
    for (Vertex& vertex : scene->vertices)
    {
        vertex.normal  = Vec4(0, 0, 1, 0);
        vertex.tangent = Vec4(1, 0, 0, 1);
        scene->vertexTexCoords.push_back({vertex.uv0});
    }
    scene->indices = {0, 1, 2};
    scene->meshes.resize(1);
    scene->nodes.resize(2);
    scene->nodes[0].mesh = scene->nodes[1].mesh = MeshId{0};
    scene->nodes[1].translation                 = Vec3(2, 0, 0);
    scene->roots                                = {NodeId{0}, NodeId{1}};
    SamplerAsset sampler;
    sampler.minFilter = sampler.magFilter = sampler.mipFilter = TextureFilter::Linear;
    sampler.wrapS = sampler.wrapT = SamplerAddressMode::ClampToEdge;
    sampler.useMipmaps            = scenario.array;
    scene->samplers.push_back(sampler);
    for (uint32_t image = 0; image < ImageCount; ++image)
    {
        std::shared_ptr<ImagePixels> pixels = std::make_shared<ImagePixels>();
        pixels->width = pixels->height = scenario.size;
        pixels->format                 = PixelFormat::RGBA8Srgb;
        uint32_t extent = scenario.size, level = 0;
        bool     more = true;
        while (more)
        {
            std::vector<uint8_t> bytes(size_t(extent) * extent * 4);
            for (uint32_t y = 0; y < extent; ++y)
            {
                for (uint32_t x = 0; x < extent; ++x)
                {
                    const size_t index = (size_t(y) * extent + x) * 4;
                    bytes[index]       = uint8_t((x / 4 + 17 * image + 13 * level) % 256);
                    bytes[index + 1]   = uint8_t((y / 4 + 29 * image + 3 * level) % 256);
                    bytes[index + 2]   = uint8_t(((x / 16 ^ y / 16) * 17 + 11 * image + 7 * level) % 256);
                    bytes[index + 3]   = 255;
                }
            }
            pixels->mips.push_back(std::move(bytes));
            more   = scenario.array && extent > 1;
            extent = std::max(1u, extent / 2);
            ++level;
        }
        pixels->bytes = pixels->mips[0];
        scene->images.push_back({"image-" + std::to_string(image), pixels});
        TextureAsset texture;
        texture.name    = "texture-" + std::to_string(image);
        texture.image   = ImageId{image};
        texture.sampler = SamplerId{0};
        texture.format  = PixelFormat::RGBA8Srgb;
        scene->textures.push_back(texture);
        MaterialAsset material;
        material.name                = "material-" + std::to_string(image);
        material.m_pBaseColorTexture = TextureId{image};
        if (image % 2)
        {
            material.textureTransforms[0].row0 = Vec4(0.5f, 0, 0.25f, 0);
            material.textureTransforms[0].row1 = Vec4(0, 0.5f, 0.1f, 0);
        }
        scene->materials.push_back(material);
        MeshPrimitiveAsset primitive;
        primitive.vertexCount = primitive.indexCount = 3;
        primitive.material                           = MaterialId{image};
        primitive.materialSlot                       = image;
        primitive.bounds                             = Bounds(Vec3(0), Vec3(1, 1, 0));
        scene->meshes[0].primitives.push_back(primitive);
    }
    return scene;
}

bool ExpectedPages(const Scene& scene, const Scenario& scenario, bool enabled)
{
    const uint32_t expected = !enabled ? ImageCount : scenario.array ? 3 : scenario.size == 128 ? 1 : 4;
    bool           valid    = Check(scene.images.size() == expected && scene.textures.size() == ImageCount,
                                    "Expected actual page creation and unchanged logical texture count");
    for (const TextureAsset& texture : scene.textures)
    {
        const ImagePixels& pixels  = *scene.images[texture.image.value].pixels;
        valid                     &= Check((!enabled || !scenario.array || pixels.arrayLayers == 12)
                                               && (enabled && !scenario.array) == (texture.atlasRegion != ImageRegion{}),
                                           "Unexpected page/layer/region mapping");
    }
    return valid;
}

bool Preserved(const Scene& source, const Scene& cooked)
{
    Scene restored    = cooked;
    restored.images   = source.images;
    restored.textures = source.textures;
    bool valid        = Check(scene_test::Same(source, restored), "Non-image resources changed");
    for (size_t index = 0; valid && index < source.textures.size(); ++index)
    {
        const TextureAsset& texture   = cooked.textures[index];
        TextureAsset        expected  = source.textures[index];
        expected.image                = texture.image;
        expected.arrayLayer           = texture.arrayLayer;
        expected.atlasRegion          = texture.atlasRegion;
        valid                        &= Check(scene_test::Same(expected, texture), "Authored texture binding changed");
        const ImagePixels& a          = *source.images[source.textures[index].image.value].pixels;
        const ImagePixels& b          = *cooked.images[texture.image.value].pixels;
        if (texture.atlasRegion != ImageRegion{})
        {
            const ImageRegion& region = texture.atlasRegion;
            valid &= Check(region.width == a.width && region.height == a.height && b.mips.size() == 1 && a.format == b.format
                               && b.arrayLayers == 1,
                           "Spatial logical shape changed");
            for (uint32_t y = 0; valid && y < a.height; ++y)
            {
                const size_t from = size_t(y) * a.width * 4;
                const size_t to   = (size_t(region.y + y) * b.width + region.x) * 4;
                valid &= Check(std::equal(a.bytes.begin() + from, a.bytes.begin() + from + a.width * 4, b.bytes.begin() + to),
                               "Spatial tile bytes changed");
            }
        }
        else
        {
            valid &= Check(a.width == b.width && a.height == b.height && a.format == b.format && a.mips.size() == b.mips.size(),
                           "Array logical shape/mips changed");
            for (uint32_t level = 0; valid && level < a.mips.size(); ++level)
            {
                std::span<const uint8_t> first, second;
                valid &= Check(GetImageLayerMipBytes(a, 0, level, first)
                                   && GetImageLayerMipBytes(b, texture.arrayLayer, level, second)
                                   && std::equal(first.begin(), first.end(), second.begin(), second.end()),
                               "Array logical mip bytes changed");
            }
        }
    }
    return valid;
}

bool RoundTrip(const Scene& scene, const SceneCookOptions& options, const std::filesystem::path& path)
{
    const SceneReadResult read = ReadScene(path.string());
    return Check(read.status.Succeeded() && read.scene && scene_test::Same(scene, *read.scene)
                     && read.metadata.processing == options.settings
                     && read.metadata.compression.transport == TransportCodec::Zstandard
                     && !read.metadata.compression.meshoptimizer,
                 "Full scene roundtrip or settings comparison failed");
}

bool Prepare(const Scenario& scenario, const std::filesystem::path& directory)
{
    const std::filesystem::path  recipePath = directory / "source.json";
    const std::shared_ptr<Scene> source     = Fixture(scenario, recipePath);
    std::ofstream                recipe(recipePath, std::ios::binary);
    recipe << source->sourceDocument;
    recipe.close();
    std::string failure;
    bool        valid = Check(bool(recipe) && ValidateSceneAsset(*source, UINT64_MAX, &failure), "Fixture invalid: " + failure);
    SceneWriteOptions raw;
    if (valid)
    {
        valid = Check(WriteScene((directory / "input.zenasset").string(), *source, raw).Succeeded(), "Input write failed");
        std::cout << "FIXTURE {";
        Statistics(*source);
        std::cout << "}\n";
    }
    for (bool enabled : {false, true})
    {
        if (valid)
        {
            const SceneCookOptions options  = Options(scenario, enabled);
            const SceneCookResult  cooked   = CookScene(source, options);
            const SceneCookResult  repeated = CookScene(source, options);
            valid = Check(cooked.status.Succeeded() && cooked.scene && repeated.status.Succeeded() && repeated.scene,
                          "Preflight cook failed: " + cooked.status.message);
            if (valid)
            {
                valid &= ExpectedPages(*cooked.scene, scenario, enabled) && Preserved(*source, *cooked.scene)
                      && Check(scene_test::Same(*cooked.scene, *repeated.scene), "Repeated cook is not deterministic");
                const std::filesystem::path path = directory / (enabled ? "expected-on.zenasset" : "expected-off.zenasset");
                valid &= Check(WriteScene(path.string(), *cooked.scene, WriteOptions(options)).Succeeded(),
                               "Expected package write failed");
                valid &= RoundTrip(*cooked.scene, options, path);
                std::cout << "EXPECTED {\"mode\":\"" << (enabled ? "on" : "off")
                          << "\",\"diagnostics\":" << cooked.diagnostics.size()
                          << ",\"settings_hash\":" << ProcessingSettingsHash(options.settings) << ',';
                Statistics(*cooked.scene);
                std::cout << "}\n";
            }
        }
    }
    const SceneReadResult unchanged = ReadScene((directory / "input.zenasset").string());
    valid &= Check(unchanged.status.Succeeded() && unchanged.scene && scene_test::Same(*source, *unchanged.scene),
                   "Input scene mutated during cooking");
    return valid;
}

bool Measure(const Scenario& scenario, bool enabled, const std::filesystem::path& input, const std::filesystem::path& output)
{
    const Clock::time_point readStart = Clock::now();
    const SceneReadResult   source    = ReadScene(input.string());
    const Clock::time_point readEnd   = Clock::now();
    bool                    valid     = Check(source.status.Succeeded() && source.scene, "Input read failed");
    if (valid)
    {
        const SceneCookOptions  options   = Options(scenario, enabled);
        const Clock::time_point cookStart = Clock::now();
        const SceneCookResult   cooked    = CookScene(source.scene, options);
        const Clock::time_point cookEnd   = Clock::now();
        valid = Check(cooked.status.Succeeded() && cooked.scene, "Measured cook failed: " + cooked.status.message);
        if (valid)
        {
            const SceneWriteOptions write      = WriteOptions(options);
            const Clock::time_point writeStart = Clock::now();
            const Status            written    = WriteScene(output.string(), *cooked.scene, write);
            const Clock::time_point writeEnd   = Clock::now();
            valid                              = Check(written.Succeeded(), "Measured write failed: " + written.message)
                                              && ExpectedPages(*cooked.scene, scenario, enabled);
            std::cout << "ATLAS_SAMPLE {\"input_read_seconds\":" << std::chrono::duration<double>(readEnd - readStart).count()
                      << ",\"cook_seconds\":" << std::chrono::duration<double>(cookEnd - cookStart).count()
                      << ",\"write_seconds\":" << std::chrono::duration<double>(writeEnd - writeStart).count()
                      << ",\"diagnostics\":" << cooked.diagnostics.size() << ',';
            Statistics(*cooked.scene);
            std::cout << "}\n";
        }
    }
    return valid;
}

bool Verify(const std::filesystem::path& expected, int count, char** paths)
{
    const SceneReadResult reference = ReadScene(expected.string());
    bool                  valid     = Check(reference.status.Succeeded() && reference.scene, "Expected package read failed");
    for (int index = 0; valid && index < count; ++index)
    {
        const SceneReadResult read = ReadScene(paths[index]);
        valid &= Check(read.status.Succeeded() && read.scene && scene_test::Same(*reference.scene, *read.scene)
                           && read.metadata.processing == reference.metadata.processing,
                       "Measured package differs from validated deterministic scene");
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
    bool     valid = false;
    Scenario scenario;
    if (argc == 2 && std::string_view(argv[1]) == "baseline")
    {
        valid = true;
    }
    else if (argc == 5 && std::string_view(argv[1]) == "prepare" && ParseScenario(argv[2], argv[3], scenario))
    {
        valid = Prepare(scenario, argv[4]);
    }
    else if (argc == 7 && std::string_view(argv[1]) == "measure" && ParseScenario(argv[2], argv[3], scenario)
             && (std::string_view(argv[4]) == "off" || std::string_view(argv[4]) == "on"))
    {
        valid = Measure(scenario, std::string_view(argv[4]) == "on", argv[5], argv[6]);
    }
    else if (argc >= 4 && std::string_view(argv[1]) == "verify")
    {
        valid = Verify(argv[2], argc - 3, argv + 3);
    }
    else
    {
        std::cerr << "Usage: baseline | prepare array/spatial 128/512 directory | "
                     "measure array/spatial 128/512 off/on input output | verify expected outputs...\n";
    }
    return valid ? 0 : 1;
}
