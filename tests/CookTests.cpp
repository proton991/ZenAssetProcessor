#include "ZenAsset/Processor/Cook.h"
#include "ZenAsset/Processor/Import.h"
#include "ZenAsset/Processor/Write.h"
#include "Processing/SceneStorage.h"
#include "SceneValueComparison.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <limits>

namespace
{
using namespace zenasset;
uint32_t checks = 0;

bool Check(bool condition, const char* message)
{
    ++checks;
    if (!condition)
    {
        std::fprintf(stderr, "%s\n", message);
    }
    return condition;
}

bool CancelDuringTraversal(void* user)
{
    uint32_t& polls = *static_cast<uint32_t*>(user);
    ++polls;
    return polls >= 8;
}

std::shared_ptr<const Scene> MakeScene()
{
    std::shared_ptr<Scene> scene = std::make_shared<Scene>();
    scene->name                  = "cook fixture";
    scene->sourceDocument.assign(128 * 1024, ' ');
    scene->vertices.resize(128);
    scene->vertexTexCoords.resize(128, std::vector<Vec2>{{0, 0}, {1, 1}});
    scene->vertexTexCoordSets = {0, 1};
    scene->materials.resize(1);
    scene->materials[0].name            = "retained material";
    std::shared_ptr<ImagePixels> pixels = std::make_shared<ImagePixels>();
    pixels->width                       = 4;
    pixels->height                      = 4;
    pixels->bytes.assign(64, 127);
    pixels->mips.push_back(pixels->bytes);
    scene->images.push_back({"shared pixels", pixels});
    return scene;
}

bool CheckCooking(const std::filesystem::path& path)
{
    bool                               valid    = true;
    const std::shared_ptr<const Scene> input    = MakeScene();
    const Scene                        snapshot = *input;
    SceneCookOptions                   options;
    SceneCookResult                    cooked = CookScene(input, options);
    valid &= Check(cooked.status.Succeeded() && cooked.scene == input, "disabled processing shares the input owner");
    options.settings.geometry.optimizeVertexFetch = true;
    // Inactive texture policy is still explicit provenance/cache identity.
    options.settings.textures.bc7Quality             = 3;
    options.settings.textures.maxDimension           = 8192;
    options.settings.textures.colorMinPsnr           = 42.0f;
    options.settings.textures.colorMaxRmse           = 0.005f;
    options.settings.textures.scalarMaxError         = 0.02f;
    options.settings.textures.normalRmsAngleDegrees  = 0.5f;
    options.settings.textures.normalMaxAngleDegrees  = 2.5f;
    options.settings.textures.alphaCoverageMaxError  = 0.005f;
    options.settings.textures.hdrMaxRelativeRmse     = 0.01f;
    options.limits.maxStringBytes                    = 64;
    options.limits.maxSourceDocumentBytes            = 256 * 1024;
    cooked                                           = CookScene(input, options);
    valid                                           &= Check(cooked.status.Succeeded() && cooked.scene && cooked.scene != input,
                                                             "enabled processing produces an independent owning scene");
    if (cooked.scene)
    {
        valid &= Check(scene_test::Same(*cooked.scene, snapshot) && scene_test::Same(*input, snapshot),
                       "cooking preserves unchanged records and never mutates input");
        valid &= Check(cooked.scene->images[0].pixels == input->images[0].pixels,
                       "immutable image storage is shared across the cooked copy");
        SceneWriteOptions write;
        write.metadata.profile           = "desktop";
        write.metadata.processing        = options.settings;
        write.compression.transport      = TransportCodec::Zstandard;
        write.compression.meshoptimizer  = true;
        const Status stored              = WriteScene(path.string(), *cooked.scene, write);
        valid                           &= Check(stored.Succeeded(), stored.message.c_str());
        if (stored.Succeeded())
        {
            const SceneReadResult read = ReadScene(path.string());
            valid &= Check(read.status.Succeeded() && read.scene && scene_test::Same(*cooked.scene, *read.scene),
                           "the same cooked result survives mesh and transport encoding");
            valid &= Check(read.metadata.processing == options.settings, "metadata retains all applied processing settings");
        }
    }
    uint64_t remaining                 = UINT64_MAX;
    valid                             &= Check(detail::ReserveSceneCopy(*input, options.limits, remaining, nullptr).Succeeded(),
                                               "copy storage can be counted before allocation");
    SceneCookOptions limited           = options;
    limited.limits.maxAllocationBytes  = UINT64_MAX - remaining - 1;
    cooked                             = CookScene(input, limited);
    valid                             &= Check(cooked.status.code == StatusCode::ResourceLimit && !cooked.scene,
                                               "nested copy allocations are rejected before publishing a scene");
    limited                            = options;
    limited.limits.maxSourceDocumentBytes  = 1;
    cooked                                 = CookScene(input, limited);
    valid                                 &= Check(cooked.status.code == StatusCode::ResourceLimit && !cooked.scene,
                                                   "source document has its own bound during cooking");
    limited                                = options;
    limited.settings.geometry.positionQuantizationBits = 25;
    cooked                                             = CookScene(input, limited);
    valid   &= Check(cooked.status.code == StatusCode::InvalidArgument && !cooked.scene, "invalid quantization bits rejected");
    limited  = options;
    limited.settings.geometry.positionMaxAbsoluteError = std::numeric_limits<float>::quiet_NaN();
    valid &= Check(CookScene(input, limited).status.code == StatusCode::InvalidArgument, "nonfinite quality bounds rejected");
    valid &= Check(CookScene(nullptr).status.code == StatusCode::InvalidArgument, "null input rejected");
    Cancellation cancelled;
    cancelled.Cancel();
    cooked  = CookScene(input, options, &cancelled);
    valid  &= Check(cooked.status.code == StatusCode::Cancelled && !cooked.scene, "already-cancelled cook publishes no scene");
    uint32_t           polls = 0;
    const Cancellation during(&CancelDuringTraversal, &polls);
    cooked  = CookScene(input, options, &during);
    valid  &= Check(cooked.status.code == StatusCode::Cancelled && !cooked.scene && polls == 8,
                    "cancellation interrupts nested record traversal");
    valid  &= Check(scene_test::Same(*input, snapshot), "failed and cancelled cooks leave input unchanged");
    return valid;
}

bool CheckImported(const char* source, const std::filesystem::path& path)
{
    SceneImportContext context;
    context.workerCount              = 2;
    const SceneImportResult imported = ImportScene({source}, context);
    bool                    valid    = Check(bool(imported.asset), imported.error.c_str());
    if (imported.asset)
    {
        const SceneCookResult reference  = CookScene(imported.asset);
        valid                           &= Check(reference.status.Succeeded() && reference.scene == imported.asset,
                                                 "reference cooking preserves imported ownership and values");
        SceneCookOptions options;
        options.settings.geometry.optimizeVertexFetch = true;
        const SceneCookResult cooked                  = CookScene(imported.asset, options);
        valid &= Check(cooked.status.Succeeded() && cooked.scene, cooked.status.message.c_str());
        if (cooked.scene)
        {
            SceneWriteOptions write;
            write.metadata.processing        = options.settings;
            write.metadata.profile           = "desktop";
            write.compression.transport      = TransportCodec::Zstandard;
            write.compression.meshoptimizer  = true;
            const Status stored              = WriteScene(path.string(), *cooked.scene, write);
            valid                           &= Check(stored.Succeeded(), stored.message.c_str());
            if (stored.Succeeded())
            {
                const SceneReadResult read = ReadScene(path.string());
                valid &= Check(read.status.Succeeded() && read.scene && scene_test::Same(*cooked.scene, *read.scene),
                               "cooked import direct and serialized scene records agree");
            }
        }
    }
    return valid;
}

bool CheckTextureCooking(const std::filesystem::path& path)
{
    std::shared_ptr<Scene> input        = std::make_shared<Scene>();
    input->vertexTexCoordSets           = {0};
    std::shared_ptr<ImagePixels> pixels = std::make_shared<ImagePixels>();
    pixels->width                       = 32;
    pixels->height                      = 32;
    pixels->bytes.resize(32 * 32 * 4);
    for (size_t offset = 0; offset < pixels->bytes.size(); offset += 4)
    {
        pixels->bytes[offset]     = 128;
        pixels->bytes[offset + 1] = 64;
        pixels->bytes[offset + 2] = 32;
        pixels->bytes[offset + 3] = 255;
    }
    input->images.push_back({"color", pixels});
    input->textures.resize(1);
    input->textures[0].image  = ImageId{0};
    input->textures[0].format = PixelFormat::RGBA8Srgb;
    input->materials.resize(1);
    input->materials[0].m_pBaseColorTexture = TextureId{0};
    const Scene      snapshot               = *input;
    SceneCookOptions options;
    options.settings.textures.generateMipmaps = true;
    options.settings.textures.compressBC7     = true;
    const SceneCookResult cooked              = CookScene(input, options);
    bool                  valid               = Check(cooked.status.Succeeded() && cooked.scene && cooked.scene != input,
                                                      "texture-only cooking enters the shared processing path");
    if (cooked.scene)
    {
        valid &= Check(cooked.scene->images.size() == 1 && cooked.scene->textures.size() == 1,
                       "texture cooking removes no required resource and retains no obsolete full-size image");
        if (cooked.scene->images.size() == 1 && cooked.scene->textures.size() == 1)
        {
            const ImagePixels& output = *cooked.scene->images[0].pixels;
            valid &= Check(output.format == PixelFormat::BC7Srgb && output.mips.size() == 6 && output.mips[0] == output.bytes
                               && output.mips.back().size() == 16 && cooked.scene->textures[0].format == PixelFormat::BC7Srgb,
                           "texture-only cooking creates BC7 storage with a complete offline mip chain and matching view");
        }
        SceneWriteOptions write;
        write.metadata.profile       = "desktop";
        write.metadata.processing    = options.settings;
        write.compression.transport  = TransportCodec::Zstandard;
        const Status stored          = WriteScene(path.string(), *cooked.scene, write);
        valid                       &= Check(stored.Succeeded(), stored.message.c_str());
        if (stored.Succeeded())
        {
            const SceneReadResult read = ReadScene(path.string());
            valid &= Check(read.status.Succeeded() && read.scene && scene_test::Same(*cooked.scene, *read.scene)
                               && read.metadata.processing == options.settings,
                           "compressed image bytes and active texture policy survive source-free package loading");
        }
    }
    valid &= Check(scene_test::Same(*input, snapshot) && input->images[0].pixels == pixels,
                   "texture cooking preserves the caller's original pixels and scene records");
    return valid;
}

std::shared_ptr<EnvironmentCubePixels> MakeEnvironmentPixels(uint32_t size, uint32_t mipLevels)
{
    std::shared_ptr<EnvironmentCubePixels> pixels = std::make_shared<EnvironmentCubePixels>();
    pixels->size                                  = size;
    pixels->mipLevels                             = mipLevels;
    pixels->rgba32FloatMipFaces.resize(size_t(mipLevels) * 6);
    for (size_t face = 0; face < pixels->rgba32FloatMipFaces.size(); ++face)
    {
        const uint32_t extent = std::max(1u, size >> (face / 6));
        pixels->rgba32FloatMipFaces[face].assign(size_t(extent) * extent, Vec4{4 + float(face), 2, 16, 1});
    }
    return pixels;
}

std::shared_ptr<Scene> MakeEnvironmentScene(const std::shared_ptr<const EnvironmentCubePixels>& pixels)
{
    std::shared_ptr<Scene> scene = std::make_shared<Scene>();
    scene->name                  = "authored HDR environment";
    scene->imageBasedLight       = 0;
    scene->imageBasedLights.resize(1);
    ImageBasedLightAsset& light = scene->imageBasedLights[0];
    light.name                  = "studio cube";
    light.intensity             = 2.5f;
    light.irradianceCoefficients.assign(9, Vec3{0.5f, 0.25f, 0.125f});
    light.specular = pixels;
    return scene;
}

SceneCookOptions EnvironmentOptions()
{
    SceneCookOptions options;
    options.settings.textures.compressBC6HEnvironment = true;
    return options;
}

bool CheckEnvironmentCooking(const std::filesystem::path& path)
{
    const std::shared_ptr<EnvironmentCubePixels> pixels    = MakeEnvironmentPixels(5, 2);
    const EnvironmentCubePixels                  original  = *pixels;
    const std::shared_ptr<Scene>                 input     = MakeEnvironmentScene(pixels);
    const Scene                                  snapshot  = *input;
    const SceneCookResult                        reference = CookScene(input);
    bool             valid   = Check(reference.status.Succeeded() && reference.scene == input
                                         && reference.scene->imageBasedLights[0].specular == pixels,
                                     "disabled environment cooking shares the original scene and cube owners");
    SceneCookOptions options = EnvironmentOptions();
    // Ordinary image mip generation must not replace authored roughness levels.
    options.settings.textures.generateMipmaps = true;
    const SceneCookResult cooked              = CookScene(input, options);
    valid &= Check(cooked.status.Succeeded() && cooked.scene && cooked.scene != input, cooked.status.message.c_str());
    if (cooked.scene)
    {
        const std::shared_ptr<const EnvironmentCubePixels>& output = cooked.scene->imageBasedLights[0].specular;
        valid &=
            Check(output && output != pixels && output->format == PixelFormat::BC6HUfloat && output->size == 5
                      && output->mipLevels == 2 && output->blockMipFaces.size() == 12 && output->rgba32FloatMipFaces.empty(),
                  "BC6H cooking owns only compressed faces and preserves the partial authored mip chain");
        if (output && output->blockMipFaces.size() == 12)
        {
            for (size_t face = 0; face < output->blockMipFaces.size(); ++face)
            {
                valid &= Check(output->blockMipFaces[face].size() == (face < 6 ? 64 : 16),
                               "odd-sized authored cube faces retain complete BC6H block footprints");
            }
            valid &= Check(output->blockMipFaces[0] != output->blockMipFaces[5]
                               && output->blockMipFaces[6] != output->blockMipFaces[11],
                           "distinct authored faces remain distinct at each mip");
        }
        Scene expected                        = snapshot;
        expected.imageBasedLights[0].specular = output;
        valid                          &= Check(scene_test::Same(*cooked.scene, expected),
                                                "environment cooking changes only cube storage and preserves lighting records");
        const SceneCookResult repeated  = CookScene(cooked.scene, options);
        valid &= Check(repeated.status.Succeeded() && repeated.scene && repeated.scene->imageBasedLights[0].specular == output,
                       "already compressed environments retain their immutable owner without transcoding");
        SceneWriteOptions write;
        write.metadata.profile       = "desktop";
        write.metadata.processing    = options.settings;
        write.compression.transport  = TransportCodec::Zstandard;
        const Status stored          = WriteScene(path.string(), *cooked.scene, write);
        valid                       &= Check(stored.Succeeded(), stored.message.c_str());
        if (stored.Succeeded())
        {
            const SceneReadResult read = ReadScene(path.string());
            valid &= Check(read.status.Succeeded() && read.scene && scene_test::Same(*cooked.scene, *read.scene)
                               && read.metadata.processing == options.settings,
                           "BC6H faces and active HDR policy survive package loading without a source document");
        }
    }
    valid &= Check(scene_test::Same(*input, snapshot) && scene_test::Same(*pixels, original)
                       && input->imageBasedLights[0].specular == pixels,
                   "environment cooking preserves the caller's raw cube contents and shared owner");
    return valid;
}

bool CheckEnvironmentFallbacks()
{
    bool valid = true;
    for (uint32_t mode = 0; mode < 5; ++mode)
    {
        const std::shared_ptr<EnvironmentCubePixels> pixels  = MakeEnvironmentPixels(4, 3);
        SceneCookOptions                             options = EnvironmentOptions();
        Vec4&                                        last    = pixels->rgba32FloatMipFaces.back()[0];
        if (mode == 0)
        {
            last.x = -1;
        }
        else if (mode == 1)
        {
            last.y = 65505;
        }
        else if (mode == 2)
        {
            last.w = 0.5f;
        }
        else if (mode == 3)
        {
            options.settings.textures.maxDimension = 2;
        }
        else
        {
            last = {1e-10f, 1e-10f, 1e-10f, 1};
        }
        const EnvironmentCubePixels  original = *pixels;
        const std::shared_ptr<Scene> input    = MakeEnvironmentScene(pixels);
        const SceneCookResult        cooked   = CookScene(input, options);
        valid &= Check(cooked.status.Succeeded() && cooked.scene && cooked.scene->imageBasedLights[0].specular == pixels
                           && !cooked.diagnostics.empty() && scene_test::Same(*cooked.scene, *input)
                           && scene_test::Same(*pixels, original),
                       "HDR eligibility or one failed mip quality gate retains the entire original cube owner");
    }
    return valid;
}

struct EnvironmentCancellation
{
    uint32_t polls{0};
    uint32_t stop{UINT32_MAX};
};

bool PollEnvironmentCancellation(void* user)
{
    EnvironmentCancellation& state = *static_cast<EnvironmentCancellation*>(user);
    ++state.polls;
    return state.polls >= state.stop;
}

bool CheckEnvironmentControls()
{
    const std::shared_ptr<EnvironmentCubePixels> pixels   = MakeEnvironmentPixels(64, 1);
    const EnvironmentCubePixels                  original = *pixels;
    const std::shared_ptr<Scene>                 input    = MakeEnvironmentScene(pixels);
    SceneCookOptions                             options  = EnvironmentOptions();
    options.limits.maxAllocationBytes                     = 64 * 1024;
    const SceneCookResult bounded                         = CookScene(input, options);
    bool     valid      = Check(bounded.status.Succeeded() && bounded.scene
                                    && bounded.scene->imageBasedLights[0].specular->format == PixelFormat::BC6HUfloat,
                                "environment cooking fits below the caller-owned raw cube size without copying raw pixels");
    uint64_t remaining  = UINT64_MAX;
    valid              &= Check(detail::ReserveSceneCopy(*input, options.limits, remaining, nullptr).Succeeded(),
                                "environment scene copy storage is measurable independently of immutable pixels");
    SceneCookOptions limited          = options;
    limited.limits.maxAllocationBytes = UINT64_MAX - remaining + 1;
    SceneCookResult cooked            = CookScene(input, limited);
    valid &= Check(cooked.status.code == StatusCode::ResourceLimit && !cooked.scene,
                   "environment output and codec scratch obey the remaining cooking allocation budget");
    EnvironmentCancellation full;
    const Cancellation      observe(&PollEnvironmentCancellation, &full);
    cooked  = CookScene(input, options, &observe);
    valid  &= Check(cooked.status.Succeeded() && cooked.scene && full.polls > 100,
                    "environment pixel and block traversal polls cancellation throughout the cook");
    for (uint32_t stop : {1u, full.polls / 2, full.polls})
    {
        EnvironmentCancellation state{0, stop};
        const Cancellation      cancellation(&PollEnvironmentCancellation, &state);
        cooked  = CookScene(input, options, &cancellation);
        valid  &= Check(cooked.status.code == StatusCode::Cancelled && !cooked.scene && state.polls == stop,
                        "cancellation before, during, or after HDR encoding publishes no cooked scene");
    }
    valid &= Check(input->imageBasedLights[0].specular == pixels && scene_test::Same(*pixels, original),
                   "budget failures and cancellation never modify caller-owned environment pixels");
    return valid;
}

bool CheckInvalidEnvironments()
{
    bool valid = true;
    for (uint32_t mode = 0; mode < 8; ++mode)
    {
        const std::shared_ptr<EnvironmentCubePixels> pixels = MakeEnvironmentPixels(4, 2);
        if (mode == 0)
        {
            pixels->size = 0;
        }
        else if (mode == 1)
        {
            pixels->mipLevels = 4;
        }
        else if (mode == 2)
        {
            pixels->rgba32FloatMipFaces.pop_back();
        }
        else if (mode == 3)
        {
            pixels->rgba32FloatMipFaces.back().pop_back();
        }
        else if (mode == 4)
        {
            pixels->rgba32FloatMipFaces.back()[0].x = std::numeric_limits<float>::quiet_NaN();
        }
        else if (mode == 5)
        {
            pixels->blockMipFaces.resize(12);
        }
        else if (mode == 6)
        {
            pixels->format = PixelFormat::BC7Unorm;
        }
        const std::shared_ptr<Scene> input = MakeEnvironmentScene(pixels);
        if (mode == 7)
        {
            input->imageBasedLights[0].specular.reset();
        }
        const SceneCookResult cooked = CookScene(input, EnvironmentOptions());
        valid &= Check(cooked.status.code == StatusCode::InvalidData && !cooked.scene,
                       "invalid environment ownership, format, mip footprint, or values are rejected before cooking");
    }
    return valid;
}
} // namespace

int main(int argc, char** argv)
{
    const int64_t               tick = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("zenasset-cook-" + std::to_string(tick) + ".zenasset");
    bool valid  = CheckCooking(path);
    valid      &= CheckTextureCooking(path);
    valid      &= CheckEnvironmentCooking(path);
    valid      &= CheckEnvironmentFallbacks();
    valid      &= CheckEnvironmentControls();
    valid      &= CheckInvalidEnvironments();
    for (int index = 1; index < argc; ++index)
    {
        valid &= CheckImported(argv[index], path);
    }
    std::error_code error;
    std::filesystem::remove(path, error);
    std::printf("Scene cooking: %u checks, %s\n", checks, valid ? "passed" : "failed");
    return valid ? 0 : 1;
}
