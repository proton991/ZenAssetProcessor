#include "ZenAsset/Processor/Cook.h"
#include "ZenAsset/Processor/Write.h"
#include "ZenAsset/Format/Validation.h"
#include "Format/SceneArchive.h"
#include "SceneValueComparison.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>

namespace
{
using namespace zenasset;
uint32_t checks  = 0;
bool     spatial = false;

bool Check(bool valid, const char* message)
{
    ++checks;
    if (!valid)
    {
        std::fprintf(stderr, "%s\n", message);
    }
    return valid;
}

std::shared_ptr<Scene> Fixture(uint32_t imageCount = 3)
{
    std::shared_ptr<Scene> scene = std::make_shared<Scene>();
    scene->vertexTexCoordSets    = {0, 7, 42};
    scene->vertices.resize(3);
    scene->vertices[1].pos = Vec4(1, 0, 0, 1);
    scene->vertices[2].pos = Vec4(0, 1, 0, 1);
    scene->vertices[0].uv0 = Vec2(0.1f, 0.2f);
    scene->vertices[1].uv0 = Vec2(0.9f, 0.2f);
    scene->vertices[2].uv0 = Vec2(0.4f, 0.9f);
    for (const Vertex& vertex : scene->vertices)
    {
        scene->vertexTexCoords.push_back({vertex.uv0, vertex.uv0, vertex.uv0});
    }
    scene->indices = {0, 1, 2};
    scene->meshes.resize(1);
    scene->nodes.resize(1);
    scene->nodes[0].mesh = MeshId{0};
    scene->roots         = {NodeId{0}};
    SamplerAsset sampler;
    sampler.minFilter = sampler.magFilter = sampler.mipFilter = TextureFilter::Linear;
    sampler.wrapS = sampler.wrapT = SamplerAddressMode::ClampToEdge;
    scene->samplers.push_back(sampler);
    for (uint32_t image = 0; image < imageCount; ++image)
    {
        std::shared_ptr<ImagePixels> pixels = std::make_shared<ImagePixels>();
        pixels->width = pixels->height = 8;
        pixels->bytes.resize(8 * 8 * 4, 255);
        for (size_t pixel = 0; pixel < pixels->bytes.size(); pixel += 4)
        {
            pixels->bytes[pixel]     = static_cast<uint8_t>(40 + image * 70);
            pixels->bytes[pixel + 1] = 128;
            pixels->bytes[pixel + 2] = 160;
        }
        pixels->mips = {pixels->bytes, std::vector<uint8_t>(pixels->bytes.begin(), pixels->bytes.begin() + 4 * 4 * 4)};
        if (spatial)
        {
            pixels->mips.resize(1);
        }
        scene->images.push_back({"image " + std::to_string(image), pixels});
        TextureAsset texture;
        texture.name    = "texture " + std::to_string(image);
        texture.image   = ImageId{image};
        texture.sampler = SamplerId{0};
        texture.format  = PixelFormat::RGBA8Srgb;
        scene->textures.push_back(texture);
        MaterialAsset material;
        material.name                = "material " + std::to_string(image);
        material.m_pBaseColorTexture = TextureId{image};
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

SceneCookOptions Options()
{
    SceneCookOptions options;
    options.settings.textures.arrayPages   = !spatial;
    options.settings.textures.spatialPages = spatial;
    return options;
}

bool Preserved(const Scene& original, const Scene& cooked)
{
    bool valid =
        Check(original.textures.size() == cooked.textures.size(), "Logical texture IDs and count survive array packing");
    valid &=
        Check(scene_test::Same(original.materials, cooked.materials) && scene_test::Same(original.meshes, cooked.meshes)
                  && scene_test::Same(original.nodes, cooked.nodes) && scene_test::Same(original.animations, cooked.animations)
                  && scene_test::Same(original.variantPrimitives, cooked.variantPrimitives)
                  && scene_test::Same(original.vertices, cooked.vertices)
                  && scene_test::Same(original.vertexTexCoords, cooked.vertexTexCoords),
              "Packing retains authored materials, UV transforms, variants, instances, animation and geometry");
    for (size_t texture = 0; texture < std::min(original.textures.size(), cooked.textures.size()); ++texture)
    {
        TextureAsset expected      = original.textures[texture];
        expected.image             = cooked.textures[texture].image;
        expected.arrayLayer        = cooked.textures[texture].arrayLayer;
        expected.atlasRegion       = cooked.textures[texture].atlasRegion;
        valid                     &= Check(scene_test::Same(expected, cooked.textures[texture]),
                                           "Only image reference and layer selection change on a texture");
        const ImagePixels& source  = *original.images[original.textures[texture].image.value].pixels;
        const ImagePixels& target  = *cooked.images[cooked.textures[texture].image.value].pixels;
        if (cooked.textures[texture].atlasRegion != ImageRegion{})
        {
            const ImageRegion& region = cooked.textures[texture].atlasRegion;
            valid &= Check(region.width == source.width && region.height == source.height && target.mips.size() == 1
                               && target.bytes == target.mips[0] && target.arrayLayers == 1 && source.format == target.format,
                           "Spatial region retains logical dimensions and format in a single-level page");
            bool identical = true;
            for (int32_t y = -4; y < int32_t(region.height) + 4; ++y)
            {
                for (int32_t x = -4; x < int32_t(region.width) + 4; ++x)
                {
                    const uint32_t sx   = static_cast<uint32_t>(std::clamp(x, 0, int32_t(source.width) - 1));
                    const uint32_t sy   = static_cast<uint32_t>(std::clamp(y, 0, int32_t(source.height) - 1));
                    const size_t   from = (size_t(sy) * source.width + sx) * 4;
                    const size_t   to   = ((size_t(region.y) + y) * target.width + region.x + x) * 4;
                    identical &=
                        std::equal(source.bytes.begin() + from, source.bytes.begin() + from + 4, target.bytes.begin() + to);
                }
            }
            valid &= Check(identical, "Every tile and all four extruded gutters retain exact source texels");
        }
        else
        {
            valid &= Check(source.width == target.width && source.height == target.height && source.format == target.format
                               && source.mips.size() == target.mips.size(),
                           "Logical extent, storage format and authored mip policy survive");
            for (uint32_t level = 0; level < std::max(size_t(1), source.mips.size()); ++level)
            {
                std::span<const uint8_t> a;
                std::span<const uint8_t> b;
                valid &= Check(GetImageLayerMipBytes(source, original.textures[texture].arrayLayer, level, a)
                                   && GetImageLayerMipBytes(target, cooked.textures[texture].arrayLayer, level, b)
                                   && std::equal(a.begin(), a.end(), b.begin(), b.end()),
                               "Every logical texture mip retains identical bytes");
            }
        }
    }
    return valid;
}

bool HasReason(const SceneCookResult& result, const char* text)
{
    bool found = false;
    for (const Diagnostic& diagnostic : result.diagnostics)
    {
        found |= diagnostic.message.find(text) != std::string::npos;
    }
    return found;
}

bool CookAndCheck(const std::shared_ptr<Scene>& source,
                  size_t                        images,
                  const char*                   reason  = nullptr,
                  SceneCookOptions              options = Options())
{
    std::string           failure;
    bool                  valid     = Check(ValidateSceneAsset(*source, UINT64_MAX, &failure), failure.c_str());
    const Scene           snapshot  = *source;
    const SceneCookResult cooked    = CookScene(source, options);
    valid                          &= Check(cooked.status.Succeeded() && cooked.scene, cooked.status.message.c_str());
    valid                          &= Check(scene_test::Same(snapshot, *source), "Cooking never changes the input scene");
    if (cooked.scene)
    {
        valid &= Check(cooked.scene->images.size() == images, "Qualified groups pack while excluded images remain standalone");
        valid &= Preserved(*source, *cooked.scene);
    }
    if (reason)
    {
        valid &= Check(HasReason(cooked, reason), "Excluded image has the expected explicit diagnostic");
    }
    return valid;
}

bool TestGroups()
{
    std::shared_ptr<Scene> source                   = Fixture();
    source->textures[1].linearSource                = TextureId{0};
    source->materials[1].texCoordSets.baseColor     = 2;
    source->materials[1].textureTransforms[0].row0  = Vec4(0.5f, 0, 0.25f, 0);
    bool             valid                          = CookAndCheck(source, 1);
    SceneCookOptions split                          = Options();
    split.settings.textures.arrayMaxLayers          = 2;
    valid                                          &= CookAndCheck(source, 2, "no compatible partner", split);
    valid                                          &= CookAndCheck(Fixture(7), 4, "no compatible partner", split);
    source                                          = Fixture();
    for (uint32_t image = 0; image < 3; ++image)
    {
        source->textures.push_back(source->textures[image]);
        source->materials[image].m_pEmissiveTexture = TextureId{image + 3};
    }
    valid &= CookAndCheck(source, 1);
    SceneCookOptions      off;
    const SceneCookResult unchanged = CookScene(source, off);
    valid &=
        Check(unchanged.status.Succeeded() && unchanged.scene == source, "Array pages default off preserves the input owner");
    const SceneCookResult packed = CookScene(source, Options());
    if (packed.scene)
    {
        const SceneCookResult again = CookScene(packed.scene, Options());
        valid &= Check(again.scene && scene_test::Same(*again.scene, *packed.scene) && HasReason(again, "Existing array page"),
                       "Already packed pages retain layer references when recooked");
    }
    source                                 = Fixture();
    source->materials[0].m_pNormalTexture  = TextureId{0};
    valid                                 &= CookAndCheck(source, 2, "no compatible partner");
    source                                 = Fixture();
    source->textures[0].format             = PixelFormat::RGBA8Unorm;
    valid                                 &= CookAndCheck(source, 2, "no compatible partner");
    source                                 = Fixture();
    std::shared_ptr<ImagePixels> partial   = std::make_shared<ImagePixels>(*source->images[0].pixels);
    partial->mips.resize(1);
    source->images[0].pixels  = partial;
    valid                    &= CookAndCheck(source, 2, "no compatible partner");
    partial->mips.clear();
    valid  &= CookAndCheck(source, 2, "finalized explicit mips");
    source  = Fixture();
    source->textures.push_back(source->textures[0]);
    valid  &= CookAndCheck(source, 2, "catalog or unbound");
    source  = Fixture();
    source->samplers.push_back(source->samplers[0]);
    source->samplers[1].useMipmaps = false;
    source->textures.push_back(source->textures[0]);
    source->textures.back().sampler          = SamplerId{1};
    source->materials[0].m_pEmissiveTexture  = TextureId{3};
    valid                                   &= CookAndCheck(source, 2, "different sampler policies");
    for (SamplerAddressMode wrap :
         {SamplerAddressMode::Repeat, SamplerAddressMode::MirroredRepeat, SamplerAddressMode::ClampToBorder,
          SamplerAddressMode::MirrorClampToEdge, SamplerAddressMode::Unspecified})
    {
        source = Fixture();
        source->samplers.push_back(source->samplers[0]);
        source->samplers[1].wrapS    = wrap;
        source->textures[0].sampler  = SamplerId{1};
        valid                       &= CookAndCheck(source, 2, "explicit clamp-to-edge");
    }
    source                                     = Fixture();
    source->textures[0].sampler                = {};
    valid                                     &= CookAndCheck(source, 2, "explicit clamp-to-edge");
    source                                     = Fixture();
    source->samplers[0].minFilter              = TextureFilter::Nearest;
    valid                                     &= CookAndCheck(source, 3, "linear filtering");
    SceneCookOptions small                     = Options();
    small.settings.textures.arrayMaxDimension  = 4;
    valid                                     &= CookAndCheck(Fixture(), 3, "page dimension", small);
    return valid;
}

bool TestUses()
{
    std::shared_ptr<Scene> source         = Fixture();
    MaterialAsset          alternate      = source->materials[0];
    alternate.textureTransforms[0].row0.z = 2;
    source->materials.push_back(alternate);
    source->materialVariants = {"alternate"};
    source->variantPrimitives.push_back({0, 0, {3}});
    bool valid = CookAndCheck(source, 2, "effective authored UVs");
    source->variantPrimitives.clear();
    source->materialVariants.clear();
    NodeAsset instance = source->nodes[0];
    instance.materials = {MaterialId{3}, MaterialId{1}, MaterialId{2}};
    source->nodes.push_back(instance);
    source->roots.push_back(NodeId{1});
    valid                                       &= CookAndCheck(source, 2, "effective authored UVs");
    source                                       = Fixture();
    source->materials[0].texCoordSets.baseColor  = 2;
    source->vertexTexCoords[1][2].x              = 2;
    valid                                       &= CookAndCheck(source, 2, "effective authored UVs");
    source->vertexTexCoords[1].resize(2);
    valid  &= CookAndCheck(source, 2, "missing UV");
    source  = Fixture();
    AnimationAsset animation;
    animation.samplers.push_back({AnimationInterpolation::Linear, 2, {0, 1}, {0, 0, 0.2f, 0}});
    AnimationChannel channel;
    channel.path   = AnimationPath::Property;
    channel.target = {AnimatedObject::Material, 0, AnimatedProperty::TextureOffset, 2, UINT32_MAX, 0};
    animation.channels.push_back(channel);
    source->animations.push_back(animation);
    valid                                    &= CookAndCheck(source, 2, "animated UV");
    source->animations[0].samplers[0]         = {AnimationInterpolation::Linear, 1, {0, 1}, {0.4f, 0.6f}};
    source->animations[0].channels[0].target  = {AnimatedObject::Material, 0, AnimatedProperty::AlphaCutoff, 1};
    valid                                    &= CookAndCheck(source, 1);
    source                                    = Fixture();
    MorphPrimitiveAsset morph;
    morph.mesh      = 0;
    morph.primitive = 0;
    morph.targets.resize(1);
    morph.targets[0].texCoords = {std::vector<Vec2>(3)};
    source->morphPrimitives.push_back(morph);
    valid                                                                          &= CookAndCheck(source, 2, "UV morph");
    source                                                                          = Fixture();
    source->materials[0].m_pBaseColorTexture                                        = {};
    source->materials[0].features.diffuseTransmissionColorTexture.texture           = TextureId{0};
    source->materials[0].features.diffuseTransmissionColorTexture.texCoord          = 2;
    source->materials[0].features.diffuseTransmissionColorTexture.transform.row1.z  = 2;
    valid &= CookAndCheck(source, 2, "effective authored UVs");
    return valid;
}

bool TestCompressionAndMetadata(const std::filesystem::path& path)
{
    bool valid = true;
    for (uint32_t role = 0; role < 3; ++role)
    {
        const std::shared_ptr<Scene> source = Fixture();
        SceneCookOptions             plain;
        plain.settings.textures.generateMipmaps      = true;
        plain.settings.textures.compressBC7          = role == 0;
        plain.settings.textures.compressBC4Occlusion = role == 1;
        plain.settings.textures.compressBC5Normals   = role == 2;
        for (uint32_t image = 0; image < 3 && role != 0; ++image)
        {
            MaterialAsset& material                                                = source->materials[image];
            material.m_pBaseColorTexture                                           = {};
            (role == 1 ? material.m_pOcclusionTexture : material.m_pNormalTexture) = TextureId{image};
            source->textures[image].format                                         = PixelFormat::RGBA8Unorm;
            if (role == 2)
            {
                std::shared_ptr<ImagePixels> normal = std::make_shared<ImagePixels>(*source->images[image].pixels);
                for (std::vector<uint8_t>& mip : normal->mips)
                {
                    for (size_t offset = 0; offset < mip.size(); offset += 4)
                    {
                        mip[offset] = mip[offset + 1] = 128;
                        mip[offset + 2] = mip[offset + 3] = 255;
                    }
                }
                normal->bytes                = normal->mips[0];
                source->images[image].pixels = normal;
            }
        }
        const SceneCookResult reference            = CookScene(source, plain);
        SceneCookOptions      pages                = plain;
        pages.settings.textures.arrayPages         = true;
        pages.settings.textures.arrayMaxLayers     = 4;
        pages.settings.textures.arrayMaxDimension  = 512;
        pages.settings.textures.arrayMaxPages      = 8;
        pages.settings.textures.arrayMaxImages     = 16;
        const SceneCookResult cooked               = CookScene(source, pages);
        valid                                     &= Check(reference.scene && cooked.scene && cooked.scene->images.size() == 1,
                                                           "Compression followed by array cooking creates one page");
        if (reference.scene && cooked.scene)
        {
            valid                      &= Preserved(*reference.scene, *cooked.scene);
            const PixelFormat expected  = role == 0 ? PixelFormat::BC7Srgb
                                        : role == 1 ? PixelFormat::BC4Unorm
                                                    : PixelFormat::BC5Unorm;
            valid                      &= Check(cooked.scene->images[0].pixels->format == expected,
                                                "The test exercises actual role-selected encoded bytes");
            for (TransportCodec codec : {TransportCodec::None, TransportCodec::Zstandard})
            {
                SceneWriteOptions write;
                write.compression.transport = codec;
                write.metadata.profile      = "desktop";
                write.metadata.processing   = pages.settings;
                valid &= Check(WriteScene(path.string(), *cooked.scene, write).Succeeded(), "Cooked array package writes");
                const SceneReadResult read = ReadScene(path.string());
                valid &= Check(read.scene && scene_test::Same(*read.scene, *cooked.scene)
                                   && read.metadata.processing == pages.settings,
                               "Metadata v7 and complete array scene round-trip under raw/Zstandard transport");
            }
        }
    }
    BuildMetadata metadata;
    metadata.processing.textures.generateMipmaps = true;
    std::vector<std::byte>      bytes;
    SceneReadLimits             limits;
    uint64_t                    budget = limits.maxAllocationBytes;
    detail::SceneArchive<false> sizing({}, nullptr, limits, budget, nullptr, 6);
    sizing(metadata);
    valid &= Check(sizing.Finish().Succeeded(), "Legacy metadata can be sized");
    bytes.resize(sizing.ByteSize());
    budget = limits.maxAllocationBytes;
    detail::SceneArchive<false> writer({}, &bytes, limits, budget, nullptr, 6);
    writer(metadata);
    valid &= Check(writer.Finish().Succeeded(), "Legacy metadata fixture writes with version six schema");
    BuildMetadata legacy;
    budget = limits.maxAllocationBytes;
    detail::SceneArchive<true> reader(bytes, nullptr, limits, budget, nullptr, 6);
    reader(legacy);
    valid &=
        Check(reader.Finish().Succeeded() && legacy.processing == metadata.processing && !legacy.processing.textures.arrayPages,
              "Metadata v6 defaults array processing off while preserving prior settings");
    return valid;
}

struct PollState
{
    uint32_t count{0};
    uint32_t stop{0};
};
bool CancelAt(void* user)
{
    PollState& state = *static_cast<PollState*>(user);
    return ++state.count >= state.stop;
}

bool TestFailures()
{
    const std::shared_ptr<Scene> source   = Fixture();
    const Scene                  snapshot = *source;
    bool                         valid    = true;
    uint32_t                     failed   = 0;
    uint32_t                     passed   = 0;
    for (uint64_t budget = 4096; budget < 32768; budget += 257)
    {
        SceneCookOptions options           = Options();
        options.limits.maxAllocationBytes  = budget;
        const SceneCookResult result       = CookScene(source, options);
        failed                            += !result.status.Succeeded();
        passed                            += result.status.Succeeded();
        valid &= Check(result.status.Succeeded() ? result.scene && result.scene->images.size() == 1
                                                 : result.status.code == StatusCode::ResourceLimit && !result.scene,
                       "Allocation boundaries either publish a complete array scene or fail without output");
        valid &= Check(scene_test::Same(snapshot, *source), "Allocation failure leaves input untouched");
    }
    valid &= Check(failed != 0 && passed != 0, "Budget sweep crosses both rejection and successful publication");
    PollState             total{0, UINT32_MAX};
    Cancellation          counter(CancelAt, &total);
    const SceneCookResult counted = CookScene(source, Options(), &counter);
    valid &= Check(counted.scene != nullptr, "Uncancelled analysis establishes cancellation poll coverage");
    for (uint32_t stop = 1; stop <= total.count; stop += 7)
    {
        PollState             state{0, stop};
        Cancellation          cancellation(CancelAt, &state);
        const SceneCookResult result = CookScene(source, Options(), &cancellation);
        valid &=
            Check(result.status.code == StatusCode::Cancelled && !result.scene && scene_test::Same(snapshot, *source),
                  "Cancellation across copying, eligibility, packing and publication exposes no scene and preserves input");
    }
    SceneCookOptions options = Options();
    if (spatial)
    {
        options.settings.textures.spatialMaxImages = 2;
    }
    else
    {
        options.settings.textures.arrayMaxImages = 2;
    }
    const SceneCookResult limited = CookScene(source, options);
    valid &= Check(limited.status.code == StatusCode::ResourceLimit && !limited.scene, "Configured image count bounds cooking");
    return valid;
}
void SpatialPixels(Scene& scene, uint32_t image, uint32_t width, uint32_t height, bool explicitMip = true)
{
    std::shared_ptr<ImagePixels> pixels = std::make_shared<ImagePixels>();
    pixels->width                       = width;
    pixels->height                      = height;
    pixels->bytes.resize(size_t(width) * height * 4);
    for (size_t byte = 0; byte < pixels->bytes.size(); ++byte)
    {
        pixels->bytes[byte] = static_cast<uint8_t>((byte * 31 + image * 17) % 256);
    }
    if (explicitMip)
    {
        pixels->mips = {pixels->bytes};
    }
    scene.images[image].pixels = pixels;
}

bool TestSpatial(const std::filesystem::path& path)
{
    std::shared_ptr<Scene> source = Fixture(7);
    const uint32_t         sizes[]{1, 2, 4, 8, 16, 4, 8};
    for (uint32_t image = 0; image < 7; ++image)
    {
        SpatialPixels(*source, image, sizes[image], sizes[6 - image]);
    }
    source->materials[0].textureTransforms[0].row0 = Vec4(0.5f, 0, 0.25f, 0);
    source->textures[1].linearSource               = TextureId{0};
    bool                  valid                    = CookAndCheck(source, 1);
    const SceneCookResult cooked                   = CookScene(source, Options());
    const SceneCookResult repeated                 = CookScene(source, Options());
    valid &= Check(cooked.scene && repeated.scene && scene_test::Same(*cooked.scene, *repeated.scene),
                   "Spatial construction is deterministic");
    if (cooked.scene)
    {
        const SceneCookResult recooked  = CookScene(cooked.scene, Options());
        valid                          &= Check(recooked.scene && scene_test::Same(*cooked.scene, *recooked.scene)
                                                    && HasReason(recooked, "Existing spatial page"),
                                                "Recooking preserves existing spatial pages");
        for (TransportCodec codec : {TransportCodec::None, TransportCodec::Zstandard})
        {
            SceneWriteOptions write;
            write.compression.transport = codec;
            write.metadata.processing   = Options().settings;
            valid &= Check(WriteScene(path.string(), *cooked.scene, write).Succeeded(), "Cooked spatial package writes");
            const SceneReadResult read = ReadScene(path.string());
            valid &= Check(read.scene && scene_test::Same(*read.scene, *cooked.scene)
                               && read.metadata.processing == write.metadata.processing,
                           "Texture v4 and spatial metadata v8 round-trip under raw and Zstandard transport");
        }
    }
    source                                         = Fixture(5);
    SceneCookOptions bounded                       = Options();
    bounded.settings.textures.spatialMaxDimension  = 32;
    valid                                         &= CookAndCheck(source, 2, "no compatible partner", bounded);
    bounded.settings.textures.spatialMaxPages      = 1;
    const SceneCookResult limited                  = CookScene(source, bounded);
    valid                                         &= Check(limited.status.code == StatusCode::ResourceLimit && !limited.scene,
                                                           "Page limit failure exposes no partial spatial scene");
    bounded.settings.textures.spatialMaxPages      = 64;
    bounded.settings.textures.spatialMaxDimension  = 16;
    valid                                         &= CookAndCheck(source, 5, "no compatible partner", bounded);
    source                                         = Fixture();
    SpatialPixels(*source, 0, 7, 8);
    valid &= CookAndCheck(source, 2, "power-of-two tile");
    SpatialPixels(*source, 0, 2048, 1);
    valid &= CookAndCheck(source, 2, "including gutters");
    SpatialPixels(*source, 0, 8, 8, false);
    valid                             &= CookAndCheck(source, 2, "implicit mip generation");
    source->samplers[0].useMipmaps     = false;
    valid                             &= CookAndCheck(source, 1);
    std::shared_ptr<ImagePixels> mips  = std::make_shared<ImagePixels>(*source->images[1].pixels);
    mips->mips.push_back(std::vector<uint8_t>(4 * 4 * 4, 127));
    source->images[1].pixels  = mips;
    valid                    &= CookAndCheck(source, 2, "authored mip chains");
    source                    = Fixture();
    source->samplers.push_back(source->samplers[0]);
    source->samplers[1].wrapS    = SamplerAddressMode::Repeat;
    source->textures[0].sampler  = SamplerId{1};
    valid                       &= CookAndCheck(source, 2, "explicit clamp-to-edge");
    source                       = Fixture();
    source->textures.push_back(source->textures[0]);
    valid  &= CookAndCheck(source, 2, "catalog or unbound");
    source  = Fixture();
    source->textures.push_back(source->textures[0]);
    source->materials[0].m_pEmissiveTexture = TextureId{3};
    source->samplers.push_back(source->samplers[0]);
    source->samplers[1].useMipmaps         = false;
    source->textures[3].sampler            = SamplerId{1};
    valid                                 &= CookAndCheck(source, 2, "different sampler policies");
    source                                 = Fixture();
    source->materials[0].m_pNormalTexture  = TextureId{0};
    valid                                 &= CookAndCheck(source, 2, "no compatible partner");
    source                                 = Fixture();
    source->textures[0].format             = PixelFormat::RGBA8Unorm;
    valid                                 &= CookAndCheck(source, 2, "no compatible partner");
    source                                 = Fixture();
    SceneCookOptions compressed;
    compressed.settings.textures.compressBC7 = true;
    const SceneCookResult blocks             = CookScene(source, compressed);
    valid &= Check(blocks.scene && blocks.scene->images[0].pixels->format == PixelFormat::BC7Srgb,
                   "Spatial exclusion test uses actual encoded image storage");
    if (blocks.scene)
    {
        const SceneCookResult retained  = CookScene(blocks.scene, Options());
        valid                          &= Check(retained.scene && scene_test::Same(*retained.scene, *blocks.scene)
                                                    && HasReason(retained, "uncompressed RGBA8"),
                                                "Spatial pages preserve unsupported BC payloads");
    }
    // Metadata v7 remains readable and initializes the added settings to off/defaults.
    BuildMetadata metadata;
    metadata.processing.textures.arrayPages = true;
    SceneReadLimits             limits;
    uint64_t                    budget = limits.maxAllocationBytes;
    detail::SceneArchive<false> sizing({}, nullptr, limits, budget, nullptr, 7);
    sizing(metadata);
    valid &= Check(sizing.Finish().Succeeded(), "Legacy v7 metadata sizes");
    std::vector<std::byte> bytes(sizing.ByteSize());
    budget = limits.maxAllocationBytes;
    detail::SceneArchive<false> writer({}, &bytes, limits, budget, nullptr, 7);
    writer(metadata);
    valid &= Check(writer.Finish().Succeeded(), "Legacy v7 metadata writes");
    BuildMetadata legacy;
    budget = limits.maxAllocationBytes;
    detail::SceneArchive<true> reader(bytes, nullptr, limits, budget, nullptr, 7);
    reader(legacy);
    valid &= Check(reader.Finish().Succeeded() && legacy.processing == metadata.processing,
                   "Legacy metadata defaults spatial processing off without losing array settings");
    return valid;
}
} // namespace

int main()
{
    const int64_t               tick   = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path path   = std::filesystem::current_path() / ("array-cook-" + std::to_string(tick) + ".zenasset");
    bool                        valid  = TestGroups();
    valid                             &= TestUses();
    valid                             &= TestCompressionAndMetadata(path);
    valid                             &= TestFailures();
    spatial                            = true;
    valid                             &= TestSpatial(path);
    valid                             &= TestUses();
    valid                             &= TestFailures();
    std::printf("%u array and spatial cooking checks\n", checks);
    return valid ? 0 : 1;
}
