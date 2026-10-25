#include "Processing/TextureProcessing.h"
#include "Processing/Diagnostics.h"
#include "Processing/TextureImage.h"
#include "Processing/MaterialTextureUses.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <string_view>
#include <tuple>

namespace zenasset::detail
{
namespace
{
bool UseLess(const TextureImageUse& a, const TextureImageUse& b)
{
    return std::tie(a.role, a.srgb, a.requiresMipmaps, a.alphaMask, a.exactAlpha, a.alphaCutoff, a.filterNormals,
                    a.filterNormalMin, a.filterNormalMips, a.normalRepeatS, a.normalRepeatT)
         < std::tie(b.role, b.srgb, b.requiresMipmaps, b.alphaMask, b.exactAlpha, b.alphaCutoff, b.filterNormals,
                    b.filterNormalMin, b.filterNormalMips, b.normalRepeatS, b.normalRepeatT);
}

enum class PreserveReason
{
    None,
    Catalog,
    NegativeOpacity,
    VertexOpacity,
    NormalScale,
    NormalBorder
};

struct Binding
{
    uint32_t        texture;
    TextureImageUse use;
    PreserveReason  preserve;
    TextureId*      target;
};

struct BindingOrder
{
    bool operator()(const Binding& a, const Binding& b) const
    {
        return a.texture < b.texture
            || (a.texture == b.texture && (a.preserve < b.preserve || (a.preserve == b.preserve && UseLess(a.use, b.use))));
    }
};

struct Variant
{
    uint32_t        sourceTexture;
    uint32_t        sourceImage;
    uint32_t        targetTexture;
    TextureImageUse use;
    PreserveReason  preserve;
    uint32_t        targetImage{UINT32_MAX};
};

struct ImageOrder
{
    const std::vector<Variant>& variants;
    bool                        operator()(uint32_t left, uint32_t right) const
    {
        const Variant& a = variants[left];
        const Variant& b = variants[right];
        return a.sourceImage < b.sourceImage
            || (a.sourceImage == b.sourceImage
                && (a.preserve < b.preserve || (a.preserve == b.preserve && UseLess(a.use, b.use))));
    }
};

PixelFormat ViewFormat(PixelFormat storage, bool srgb)
{
    PixelFormat format = storage;
    if (storage == PixelFormat::RGBA8Unorm || storage == PixelFormat::RGBA8Srgb)
    {
        format = srgb ? PixelFormat::RGBA8Srgb : PixelFormat::RGBA8Unorm;
    }
    else if (storage == PixelFormat::BC7Unorm || storage == PixelFormat::BC7Srgb)
    {
        format = srgb ? PixelFormat::BC7Srgb : PixelFormat::BC7Unorm;
    }
    return format;
}

class TextureProcessor
{
public:
    TextureProcessor(Scene&                   scene,
                     const TextureSettings&   settings,
                     uint64_t&                remaining,
                     std::vector<Diagnostic>& diagnostics,
                     const Cancellation*      cancellation) :
        m_scene(scene), m_settings(settings), m_remaining(remaining), m_diagnostics(diagnostics), m_cancellation(cancellation)
    {}

    ~TextureProcessor()
    {
        m_remaining += m_scratch;
    }

    Status Run()
    {
        if (Active()
            && (m_settings.compressBC7 || m_settings.compressBC5Normals || m_settings.compressBC4Occlusion
                || m_settings.generateMipmaps))
        {
            for (size_t index = 0; Active() && index < m_scene.textures.size(); ++index)
            {
                if (m_scene.textures[index].atlasRegion != ImageRegion{})
                {
                    m_status = {
                        StatusCode::UnsupportedFeature,
                        "Spatial atlas textures must be recooked from original sources before mip generation or encoding"};
                }
            }
            if (Active())
            {
                CollectBindings();
            }
            if (Active())
            {
                BuildVariants();
            }
            if (Active())
            {
                ProcessImages();
            }
            if (Active())
            {
                Publish();
            }
        }
        return m_status;
    }

private:
    bool Active()
    {
        if (m_status.Succeeded() && m_cancellation != nullptr && m_cancellation->IsCancelled())
        {
            m_status = {StatusCode::Cancelled, "Texture processing cancelled"};
        }
        return m_status.Succeeded();
    }

    bool Reserve(uint64_t count, uint64_t stride, bool scratch = true)
    {
        if (m_status.Succeeded() && (count > SIZE_MAX / stride || count > m_remaining / stride))
        {
            m_status = {StatusCode::ResourceLimit, "Texture processing exceeds its allocation budget"};
        }
        if (m_status.Succeeded())
        {
            m_remaining -= count * stride;
            if (scratch)
            {
                m_scratch += count * stride;
            }
        }
        return m_status.Succeeded();
    }

    void Warn(uint32_t image, std::string_view reason)
    {
        char object[48]{};
        std::snprintf(object, sizeof(object), "image %u", image);
        m_status = AddProcessingWarning(object, reason, m_remaining, m_diagnostics);
    }

    void AddBinding(TextureId&           id,
                    TextureImageRole     role,
                    const MaterialAsset& material,
                    bool                 opacity,
                    float                factor,
                    bool                 animated)
    {
        if (id.IsValid() && m_status.Succeeded())
        {
            if (id.value >= m_scene.textures.size())
            {
                m_status = {StatusCode::InvalidData, "Texture binding is out of range"};
            }
            else
            {
                const TextureAsset& texture = m_scene.textures[id.value];
                PixelFormatInfo     info;
                TextureImageUse     use;
                use.role = role;
                GetPixelFormatInfo(texture.format, info);
                use.srgb            = info.srgb;
                use.requiresMipmaps = !texture.sampler.IsValid() || m_scene.samplers[texture.sampler.value].useMipmaps;
                use.alphaMask       = opacity && material.alphaMode == AlphaMode::Mask && factor > 0;
                use.exactAlpha =
                    opacity && material.alphaMode != AlphaMode::Opaque && (animated || material.alphaMode == AlphaMode::Blend);
                if (use.alphaMask)
                {
                    const double cutoff = double(material.alphaCutoff) / factor;
                    use.alphaCutoff     = static_cast<float>(std::clamp(cutoff, -double(std::numeric_limits<float>::max()),
                                                                        double(std::numeric_limits<float>::max())));
                }
                PreserveReason preserve = PreserveReason::None;
                if (opacity && material.alphaMode == AlphaMode::Mask)
                {
                    // A spatially varying vertex alpha has no single effective texture cutoff.
                    if (m_vertexOpacity)
                    {
                        preserve = PreserveReason::VertexOpacity;
                    }
                    else if (factor < 0)
                    {
                        preserve = PreserveReason::NegativeOpacity;
                    }
                }
                else if (role == TextureImageRole::Normal && (factor != 1.0f || animated))
                {
                    // The current angular gate measures unit-scale normals; shader XY scaling can amplify error.
                    preserve = PreserveReason::NormalScale;
                }
                const bool normalEncoding = (m_settings.compressBC5Normals && !use.srgb) || m_settings.compressBC7;
                if (role == TextureImageRole::Normal && normalEncoding && texture.sampler.IsValid())
                {
                    const SamplerAsset& sampler = m_scene.samplers[texture.sampler.value];
                    use.filterNormals =
                        sampler.minFilter != TextureFilter::Nearest || sampler.magFilter != TextureFilter::Nearest;
                    use.filterNormalMin  = sampler.minFilter != TextureFilter::Nearest;
                    use.filterNormalMips = sampler.useMipmaps && sampler.mipFilter != TextureFilter::Nearest;
                    use.normalRepeatS =
                        sampler.wrapS == SamplerAddressMode::Repeat || sampler.wrapS == SamplerAddressMode::Unspecified;
                    use.normalRepeatT =
                        sampler.wrapT == SamplerAddressMode::Repeat || sampler.wrapT == SamplerAddressMode::Unspecified;
                    if (preserve == PreserveReason::None
                        && (sampler.wrapS == SamplerAddressMode::ClampToBorder
                            || sampler.wrapT == SamplerAddressMode::ClampToBorder))
                    {
                        // Border color is selected by the runtime, outside this package's quality contract.
                        preserve = PreserveReason::NormalBorder;
                    }
                }
                m_bindings.push_back({id.value, use, preserve, &id});
                m_seen[id.value] = 1;
            }
        }
    }

    void FindVertexOpacity()
    {
        for (size_t index = 0; Active() && !m_vertexOpacity && index < m_scene.vertices.size(); ++index)
        {
            m_vertexOpacity = m_scene.vertices[index].color.w != 1.0f;
        }
        for (size_t index = 0; Active() && !m_vertexOpacity && index < m_scene.morphPrimitives.size(); ++index)
        {
            const MorphPrimitiveAsset& primitive = m_scene.morphPrimitives[index];
            for (size_t targetIndex = 0; Active() && !m_vertexOpacity && targetIndex < primitive.targets.size(); ++targetIndex)
            {
                const MorphTargetAsset& target = primitive.targets[targetIndex];
                for (size_t color = 0; Active() && !m_vertexOpacity && color < target.colors.size(); ++color)
                {
                    m_vertexOpacity = target.colors[color].w != 0.0f;
                }
                for (size_t set = 0; Active() && !m_vertexOpacity && set < target.extraColors.size(); ++set)
                {
                    const MorphColorAttributeAsset& attribute = target.extraColors[set];
                    for (size_t color = 0; Active() && !m_vertexOpacity && color < attribute.values.size(); ++color)
                    {
                        m_vertexOpacity = attribute.values[color].w != 0.0f;
                    }
                }
            }
        }
    }

    void CollectBindings()
    {
        const uint64_t maxBindings = uint64_t(m_scene.materials.size()) * MaterialTextureUseCount + m_scene.textures.size();
        if (maxBindings > UINT32_MAX || !Reserve(maxBindings, sizeof(Binding)) || !Reserve(m_scene.textures.size(), 1)
            || !Reserve(m_scene.materials.size(), 1))
        {
            if (m_status.Succeeded())
            {
                m_status = {StatusCode::ResourceLimit, "Too many texture bindings"};
            }
        }
        else
        {
            m_bindings.reserve(static_cast<size_t>(maxBindings));
            m_seen.resize(m_scene.textures.size(), 0);
            m_animated.resize(m_scene.materials.size(), 0);
            // Conservatively applies to every masked material, including variant and instance bindings.
            FindVertexOpacity();
            for (size_t animationIndex = 0; Active() && animationIndex < m_scene.animations.size(); ++animationIndex)
            {
                const AnimationAsset& animation = m_scene.animations[animationIndex];
                for (size_t channelIndex = 0; Active() && channelIndex < animation.channels.size(); ++channelIndex)
                {
                    const AnimationChannel& channel = animation.channels[channelIndex];
                    if (channel.target.object == AnimatedObject::Material && channel.target.index < m_animated.size())
                    {
                        if (channel.target.property == AnimatedProperty::BaseColor)
                        {
                            m_animated[channel.target.index] |= 1;
                        }
                        else if (channel.target.property == AnimatedProperty::Diffuse)
                        {
                            m_animated[channel.target.index] |= 2;
                        }
                        else if (channel.target.property == AnimatedProperty::AlphaCutoff)
                        {
                            m_animated[channel.target.index] |= 3;
                        }
                        else if (channel.target.property == AnimatedProperty::NormalScale)
                        {
                            m_animated[channel.target.index] |= 4;
                        }
                        else if (channel.target.property == AnimatedProperty::ClearcoatNormalScale)
                        {
                            m_animated[channel.target.index] |= 8;
                        }
                    }
                }
            }
            for (size_t index = 0; Active() && index < m_scene.materials.size(); ++index)
            {
                MaterialAsset& material = m_scene.materials[index];
                for (const MaterialTextureUse& use : MaterialTextureUses(material))
                {
                    const TextureImageRole role = use.role == TextureImageRole::Occlusion && !m_settings.compressBC4Occlusion
                                                    ? TextureImageRole::Data
                                                    : use.role;
                    AddBinding(*use.texture, role, material, use.opacity, use.factor,
                               (m_animated[index] & use.animationMask) != 0);
                }
            }
            for (uint32_t index = 0; Active() && index < m_seen.size(); ++index)
            {
                if (!m_seen[index])
                {
                    PixelFormatInfo info;
                    GetPixelFormatInfo(m_scene.textures[index].format, info);
                    TextureImageUse use;
                    use.srgb = info.srgb;
                    // Unbound authored textures remain available to asset browsers and later material edits.
                    m_bindings.push_back({index, use, PreserveReason::Catalog, nullptr});
                }
            }
        }
    }

    void BuildVariants()
    {
        std::sort(m_bindings.begin(), m_bindings.end(), BindingOrder{});
        if (Reserve(m_bindings.size(), sizeof(Variant)))
        {
            m_variants.reserve(m_bindings.size());
            uint32_t nextTexture     = static_cast<uint32_t>(m_scene.textures.size());
            uint32_t previousTexture = UINT32_MAX;
            for (size_t bindingIndex = 0; Active() && bindingIndex < m_bindings.size(); ++bindingIndex)
            {
                Binding&   binding = m_bindings[bindingIndex];
                const bool same    = !m_variants.empty() && m_variants.back().sourceTexture == binding.texture
                                  && m_variants.back().use == binding.use && m_variants.back().preserve == binding.preserve;
                if (!same)
                {
                    const uint32_t target = binding.texture != previousTexture ? binding.texture : nextTexture++;
                    m_variants.push_back({binding.texture, m_scene.textures[binding.texture].image.value, target, binding.use,
                                          binding.preserve});
                    previousTexture = binding.texture;
                }
                if (binding.target != nullptr)
                {
                    binding.target->value = m_variants.back().targetTexture;
                }
            }
            m_textureCount = nextTexture;
        }
    }

    void ProcessImages()
    {
        if (Reserve(m_variants.size(), sizeof(uint32_t)) && Reserve(m_scene.images.size(), sizeof(uint32_t))
            && Reserve(m_variants.size(), sizeof(ImageAsset)))
        {
            m_order.resize(m_variants.size());
            std::iota(m_order.begin(), m_order.end(), 0u);
            std::sort(m_order.begin(), m_order.end(), ImageOrder{m_variants});
            m_originalImage.resize(m_scene.images.size(), UINT32_MAX);
            m_images.reserve(m_variants.size());
            uint64_t imageRecordBytes = m_variants.size() * sizeof(ImageAsset);
            for (size_t first = 0; Active() && first < m_order.size();)
            {
                const Variant& variant = m_variants[m_order[first]];
                size_t         end     = first + 1;
                while (Active() && end < m_order.size() && m_variants[m_order[end]].sourceImage == variant.sourceImage
                       && m_variants[m_order[end]].use == variant.use && m_variants[m_order[end]].preserve == variant.preserve)
                {
                    ++end;
                }
                const ImageAsset&  source = m_scene.images[variant.sourceImage];
                TextureImageResult result;
                if (Active() && variant.preserve != PreserveReason::None)
                {
                    result.pixels = source.pixels;
                    if (variant.preserve == PreserveReason::NegativeOpacity)
                    {
                        Warn(variant.sourceImage, "Negative opacity factor retains authored pixels and mip behavior");
                    }
                    else if (variant.preserve == PreserveReason::VertexOpacity)
                    {
                        Warn(variant.sourceImage, "Vertex or morph opacity retains masked authored pixels and mip behavior");
                    }
                    else if (variant.preserve == PreserveReason::NormalScale)
                    {
                        Warn(variant.sourceImage, "Non-unit or animated normal scale retains authored pixels and mip behavior");
                    }
                    else if (variant.preserve == PreserveReason::NormalBorder)
                    {
                        Warn(variant.sourceImage,
                             "Runtime border color leaves normal compression unqualified; authored pixels retained");
                    }
                }
                else if (Active())
                {
                    result   = ProcessTextureImage(source.pixels, variant.use, m_settings, m_remaining, m_cancellation);
                    m_status = result.status;
                }
                if (Active() && !result.reason.empty())
                {
                    Warn(variant.sourceImage, result.reason);
                }
                if (Active())
                {
                    const bool original = result.pixels == source.pixels;
                    uint32_t   image    = original ? m_originalImage[variant.sourceImage] : UINT32_MAX;
                    if (image == UINT32_MAX && Reserve(source.name.size() + 1, 1))
                    {
                        image = static_cast<uint32_t>(m_images.size());
                        m_images.push_back({source.name, std::move(result.pixels)});
                        imageRecordBytes += source.name.size() + 1;
                        if (original)
                        {
                            m_originalImage[variant.sourceImage] = image;
                        }
                    }
                    for (size_t index = first; Active() && index < end; ++index)
                    {
                        m_variants[m_order[index]].targetImage = image;
                    }
                }
                const uint64_t temporary  = result.ownedBytes - result.pixelBytes;
                result                    = {};
                m_remaining              += temporary;
                first                     = end;
            }
            if (Active())
            {
                uint64_t originalBytes = m_scene.images.size() * sizeof(ImageAsset);
                for (size_t index = 0; Active() && index < m_scene.images.size(); ++index)
                {
                    originalBytes += m_scene.images[index].name.size() + 1;
                }
                // Existing copied records already count in CookScene's budget. Keep only
                // the growth charged after replacement; table capacity is counted too.
                if (Active())
                {
                    m_scratch -= imageRecordBytes > originalBytes ? imageRecordBytes - originalBytes : 0;
                }
            }
        }
    }

    void Publish()
    {
        const size_t originalCount = m_scene.textures.size();
        if (m_textureCount > originalCount && Reserve(m_textureCount, sizeof(TextureAsset)))
        {
            m_scene.textures.reserve(m_textureCount);
            for (size_t index = 0; Active() && index < m_variants.size(); ++index)
            {
                const Variant& variant = m_variants[index];
                if (variant.targetTexture >= originalCount)
                {
                    const TextureAsset& source = m_scene.textures[variant.sourceTexture];
                    if (Reserve(source.name.size() + 1, 1, false))
                    {
                        m_scene.textures.push_back(source);
                    }
                }
            }
            m_scratch -= (m_textureCount - originalCount) * sizeof(TextureAsset);
        }
        for (size_t index = 0; Active() && index < m_variants.size(); ++index)
        {
            const Variant& variant = m_variants[index];
            TextureAsset&  texture = m_scene.textures[variant.targetTexture];
            texture.image          = ImageId{variant.targetImage};
            texture.format         = ViewFormat(m_images[variant.targetImage].pixels->format, variant.use.srgb);
            if (texture.format == PixelFormat::BC5Unorm
                && m_images[variant.targetImage].pixels != m_scene.images[variant.sourceImage].pixels)
            {
                texture.interpretation = TextureInterpretation::NormalXYPositiveZ;
            }
        }
        if (Active())
        {
            m_scene.images.swap(m_images);
        }
    }

    Scene&                   m_scene;
    const TextureSettings&   m_settings;
    uint64_t&                m_remaining;
    std::vector<Diagnostic>& m_diagnostics;
    const Cancellation*      m_cancellation;
    Status                   m_status;
    uint64_t                 m_scratch{0};
    uint32_t                 m_textureCount{0};
    bool                     m_vertexOpacity{false};
    std::vector<Binding>     m_bindings;
    std::vector<Variant>     m_variants;
    std::vector<uint8_t>     m_seen;
    std::vector<uint8_t>     m_animated;
    std::vector<uint32_t>    m_order;
    std::vector<uint32_t>    m_originalImage;
    std::vector<ImageAsset>  m_images;
};
} // namespace

Status ProcessTextures(Scene&                   scene,
                       const TextureSettings&   settings,
                       uint64_t&                remainingBytes,
                       std::vector<Diagnostic>& diagnostics,
                       const Cancellation*      cancellation)
{
    TextureProcessor processor(scene, settings, remainingBytes, diagnostics, cancellation);
    return processor.Run();
}
} // namespace zenasset::detail
