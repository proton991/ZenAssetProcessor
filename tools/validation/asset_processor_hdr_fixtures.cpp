// Public-API HDR fixtures for independent raw-package/DirectXTex audits.
#include "ZenAsset/Processor/Cook.h"
#include "ZenAsset/Processor/Write.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace
{
using namespace zenasset;
struct Fixture
{
    const char* name;
    uint32_t    size;
    uint32_t    mips;
    uint32_t    mode;
};

std::shared_ptr<const Scene> MakeScene(const Fixture& fixture)
{
    std::shared_ptr<Scene> scene = std::make_shared<Scene>();
    scene->name                  = fixture.name;
    scene->imageBasedLight       = 0;
    scene->imageBasedLights.resize(1);
    ImageBasedLightAsset& light = scene->imageBasedLights[0];
    light.name                  = "independent HDR audit cube";
    light.intensity             = 2.5f;
    light.rotation              = {0.9238795f, 0, 0.3826834f, 0};
    light.irradianceCoefficients.assign(9, Vec3{0.5f, 0.25f, 0.125f});
    std::shared_ptr<EnvironmentCubePixels> pixels = std::make_shared<EnvironmentCubePixels>();
    pixels->size                                  = fixture.size;
    pixels->mipLevels                             = fixture.mips;
    for (uint32_t mip = 0; mip < fixture.mips; ++mip)
    {
        const uint32_t extent = std::max(1u, fixture.size >> mip);
        for (uint32_t face = 0; face < 6; ++face)
        {
            std::vector<Vec4> values;
            for (uint32_t y = 0; y < extent; ++y)
            {
                for (uint32_t x = 0; x < extent; ++x)
                {
                    const float v = float(x + y * extent) / float(extent * extent);
                    Vec4        pixel{4.0f + float(face) + v, 2.0f + float(mip) + v * 0.5f, 16.0f + v, 1};
                    if (fixture.mode == 1)
                    {
                        pixel = {0, 0, 0, 1};
                    }
                    else if (fixture.mode == 8)
                    {
                        const Vec4 colors[]{{4, 0.125f, 0.125f, 1}, {0.125f, 4, 0.125f, 1}, {0.125f, 0.125f, 4, 1},
                                            {4, 4, 0.125f, 1},      {4, 0.125f, 4, 1},      {0.125f, 4, 4, 1}};
                        pixel = colors[(face + mip) % 6];
                    }
                    values.push_back(pixel);
                }
            }
            pixels->rgba32FloatMipFaces.push_back(std::move(values));
        }
    }
    Vec4& last = pixels->rgba32FloatMipFaces.back().back();
    if (fixture.mode == 2)
    {
        last = {1e-10f, 1e-10f, 1e-10f, 1};
    }
    else if (fixture.mode == 3)
    {
        last.x = -0.25f;
    }
    else if (fixture.mode == 4)
    {
        last.y = 65505.0f;
    }
    else if (fixture.mode == 5)
    {
        last.w = 0.5f;
    }
    else if (fixture.mode == 6)
    {
        last.x = 4.123456f;
    }
    light.specular = pixels;
    return scene;
}

Status Store(const std::filesystem::path&   path,
             const Scene&                   scene,
             const SceneCookOptions&        cook,
             const std::vector<Diagnostic>& diagnostics)
{
    SceneWriteOptions options;
    options.metadata.profile     = cook.settings.textures.compressBC6HEnvironment ? "desktop" : "reference";
    options.metadata.processing  = cook.settings;
    options.metadata.diagnostics = diagnostics;
    return WriteScene(path.string(), scene, options);
}

Status WriteFixture(const std::filesystem::path&        directory,
                    const Fixture&                      fixture,
                    const std::shared_ptr<const Scene>& geometry = {})
{
    std::error_code error;
    std::filesystem::create_directory(directory / fixture.name, error);
    Status status;
    if (error)
    {
        status = {StatusCode::IOError, error.message()};
    }
    std::shared_ptr<const Scene> source = MakeScene(fixture);
    if (geometry)
    {
        std::shared_ptr<Scene> combined        = std::make_shared<Scene>(*geometry);
        combined->imageBasedLights             = source->imageBasedLights;
        combined->imageBasedLight              = 0;
        combined->imageBasedLights[0].rotation = {1, 0, 0, 0};
        source                                 = combined;
    }
    SceneCookOptions      referenceOptions;
    const SceneCookResult reference = CookScene(source, referenceOptions);
    if (status.Succeeded())
    {
        status = reference.status;
    }
    if (status.Succeeded())
    {
        status = Store(directory / fixture.name / "reference.zenasset", *reference.scene, referenceOptions, {});
    }
    if (status.Succeeded())
    {
        status = Store(directory / fixture.name / "default.zenasset", *source, referenceOptions, {});
    }
    SceneCookOptions options;
    options.settings.textures.compressBC6HEnvironment = true;
    if (fixture.mode == 6)
    {
        options.settings.textures.hdrMaxRelativeRmse = 0;
    }
    else if (fixture.mode == 7)
    {
        options.settings.textures.maxDimension = 2;
    }
    for (const char* output : {"bc6h.zenasset", "repeat.zenasset"})
    {
        if (status.Succeeded())
        {
            const SceneCookResult cooked = CookScene(source, options);
            status                       = cooked.status;
            if (status.Succeeded())
            {
                status = Store(directory / fixture.name / output, *cooked.scene, options, cooked.diagnostics);
                std::printf("%s %s format=%u owner_retained=%u\n", fixture.name, output,
                            uint32_t(cooked.scene->imageBasedLights[0].specular->format),
                            cooked.scene->imageBasedLights[0].specular == source->imageBasedLights[0].specular);
                for (const Diagnostic& diagnostic : cooked.diagnostics)
                {
                    std::printf("diagnostic: %s\n", diagnostic.message.c_str());
                }
            }
        }
    }
    return status;
}
} // namespace

int main(int argc, char** argv)
{
    Status        status;
    const Fixture fixtures[] = {{"tiny", 1, 1, 0},  {"odd3", 3, 1, 0},     {"odd5partial", 5, 2, 0}, {"odd7full", 7, 3, 0},
                                {"black", 4, 3, 1}, {"darktail", 4, 3, 2}, {"negative", 4, 3, 3},    {"overflow", 4, 3, 4},
                                {"alpha", 4, 3, 5}, {"strict", 4, 3, 6},   {"dimension", 4, 3, 7}};
    if (argc != 2 && argc != 3)
    {
        status = {StatusCode::InvalidArgument, "Expected existing output directory"};
    }
    if (status.Succeeded() && argc == 3)
    {
        const SceneReadResult geometry = ReadScene(argv[2]);
        status                         = geometry.status;
        if (status.Succeeded())
        {
            status = WriteFixture(argv[1], {"face-orientation", 5, 2, 8}, geometry.scene);
        }
    }
    for (const Fixture& fixture : fixtures)
    {
        if (status.Succeeded() && argc == 2)
        {
            status = WriteFixture(argv[1], fixture);
        }
    }
    if (!status.Succeeded())
    {
        std::fprintf(stderr, "%s\n", status.message.c_str());
    }
    return status.Succeeded() ? 0 : 1;
}
