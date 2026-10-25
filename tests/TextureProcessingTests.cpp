#include "Processing/TextureProcessing.h"
#include "SceneValueComparison.h"
#include "ZenAsset/Format/Validation.h"
#include <cstdio>

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

Scene MakeScene()
{
    Scene scene;
    scene.name               = "texture role fixture";
    scene.vertexTexCoordSets = {0, 7};
    scene.materials.resize(1);
    scene.materials[0].m_pBaseColorTexture = TextureId{0};
    std::shared_ptr<ImagePixels> pixels    = std::make_shared<ImagePixels>();
    pixels->width = pixels->height = 16;
    pixels->bytes.resize(16 * 16 * 4, 255);
    for (size_t offset = 0; offset < pixels->bytes.size(); offset += 4)
    {
        pixels->bytes[offset] = pixels->bytes[offset + 1] = 128;
    }
    scene.images.push_back({"owned source", pixels});
    scene.textures.push_back({"shared view", ImageId{0}, {}, {}, PixelFormat::RGBA8Srgb, false});
    scene.vertices.resize(3);
    scene.vertices[1].pos = Vec4(1, 0, 0, 1);
    scene.vertices[2].pos = Vec4(0, 1, 0, 1);
    scene.indices         = {0, 1, 2};
    scene.meshes.resize(1);
    MeshPrimitiveAsset primitive;
    primitive.vertexCount = primitive.indexCount = 3;
    primitive.bounds                             = Bounds(Vec3(0), Vec3(1, 1, 0));
    scene.meshes[0].primitives.push_back(primitive);
    scene.nodes.resize(1);
    scene.nodes[0].mesh = MeshId{0};
    scene.roots         = {NodeId{0}};
    return scene;
}

TextureSettings Settings()
{
    TextureSettings settings;
    settings.compressBC7     = true;
    settings.generateMipmaps = true;
    return settings;
}

bool Process(Scene& scene, std::vector<Diagnostic>& diagnostics, const TextureSettings& settings = Settings())
{
    uint64_t     remaining = 64u * 1024u * 1024u;
    std::string  reason;
    const bool   valid  = Check(ValidateSceneAsset(scene, UINT64_MAX, &reason), reason.c_str());
    const Status status = detail::ProcessTextures(scene, settings, remaining, diagnostics, nullptr);
    return valid && Check(status.Succeeded(), status.message.c_str());
}

bool TestRolesAndAliases()
{
    Scene          scene                 = MakeScene();
    MaterialAsset& material              = scene.materials[0];
    material.m_pMetallicRoughnessTexture = material.m_pNormalTexture = material.m_pOcclusionTexture =
        material.m_pEmissiveTexture                                  = TextureId{0};
    material.extension.pDiffuseTexture = material.extension.pSpecularGlossinessTexture = TextureId{0};
    for (size_t slot = 0; slot < MaterialFeatures::textureMembers.size(); ++slot)
    {
        MaterialTextureBinding& binding = material.features.*MaterialFeatures::textureMembers[slot];
        binding.texture                 = TextureId{0};
        binding.texCoord                = 1;
        binding.scale                   = slot == static_cast<size_t>(MaterialFeatureTexture::ClearcoatNormal) ? 1.0f : 0.5f;
        binding.transform.uvOffset      = Vec2(float(slot), 2);
    }
    const Scene             original = scene;
    std::vector<Diagnostic> diagnostics;
    bool                    valid  = Process(scene, diagnostics);
    const MaterialAsset&    cooked = scene.materials[0];
    valid &= Check(scene.textures.size() == 3, "shared color/data/normal use produces three stable texture variants");
    valid &= Check(cooked.m_pBaseColorTexture == TextureId{0} && cooked.m_pEmissiveTexture == TextureId{0}
                       && cooked.m_pMetallicRoughnessTexture == TextureId{1} && cooked.m_pOcclusionTexture == TextureId{1}
                       && cooked.m_pNormalTexture == TextureId{2},
                   "all five core roles are remapped consistently");
    const uint32_t expected[]{1, 0, 0, 0, 1, 1, 2, 0, 1, 1, 1, 1, 1, 1, 1, 0, 1};
    for (size_t slot = 0; slot < MaterialFeatures::textureMembers.size(); ++slot)
    {
        const MaterialTextureBinding& binding = cooked.features.*MaterialFeatures::textureMembers[slot];
        const MaterialTextureBinding& before  = original.materials[0].features.*MaterialFeatures::textureMembers[slot];
        valid &= Check(binding.texture.value == expected[slot], "feature slot receives its semantic role");
        valid &=
            Check(binding.texCoord == before.texCoord && binding.scale == before.scale && binding.transform == before.transform,
                  "role splitting preserves UV selection, transforms and binding scale");
    }
    valid &= Check(cooked.extension.pDiffuseTexture == cooked.features.diffuseTexture.texture
                       && cooked.extension.pSpecularGlossinessTexture == cooked.features.specularGlossinessTexture.texture,
                   "extension aliases resolve to their feature binding variants");
    valid &= Check(scene_test::Same(original.images[0], MakeScene().images[0]), "source image owner remains unchanged");
    Scene repeat = original;
    diagnostics.clear();
    valid &= Process(repeat, diagnostics);
    valid &= Check(scene_test::Same(scene, repeat), "role allocation and image output are deterministic");
    return valid;
}

bool TestNormalSamplerQuality()
{
    Scene                        scene  = MakeScene();
    std::shared_ptr<ImagePixels> pixels = std::make_shared<ImagePixels>(*scene.images[0].pixels);
    for (uint32_t y = 0; y < pixels->height; ++y)
    {
        for (uint32_t x = 0; x < pixels->width; ++x)
        {
            const size_t offset       = (size_t(y) * pixels->width + x) * 4;
            pixels->bytes[offset]     = x + 1u == pixels->width ? 230 : 26;
            pixels->bytes[offset + 1] = 128;
            pixels->bytes[offset + 2] = 204;
        }
    }
    scene.images[0].pixels   = pixels;
    scene.textures[0].format = PixelFormat::RGBA8Unorm;
    scene.textures.resize(3, scene.textures[0]);
    scene.samplers.resize(3);
    scene.materials.resize(3);
    for (uint32_t index = 0; index < 3; ++index)
    {
        scene.textures[index].sampler   = SamplerId{index};
        scene.samplers[index].minFilter = scene.samplers[index].magFilter =
            index == 0 ? TextureFilter::Nearest : TextureFilter::Linear;
        scene.samplers[index].useMipmaps = false;
        scene.samplers[index].wrapS      = scene.samplers[index].wrapT =
            index == 2 ? SamplerAddressMode::ClampToBorder : SamplerAddressMode::Repeat;
        scene.materials[index].m_pBaseColorTexture = {};
        scene.materials[index].m_pNormalTexture    = TextureId{index};
    }
    TextureSettings settings;
    settings.compressBC5Normals = true;
    std::vector<Diagnostic> diagnostics;
    bool                    valid = Process(scene, diagnostics, settings);
    if (valid)
    {
        const TextureAsset& nearest  = scene.textures[scene.materials[0].m_pNormalTexture.value];
        const TextureAsset& linear   = scene.textures[scene.materials[1].m_pNormalTexture.value];
        const TextureAsset& border   = scene.textures[scene.materials[2].m_pNormalTexture.value];
        valid                       &= Check(nearest.format == PixelFormat::BC5Unorm && nearest.sampler == SamplerId{0},
                                             "Nearest-only normal sampling retains compression when texel errors pass");
        valid                       &= Check(linear.format == PixelFormat::RGBA8Unorm && linear.sampler == SamplerId{1}
                                                 && scene.images[linear.image.value].pixels == pixels,
                                             "The shared image's linear use preserves its original normal arc");
        valid &= Check(border.format == PixelFormat::RGBA8Unorm && border.sampler == SamplerId{2}
                           && border.image == linear.image && scene.images.size() == 2,
                       "Unqualified runtime-border use shares original pixels with the rejected linear use");
    }
    return valid;
}

bool TestNormalMipSamplers()
{
    Scene                        scene  = MakeScene();
    std::shared_ptr<ImagePixels> pixels = std::make_shared<ImagePixels>();
    pixels->width = pixels->height = 8;
    for (uint32_t extent = 8; extent; extent /= 2)
    {
        std::vector<uint8_t> level(size_t(extent) * extent * 4);
        for (size_t offset = 0; offset < level.size(); offset += 4)
        {
            level[offset]     = extent == 8 ? 216 : 39;
            level[offset + 1] = 179;
            level[offset + 2] = 203;
            level[offset + 3] = 255;
        }
        pixels->mips.push_back(std::move(level));
    }
    pixels->bytes          = pixels->mips[0];
    scene.images[0].pixels = pixels;
    scene.textures.resize(3, scene.textures[0]);
    scene.samplers.resize(3);
    scene.materials.resize(3);
    for (uint32_t index = 0; index < 3; ++index)
    {
        scene.textures[index].sampler   = SamplerId{index};
        scene.textures[index].format    = PixelFormat::RGBA8Unorm;
        scene.samplers[index].minFilter = scene.samplers[index].magFilter =
            index == 2 ? TextureFilter::Linear : TextureFilter::Nearest;
        scene.samplers[index].mipFilter            = index == 0 ? TextureFilter::Nearest : TextureFilter::Linear;
        scene.materials[index].m_pBaseColorTexture = {};
        scene.materials[index].m_pNormalTexture    = TextureId{index};
    }
    TextureSettings settings;
    settings.compressBC5Normals = true;
    std::vector<Diagnostic> diagnostics;
    bool                    valid = Process(scene, diagnostics, settings);
    if (valid)
    {
        for (uint32_t index = 0; index < 3; ++index)
        {
            const TextureAsset& texture = scene.textures[scene.materials[index].m_pNormalTexture.value];
            valid &= Check(texture.sampler == SamplerId{index}
                               && texture.format == (index == 0 ? PixelFormat::BC5Unorm : PixelFormat::RGBA8Unorm),
                           "Shared normal uses distinguish mip blending from texel filtering");
            if (index != 0)
            {
                valid &= Check(scene.images[texture.image.value].pixels == pixels,
                               "Rejected mip-blended uses retain the original authored chain owner");
            }
        }
    }
    return valid;
}

bool TestViewTransferSamplersAndUnboundResources()
{
    Scene scene = MakeScene();
    scene.samplers.resize(2);
    scene.samplers[0].useMipmaps = true;
    scene.samplers[1].useMipmaps = false;
    scene.textures[0].sampler    = SamplerId{0};
    TextureAsset linear          = scene.textures[0];
    linear.name                  = "linear source view";
    linear.format                = PixelFormat::RGBA8Unorm;
    linear.linearSource          = TextureId{0};
    scene.textures.push_back(linear);
    TextureAsset noMips = scene.textures[0];
    noMips.sampler      = SamplerId{1};
    scene.textures.push_back(noMips);
    TextureAsset shared = scene.textures[0];
    scene.textures.push_back(shared);
    TextureAsset unbound = scene.textures[0];
    scene.textures.push_back(unbound);
    scene.materials.resize(4, scene.materials[0]);
    for (uint32_t index = 0; index < 4; ++index)
    {
        scene.materials[index].m_pBaseColorTexture = TextureId{index};
    }
    const std::shared_ptr<const ImagePixels> original = scene.images[0].pixels;
    scene.images.push_back({"unreferenced image", original});
    std::vector<Diagnostic> diagnostics;
    TextureSettings         settings  = Settings();
    settings.generateMipmaps          = false;
    bool valid                        = Process(scene, diagnostics, settings);
    valid                            &= Check(scene.textures.size() == 5 && scene.images.size() == 4,
                                              "equal image roles share processed storage; unused image records are compacted");
    valid &= Check(scene.textures[0].format == PixelFormat::BC7Srgb && scene.textures[1].format == PixelFormat::BC7Unorm,
                   "the texture view transfer function is retained independently of source storage");
    valid &= Check(scene.textures[1].linearSource == TextureId{0}, "linearSource keeps its stable authored texture identity");
    valid &= Check(scene.textures[0].image == scene.textures[3].image, "equivalent views reuse one processed image");
    valid &= Check(scene.images[scene.textures[0].image.value].pixels->mips.size() == 5
                       && scene.images[scene.textures[2].image.value].pixels->mips.size() == 1,
                   "sampler mip requirements split otherwise shared image uses");
    valid &= Check(scene.images[scene.textures[4].image.value].pixels == original,
                   "unbound authored texture remains available with original unknown-role storage");
    return valid;
}

bool TestMaskFactorsAndAnimationIdentity()
{
    Scene scene = MakeScene();
    scene.materials.resize(3, scene.materials[0]);
    for (MaterialAsset& material : scene.materials)
    {
        material.alphaMode = AlphaMode::Mask;
    }
    scene.materials[0].alphaCutoff       = 0.25f;
    scene.materials[1].alphaCutoff       = 0.5f;
    scene.materials[1].baseColorFactor.w = 0.5f;
    scene.materials[2].alphaCutoff       = 0.75f;
    scene.animations.resize(1);
    AnimationChannel cutoff;
    cutoff.path                  = AnimationPath::Property;
    cutoff.target                = {AnimatedObject::Material, 2, AnimatedProperty::AlphaCutoff, 1};
    AnimationChannel transform   = cutoff;
    transform.sampler            = 1;
    transform.target             = {AnimatedObject::Material, 1, AnimatedProperty::TextureOffset, 2, UINT32_MAX, 0};
    scene.animations[0].channels = {cutoff, transform};
    scene.animations[0].samplers = {{AnimationInterpolation::Linear, 1, {0, 1}, {0.5f, 0.75f}},
                                    {AnimationInterpolation::Linear, 2, {0, 1}, {0, 0, 1, 1}}};
    scene.materialVariants       = {"original", "alternative"};
    scene.variantPrimitives.push_back({0, 0, {0, 2}});
    scene.nodes.resize(1);
    scene.nodes[0].materials                          = {MaterialId{1}, MaterialId{2}};
    const Scene                              original = scene;
    const std::shared_ptr<const ImagePixels> input    = scene.images[0].pixels;
    std::vector<Diagnostic>                  diagnostics;
    bool                                     valid = Process(scene, diagnostics);
    valid &=
        Check(scene.textures.size() == 3, "different mask cutoffs/factors and animated opacity receive independent variants");
    const TextureId animated  = scene.materials[2].m_pBaseColorTexture;
    valid                    &= Check(scene.images[scene.textures[animated.value].image.value].pixels == input,
                                      "animated cutoff retains exact alpha and the original incomplete mip representation");
    valid                    &= Check(scene_test::Same(scene.animations, original.animations)
                                          && scene_test::Same(scene.variantPrimitives, original.variantPrimitives)
                                          && scene_test::Same(scene.materialVariants, original.materialVariants)
                                          && scene_test::Same(scene.nodes, original.nodes),
                                      "material/UV animation targets, variants and node material identities are unchanged");
    valid                    &= Check(!diagnostics.empty(), "conservative animated opacity fallback is explicit");
    return valid;
}

bool TestSourceLifetimeAndDisabledPolicy()
{
    Scene                            cooked;
    std::weak_ptr<const ImagePixels> original;
    std::vector<Diagnostic>          diagnostics;
    bool                             valid = true;
    {
        Scene input  = MakeScene();
        original     = input.images[0].pixels;
        cooked       = input;
        valid       &= Process(cooked, diagnostics);
        valid       &= Check(cooked.images.size() == 1 && cooked.images[0].pixels->format == PixelFormat::BC7Srgb,
                             "color-only conversion leaves no obsolete source image record");
        valid       &= Check(input.images[0].pixels->format == PixelFormat::RGBA8Unorm && input.images[0].pixels->mips.empty(),
                             "processing borrows source pixels without mutating them");
    }
    valid          &= Check(original.expired() && !cooked.images[0].pixels->bytes.empty(),
                            "converted scene owns its blocks after original image storage is released");
    Scene disabled  = MakeScene();
    disabled.images.push_back(disabled.images[0]);
    const Scene  snapshot   = disabled;
    uint64_t     remaining  = 0;
    const Status status     = detail::ProcessTextures(disabled, {}, remaining, diagnostics, nullptr);
    valid                  &= Check(status.Succeeded() && scene_test::Same(disabled, snapshot) && remaining == 0,
                                    "disabled texture policy is a no-op even with unused images and no spare budget");
    return valid;
}

bool TestOpacityExclusions()
{
    bool valid = true;
    for (uint32_t mode = 0; mode < 3; ++mode)
    {
        Scene scene = MakeScene();
        scene.textures.push_back(scene.textures[0]);
        scene.materials[0].m_pBaseColorTexture             = TextureId{1};
        scene.materials[0].features.diffuseTexture.texture = TextureId{1};
        scene.materials[0].extension.pDiffuseTexture       = TextureId{1};
        scene.materials[0].alphaMode                       = AlphaMode::Mask;
        if (mode == 0)
        {
            scene.materials[0].baseColorFactor.w         = -1;
            scene.materials[0].extension.diffuseFactor.w = -1;
        }
        else if (mode == 1)
        {
            scene.vertices[1].color.w = 0.25f;
        }
        else
        {
            scene.morphPrimitives.resize(1);
            scene.morphPrimitives[0].targets.resize(1);
            scene.morphPrimitives[0].targets[0].colors.resize(3, Vec4(0));
            scene.morphPrimitives[0].targets[0].colors[1].w = -0.5f;
        }
        scene.materials.push_back(scene.materials[0]);
        scene.meshes[0].primitives[0].material = MaterialId{0};
        scene.materialVariants                 = {"alternative"};
        scene.variantPrimitives.push_back({0, 0, {1}});
        const Scene             original = scene;
        std::vector<Diagnostic> diagnostics;
        valid &= Process(scene, diagnostics);
        valid &= Check(scene.images.size() == 1 && scene.images[0].pixels == original.images[0].pixels,
                       "negative or vertex/morph alpha preserves authored pixels for all masked bindings");
        valid &= Check(diagnostics.size() == 1
                           && diagnostics[0].message.find(mode == 0 ? "Negative opacity" : "Vertex or morph opacity")
                                  != std::string::npos,
                       "opacity exclusions report one reason per shared image/use without catalog duplication");
        valid &= Check(scene_test::Same(scene.variantPrimitives, original.variantPrimitives)
                           && scene_test::Same(scene.morphPrimitives, original.morphPrimitives),
                       "opacity exclusions preserve variant bindings and morph data");
    }
    return valid;
}

bool TestNormalScaleExclusions()
{
    bool valid = true;
    for (uint32_t mode = 0; mode < 8; ++mode)
    {
        Scene scene                                                = MakeScene();
        scene.materials[0].m_pNormalTexture                        = TextureId{0};
        scene.materials[0].features.clearcoatNormalTexture.texture = TextureId{0};
        const uint32_t  scenario                                   = mode % 4;
        TextureSettings settings                                   = Settings();
        if (mode >= 4)
        {
            scene.textures[0].format    = PixelFormat::RGBA8Unorm;
            settings.compressBC5Normals = true;
        }
        if (scenario < 2)
        {
            scene.materials[0].normalScale                           = scenario == 0 ? 100.0f : 1.0f;
            scene.materials[0].features.clearcoatNormalTexture.scale = scenario == 1 ? 100.0f : 1.0f;
        }
        else
        {
            scene.animations.resize(1);
            scene.animations[0].samplers = {{AnimationInterpolation::Linear, 1, {0, 1}, {1, 100}}};
            AnimationChannel channel;
            channel.path   = AnimationPath::Property;
            channel.target = {AnimatedObject::Material, 0,
                              scenario == 2 ? AnimatedProperty::NormalScale : AnimatedProperty::ClearcoatNormalScale, 1};
            scene.animations[0].channels.push_back(channel);
        }
        const Scene             original = scene;
        std::vector<Diagnostic> diagnostics;
        valid                    &= Process(scene, diagnostics, settings);
        const TextureId retained  = scenario == 0 || scenario == 2 ? scene.materials[0].m_pNormalTexture
                                                                   : scene.materials[0].features.clearcoatNormalTexture.texture;
        valid &= Check(scene.images[scene.textures[retained.value].image.value].pixels == original.images[0].pixels,
                       "non-unit or animated core/clearcoat normal scale retains authored image");
        valid &= Check(scene.textures[scene.materials[0].m_pBaseColorTexture.value].format
                           == (mode < 4 ? PixelFormat::BC7Srgb : PixelFormat::BC7Unorm),
                       "shared color usage still compresses when normal-scale usage is retained");
        valid &= Check(diagnostics.size() == 1 && diagnostics[0].message.find("normal scale") != std::string::npos
                           && scene_test::Same(scene.animations, original.animations),
                       "normal-scale exclusion is explicit and preserves typed animation targets");
    }
    return valid;
}

bool TestNormalAndOcclusionSplits()
{
    Scene scene                         = MakeScene();
    scene.textures[0].format            = PixelFormat::RGBA8Unorm;
    std::shared_ptr<ImagePixels> pixels = std::make_shared<ImagePixels>(*scene.images[0].pixels);
    for (size_t offset = 0; offset < pixels->bytes.size(); offset += 4)
    {
        pixels->bytes[offset + 3] = static_cast<uint8_t>(offset * 37 + 17);
    }
    scene.images[0].pixels               = pixels;
    MaterialAsset& material              = scene.materials[0];
    material.m_pMetallicRoughnessTexture = material.m_pNormalTexture = material.m_pOcclusionTexture = TextureId{0};
    material.features.clearcoatNormalTexture.texture                                                = TextureId{0};
    material.features.clearcoatNormalTexture.texCoord                                               = 1;
    material.features.clearcoatNormalTexture.transform.uvOffset                                     = Vec2(0.25f, 0.75f);
    material.features.diffuseTexture.texture = material.extension.pDiffuseTexture = TextureId{0};
    scene.materialVariants                                                        = {"alternative"};
    scene.variantPrimitives.push_back({0, 0, {0}});
    scene.animations.resize(1);
    scene.animations[0].samplers = {{AnimationInterpolation::Linear, 2, {0, 1}, {0, 0, 1, 1}}};
    AnimationChannel channel;
    channel.path   = AnimationPath::Property;
    channel.target = {AnimatedObject::Material, 0, AnimatedProperty::TextureOffset, 2, UINT32_MAX, 0};
    scene.animations[0].channels.push_back(channel);
    const Scene     original = scene;
    TextureSettings settings;
    settings.compressBC5Normals = settings.compressBC4Occlusion = true;
    std::vector<Diagnostic> diagnostics;
    bool                    valid  = Process(scene, diagnostics, settings);
    const MaterialAsset&    cooked = scene.materials[0];
    valid &= Check(scene.textures.size() == 4 && scene.images.size() == 3,
                   "BC5/BC4 split normal and occlusion while untouched color and packed data share original storage");
    const TextureAsset& normal    = scene.textures[cooked.m_pNormalTexture.value];
    const TextureAsset& occlusion = scene.textures[cooked.m_pOcclusionTexture.value];
    valid &=
        Check(normal.format == PixelFormat::BC5Unorm && normal.interpretation == TextureInterpretation::NormalXYPositiveZ
                  && occlusion.format == PixelFormat::BC4Unorm && occlusion.interpretation == TextureInterpretation::Default,
              "BC5 alone marks positive-Z reconstruction; core occlusion alone becomes BC4");
    valid &= Check(scene.images[normal.image.value].pixels->mips.size() == 5
                       && scene.images[occlusion.image.value].pixels->mips.size() == 5,
                   "selected block formats provide full sampler-required mip chains without explicit mip generation");
    valid &= Check(scene.images[scene.textures[cooked.m_pBaseColorTexture.value].image.value].pixels == pixels
                       && scene.images[scene.textures[cooked.m_pMetallicRoughnessTexture.value].image.value].pixels == pixels
                       && pixels->bytes[3] == 17,
                   "alpha and GBA survive in shared color/data even though normal and occlusion discard unused channels");
    valid &= Check(cooked.features.clearcoatNormalTexture.texture == cooked.m_pNormalTexture
                       && cooked.features.clearcoatNormalTexture.transform
                              == original.materials[0].features.clearcoatNormalTexture.transform
                       && cooked.features.clearcoatNormalTexture.texCoord == 1
                       && cooked.features.diffuseTexture.texture == cooked.m_pBaseColorTexture
                       && cooked.extension.pDiffuseTexture == cooked.m_pBaseColorTexture,
                   "normal feature and extension aliases preserve transforms while following role variants");
    std::string reason;
    valid        &= Check(ValidateSceneAsset(scene, UINT64_MAX, &reason), reason.c_str());
    valid        &= Check(scene_test::Same(scene.animations, original.animations)
                              && scene_test::Same(scene.variantPrimitives, original.variantPrimitives)
                              && scene_test::Same(scene.nodes, original.nodes),
                          "BC5/BC4 role splits preserve material variants, animated UV identities and nodes");
    Scene repeat  = original;
    diagnostics.clear();
    valid                &= Process(repeat, diagnostics, settings);
    valid                &= Check(scene_test::Same(scene, repeat), "BC5/BC4 role splitting is deterministic");
    Scene precedence      = original;
    settings.compressBC7  = true;
    diagnostics.clear();
    valid &= Process(precedence, diagnostics, settings);
    valid &=
        Check(precedence.textures[precedence.materials[0].m_pNormalTexture.value].format == PixelFormat::BC5Unorm
                  && precedence.textures[precedence.materials[0].m_pOcclusionTexture.value].format == PixelFormat::BC4Unorm,
              "explicit BC5/BC4 role choices take precedence over BC7");
    Scene srgb              = original;
    srgb.textures[0].format = PixelFormat::RGBA8Srgb;
    settings.compressBC7    = false;
    diagnostics.clear();
    valid &= Process(srgb, diagnostics, settings);
    valid &= Check(srgb.images.size() == 1 && srgb.images[0].pixels == pixels,
                   "sRGB normal and occlusion views retain authored channels with only linear codecs enabled");
    return valid;
}

struct CancelState
{
    uint32_t polls{0};
    uint32_t stop{0};
};

bool CancelAfter(void* user)
{
    CancelState& state = *static_cast<CancelState*>(user);
    return ++state.polls >= state.stop;
}

bool TestBudgetsAndCancellation()
{
    bool valid = true;
    for (uint64_t budget : {0ull, 32ull, 1024ull, 4096ull})
    {
        Scene                   scene     = MakeScene();
        const Scene             original  = scene;
        uint64_t                remaining = budget;
        std::vector<Diagnostic> diagnostics;
        const Status            status = detail::ProcessTextures(scene, Settings(), remaining, diagnostics, nullptr);
        valid &= Check(status.code == StatusCode::ResourceLimit && remaining <= budget,
                       "small allocation budgets reject before uncharged work or unsigned budget underflow");
        valid &=
            Check(scene_test::Same(scene.images, original.images), "failed processing retains source owners for safe cleanup");
    }
    for (uint32_t stop : {1u, 20u, 1000u})
    {
        Scene scene = MakeScene();
        scene.materials.resize(2048, scene.materials[0]);
        uint64_t                remaining = 64u * 1024u * 1024u;
        std::vector<Diagnostic> diagnostics;
        CancelState             state{0, stop};
        Cancellation            cancellation(CancelAfter, &state);
        const Status            status = detail::ProcessTextures(scene, Settings(), remaining, diagnostics, &cancellation);
        valid &= Check(status.code == StatusCode::Cancelled, "long material traversal observes cancellation");
        valid &=
            Check(scene.images[0].pixels->format == PixelFormat::RGBA8Unorm, "cancelled work does not replace source images");
    }
    return valid;
}
} // namespace

int main()
{
    bool valid  = TestRolesAndAliases();
    valid      &= TestNormalSamplerQuality();
    valid      &= TestNormalMipSamplers();
    valid      &= TestViewTransferSamplersAndUnboundResources();
    valid      &= TestMaskFactorsAndAnimationIdentity();
    valid      &= TestSourceLifetimeAndDisabledPolicy();
    valid      &= TestOpacityExclusions();
    valid      &= TestNormalScaleExclusions();
    valid      &= TestNormalAndOcclusionSplits();
    valid      &= TestBudgetsAndCancellation();
    std::printf("Texture processing: %u checks, %s\n", checks, valid ? "PASS" : "FAIL");
    return valid ? 0 : 1;
}
