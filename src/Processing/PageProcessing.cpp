#include "Processing/PageProcessing.h"
#include "Processing/ArrayPacking.h"
#include "Processing/MaterialTextureUses.h"
#include "Processing/Diagnostics.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <tuple>

namespace zenasset::detail
{
namespace
{
enum class Exclusion
{
    None,
    ExistingPage,
    SpatialPage,
    MissingMips,
    SpatialMips,
    SpatialFormat,
    SpatialDimensions,
    Dimensions,
    Sampler,
    MixedSamplers,
    Unbound,
    AnimatedUV,
    MorphUV,
    UnknownUV,
    OutsideDomain,
    Singleton
};

const char* Reason(Exclusion reason)
{
    const char* text = "";
    switch (reason)
    {
        case Exclusion::ExistingPage: text = "Existing array page retained without repacking"; break;
        case Exclusion::SpatialPage: text = "Existing spatial page retained without repacking"; break;
        case Exclusion::MissingMips:
            text = "Array page skipped: finalized explicit mips are required; configure mip processing separately";
            break;
        case Exclusion::SpatialMips: text = "Spatial page skipped: authored mip chains require logical mip support"; break;
        case Exclusion::SpatialFormat: text = "Spatial page skipped: uncompressed RGBA8 storage and views are required"; break;
        case Exclusion::SpatialDimensions:
            text = "Spatial page skipped: power-of-two tile dimensions including gutters must fit the page";
            break;
        case Exclusion::Dimensions: text = "Array page skipped: image exceeds the configured page dimension"; break;
        case Exclusion::Sampler:
            text = "Array page skipped: every view requires explicit clamp-to-edge and linear filtering";
            break;
        case Exclusion::MixedSamplers: text = "Array page skipped: views of this image have different sampler policies"; break;
        case Exclusion::Unbound: text = "Array page skipped: catalog or unbound material usage has no proven UV domain"; break;
        case Exclusion::AnimatedUV: text = "Array page skipped: a material use has animated UV state"; break;
        case Exclusion::MorphUV: text = "Array page skipped: a material use has UV morph targets"; break;
        case Exclusion::UnknownUV:
            text = "Array page skipped: a material use has missing UV coordinates or no indexed surface";
            break;
        case Exclusion::OutsideDomain:
            text = "Array page skipped: effective authored UVs leave the supported [0,1] domain";
            break;
        case Exclusion::Singleton: text = "Array page skipped: no compatible partner within the page layer limit"; break;
        default: break;
    }
    return text;
}

struct PageMapping
{
    uint32_t    id{0};
    uint32_t    page{UINT32_MAX};
    uint32_t    layer{0};
    ImageRegion region;
};

using GroupKey = std::array<uint32_t, 9>;
struct ImageState
{
    Exclusion reason{Exclusion::None};
    GroupKey  group{};
    bool      samplerSet{false};
};

struct PrimitiveUse
{
    uint32_t material;
    uint32_t mesh;
    uint32_t primitive;
    bool     operator==(const PrimitiveUse&) const = default;
};

bool UseOrder(const PrimitiveUse& a, const PrimitiveUse& b)
{
    return std::tie(a.material, a.mesh, a.primitive) < std::tie(b.material, b.mesh, b.primitive);
}

struct ImageOrder
{
    const std::vector<ImageState>& states;
    bool                           operator()(const AtlasTileInput& a, const AtlasTileInput& b) const
    {
        return states[a.id].group < states[b.id].group || (states[a.id].group == states[b.id].group && a.id < b.id);
    }
};

GroupKey SamplerKey(const SamplerParameters& sampler)
{
    return {uint32_t(sampler.minFilter),
            uint32_t(sampler.magFilter),
            uint32_t(sampler.mipFilter),
            uint32_t(sampler.wrapS),
            uint32_t(sampler.wrapT),
            sampler.useMipmaps ? 1u : 0u,
            0,
            0,
            0};
}

class TexturePageProcessor
{
public:
    TexturePageProcessor(Scene&                   scene,
                         const TextureSettings&   settings,
                         uint64_t&                remaining,
                         std::vector<Diagnostic>& diagnostics,
                         const Cancellation*      cancellation) :
        m_scene(scene), m_settings(settings), m_remaining(remaining), m_diagnostics(diagnostics), m_cancellation(cancellation)
    {}

    ~TexturePageProcessor()
    {
        m_remaining += m_scratch;
    }

    Status Run()
    {
        if ((m_settings.arrayPages || m_settings.spatialPages) && Active())
        {
            Prepare();
            if (Active())
            {
                CollectUses(true);
            }
            if (Active() && Reserve(m_useCount, sizeof(PrimitiveUse)))
            {
                m_uses.reserve(static_cast<size_t>(m_useCount));
                CollectUses(false);
                std::sort(m_uses.begin(), m_uses.end(), UseOrder);
                m_uses.erase(std::unique(m_uses.begin(), m_uses.end()), m_uses.end());
                for (const PrimitiveUse& use : m_uses)
                {
                    m_materialUsed[use.material] = 1;
                }
            }
            if (Active())
            {
                InspectBindings();
                InspectSurfaces();
                BuildInputs();
            }
            if (Active())
            {
                PackAndPublish();
            }
        }
        return m_status;
    }

private:
    bool Active()
    {
        if (m_status.Succeeded() && m_cancellation && m_cancellation->IsCancelled())
        {
            m_status = {StatusCode::Cancelled, "Texture page processing cancelled"};
        }
        return m_status.Succeeded();
    }

    bool Reserve(uint64_t count, uint64_t stride)
    {
        if (Active() && (count > SIZE_MAX / stride || count > m_remaining / stride))
        {
            m_status = {StatusCode::ResourceLimit, "Texture page analysis exceeds its allocation budget"};
        }
        if (m_status.Succeeded())
        {
            m_remaining -= count * stride;
            m_scratch   += count * stride;
        }
        return m_status.Succeeded();
    }

    void Exclude(uint32_t image, Exclusion reason)
    {
        if (m_states[image].reason == Exclusion::None)
        {
            m_states[image].reason = reason;
        }
    }

    void Prepare()
    {
        if (Reserve(m_scene.images.size(), sizeof(ImageState)) && Reserve(m_scene.textures.size(), 1)
            && Reserve(m_scene.materials.size(), sizeof(uint32_t) + 1) && Reserve(m_scene.meshes.size(), sizeof(uint32_t)))
        {
            m_states.resize(m_scene.images.size());
            m_textureSeen.resize(m_scene.textures.size(), 0);
            m_animated.resize(m_scene.materials.size(), 0);
            m_materialUsed.resize(m_scene.materials.size(), 0);
            m_meshOffset.resize(m_scene.meshes.size());
        }
        uint64_t primitives = 0;
        for (size_t mesh = 0; Active() && mesh < m_scene.meshes.size(); ++mesh)
        {
            m_meshOffset[mesh]  = static_cast<uint32_t>(primitives);
            primitives         += m_scene.meshes[mesh].primitives.size();
            if (primitives > UINT32_MAX)
            {
                m_status = {StatusCode::ResourceLimit, "Too many primitives for texture page eligibility"};
            }
        }
        if (Active() && Reserve(primitives, 1))
        {
            m_uvMorph.resize(static_cast<size_t>(primitives), 0);
        }
        for (size_t image = 0; Active() && image < m_scene.images.size(); ++image)
        {
            const ImagePixels& pixels = *m_scene.images[image].pixels;
            if (pixels.arrayLayers != 1)
            {
                Exclude(static_cast<uint32_t>(image), Exclusion::ExistingPage);
            }
            else if (m_settings.spatialPages)
            {
                if (pixels.format != PixelFormat::RGBA8Unorm && pixels.format != PixelFormat::RGBA8Srgb)
                {
                    Exclude(static_cast<uint32_t>(image), Exclusion::SpatialFormat);
                }
                else if (pixels.mips.size() > 1)
                {
                    Exclude(static_cast<uint32_t>(image), Exclusion::SpatialMips);
                }
                else if (!std::has_single_bit(pixels.width) || !std::has_single_bit(pixels.height)
                         || pixels.width > m_settings.spatialMaxDimension - 8
                         || pixels.height > m_settings.spatialMaxDimension - 8)
                {
                    Exclude(static_cast<uint32_t>(image), Exclusion::SpatialDimensions);
                }
            }
            else if (pixels.mips.empty())
            {
                Exclude(static_cast<uint32_t>(image), Exclusion::MissingMips);
            }
            else if (pixels.width > m_settings.arrayMaxDimension || pixels.height > m_settings.arrayMaxDimension)
            {
                Exclude(static_cast<uint32_t>(image), Exclusion::Dimensions);
            }
        }
        for (size_t index = 0; Active() && index < m_scene.textures.size(); ++index)
        {
            const TextureAsset& texture = m_scene.textures[index];
            if (texture.atlasRegion != ImageRegion{})
            {
                Exclude(texture.image.value, Exclusion::SpatialPage);
            }
            if (m_settings.spatialPages && texture.format != PixelFormat::RGBA8Unorm
                && texture.format != PixelFormat::RGBA8Srgb)
            {
                Exclude(texture.image.value, Exclusion::SpatialFormat);
            }
            ImageState& state  = m_states[texture.image.value];
            state.group[6]    |= 1u << uint32_t(texture.format);
            state.group[8]    |= 1u << uint32_t(texture.interpretation);
            if (!texture.sampler.IsValid())
            {
                Exclude(texture.image.value, Exclusion::Sampler);
            }
            else
            {
                const SamplerAsset& sampler = m_scene.samplers[texture.sampler.value];
                if (sampler.minFilter != TextureFilter::Linear || sampler.magFilter != TextureFilter::Linear
                    || sampler.mipFilter != TextureFilter::Linear || sampler.wrapS != SamplerAddressMode::ClampToEdge
                    || sampler.wrapT != SamplerAddressMode::ClampToEdge)
                {
                    Exclude(texture.image.value, Exclusion::Sampler);
                }
                if (m_settings.spatialPages && sampler.useMipmaps && m_scene.images[texture.image.value].pixels->mips.empty())
                {
                    Exclude(texture.image.value, Exclusion::MissingMips);
                }
                const GroupKey key = SamplerKey(sampler);
                if (state.samplerSet && !std::equal(key.begin(), key.begin() + 6, state.group.begin()))
                {
                    Exclude(texture.image.value, Exclusion::MixedSamplers);
                }
                std::copy(key.begin(), key.begin() + 6, state.group.begin());
                state.samplerSet = true;
            }
        }
        for (size_t index = 0; Active() && index < m_scene.animations.size(); ++index)
        {
            for (const AnimationChannel& channel : m_scene.animations[index].channels)
            {
                const AnimationTarget& target = channel.target;
                if (Active() && target.object == AnimatedObject::Material
                    && (target.property == AnimatedProperty::TextureOffset || target.property == AnimatedProperty::TextureScale
                        || target.property == AnimatedProperty::TextureRotation))
                {
                    m_animated[target.index] |= 1u << target.textureSlot;
                }
            }
        }
        for (size_t index = 0; Active() && index < m_scene.morphPrimitives.size(); ++index)
        {
            const MorphPrimitiveAsset& morph = m_scene.morphPrimitives[index];
            bool                       uv    = false;
            for (size_t targetIndex = 0; Active() && targetIndex < morph.targets.size(); ++targetIndex)
            {
                const MorphTargetAsset& target = morph.targets[targetIndex];
                for (size_t set = 0; Active() && set < target.texCoords.size(); ++set)
                {
                    uv |= !target.texCoords[set].empty();
                }
            }
            if (uv)
            {
                const uint32_t first = morph.primitive == UINT32_MAX ? 0 : morph.primitive;
                const uint32_t end   = morph.primitive == UINT32_MAX
                                         ? static_cast<uint32_t>(m_scene.meshes[morph.mesh].primitives.size())
                                         : first + 1;
                for (uint32_t primitive = first; Active() && primitive < end; ++primitive)
                {
                    m_uvMorph[m_meshOffset[morph.mesh] + primitive] = 1;
                }
            }
        }
    }

    void AddUse(MaterialId material, uint32_t mesh, uint32_t primitive, bool counting)
    {
        if (Active() && material.IsValid())
        {
            if (counting)
            {
                ++m_useCount;
                if (m_useCount > UINT32_MAX || m_useCount > m_remaining / sizeof(PrimitiveUse))
                {
                    m_status = {StatusCode::ResourceLimit, "Too many material uses for bounded texture page analysis"};
                }
            }
            else
            {
                m_uses.push_back({material.value, mesh, primitive});
            }
        }
    }

    void CollectUses(bool counting)
    {
        for (uint32_t mesh = 0; Active() && mesh < m_scene.meshes.size(); ++mesh)
        {
            for (uint32_t primitive = 0; Active() && primitive < m_scene.meshes[mesh].primitives.size(); ++primitive)
            {
                AddUse(m_scene.meshes[mesh].primitives[primitive].material, mesh, primitive, counting);
            }
        }
        for (size_t index = 0; Active() && index < m_scene.nodes.size(); ++index)
        {
            const NodeAsset& node = m_scene.nodes[index];
            if (node.mesh.IsValid() && !node.materials.empty())
            {
                const MeshAsset& mesh = m_scene.meshes[node.mesh.value];
                for (uint32_t primitive = 0; Active() && primitive < mesh.primitives.size(); ++primitive)
                {
                    const uint32_t slot = mesh.primitives[primitive].materialSlot;
                    if (slot != UINT32_MAX)
                    {
                        AddUse(node.materials[slot], node.mesh.value, primitive, counting);
                    }
                }
            }
        }
        for (size_t index = 0; Active() && index < m_scene.variantPrimitives.size(); ++index)
        {
            const MaterialVariantPrimitiveAsset& variant = m_scene.variantPrimitives[index];
            for (int32_t material : variant.materials)
            {
                if (material >= 0)
                {
                    AddUse(MaterialId{static_cast<uint32_t>(material)}, variant.mesh, variant.primitive, counting);
                }
            }
        }
    }

    void InspectBindings()
    {
        for (uint32_t material = 0; Active() && material < m_scene.materials.size(); ++material)
        {
            for (const MaterialTextureUse& use : MaterialTextureUses(m_scene.materials[material]))
            {
                if (use.texture->IsValid())
                {
                    const uint32_t texture    = use.texture->value;
                    const uint32_t image      = m_scene.textures[texture].image.value;
                    m_textureSeen[texture]    = 1;
                    m_states[image].group[7] |= 1u << uint32_t(use.role);
                    if (!m_materialUsed[material])
                    {
                        Exclude(image, Exclusion::Unbound);
                    }
                    else if ((m_animated[material] & (1u << use.slot)) != 0)
                    {
                        Exclude(image, Exclusion::AnimatedUV);
                    }
                }
            }
        }
        for (size_t texture = 0; Active() && texture < m_textureSeen.size(); ++texture)
        {
            if (!m_textureSeen[texture])
            {
                Exclude(m_scene.textures[texture].image.value, Exclusion::Unbound);
            }
        }
        for (size_t image = 0; Active() && image < m_states.size(); ++image)
        {
            if (m_states[image].group[7] == 0)
            {
                Exclude(static_cast<uint32_t>(image), Exclusion::Unbound);
            }
        }
    }

    bool Coordinate(uint32_t vertex, uint32_t set, Vec2& uv) const
    {
        bool valid = set < m_scene.vertexTexCoordSets.size();
        if (valid && !m_scene.vertexTexCoords.empty() && set < m_scene.vertexTexCoords[vertex].size())
        {
            uv = m_scene.vertexTexCoords[vertex][set];
        }
        else if (valid && set < 2)
        {
            uv = set == 0 ? m_scene.vertices[vertex].uv0 : m_scene.vertices[vertex].uv1;
        }
        else
        {
            valid = false;
        }
        return valid;
    }

    void InspectSurfaces()
    {
        for (size_t index = 0; Active() && index < m_uses.size(); ++index)
        {
            const PrimitiveUse&       surface   = m_uses[index];
            const MeshPrimitiveAsset& primitive = m_scene.meshes[surface.mesh].primitives[surface.primitive];
            for (const MaterialTextureUse& use : MaterialTextureUses(m_scene.materials[surface.material]))
            {
                if (Active() && use.texture->IsValid())
                {
                    const uint32_t image = m_scene.textures[use.texture->value].image.value;
                    if (m_uvMorph[m_meshOffset[surface.mesh] + surface.primitive])
                    {
                        Exclude(image, Exclusion::MorphUV);
                    }
                    if (primitive.indexCount == 0)
                    {
                        Exclude(image, Exclusion::UnknownUV);
                    }
                    for (uint32_t corner = 0;
                         Active() && m_states[image].reason == Exclusion::None && corner < primitive.indexCount; ++corner)
                    {
                        Vec2 uv;
                        if (!Coordinate(m_scene.indices[primitive.firstIndex + corner], use.texCoord, uv))
                        {
                            Exclude(image, Exclusion::UnknownUV);
                        }
                        else
                        {
                            const Vec4&  a = use.transform->row0;
                            const Vec4&  b = use.transform->row1;
                            const double u = double(a.x) * uv.x + double(a.y) * uv.y + a.z;
                            const double v = double(b.x) * uv.x + double(b.y) * uv.y + b.z;
                            if (!std::isfinite(u) || !std::isfinite(v) || u < 0 || u > 1 || v < 0 || v > 1)
                            {
                                Exclude(image, Exclusion::OutsideDomain);
                            }
                        }
                    }
                }
            }
        }
    }

    void BuildInputs()
    {
        uint32_t count = 0;
        for (const ImageState& state : m_states)
        {
            count += state.reason == Exclusion::None;
        }
        const uint32_t limit = m_settings.spatialPages ? m_settings.spatialMaxImages : m_settings.arrayMaxImages;
        if (Active() && count > limit)
        {
            m_status = {StatusCode::ResourceLimit, "Eligible texture page images exceed the configured input limit"};
        }
        if (Active() && Reserve(count, sizeof(AtlasTileInput)))
        {
            m_inputs.reserve(count);
            for (uint32_t image = 0; Active() && image < m_states.size(); ++image)
            {
                if (m_states[image].reason == Exclusion::None)
                {
                    m_inputs.push_back({image, 0, m_scene.images[image].pixels});
                }
            }
            std::sort(m_inputs.begin(), m_inputs.end(), ImageOrder{m_states});
            uint32_t group = 0;
            for (size_t index = 0; Active() && index < m_inputs.size(); ++index)
            {
                group += index != 0 && m_states[m_inputs[index - 1].id].group != m_states[m_inputs[index].id].group;
                m_inputs[index].group = group;
            }
        }
    }

    void Warn(uint32_t image, Exclusion reason)
    {
        char object[48]{};
        std::snprintf(object, sizeof(object), "image %u", image);
        const char* text = Reason(reason);
        char        message[256]{};
        if (m_settings.spatialPages && reason == Exclusion::MissingMips)
        {
            text = "Spatial page skipped: implicit mip generation cannot be replaced by a single-level page";
        }
        else if (m_settings.spatialPages && reason == Exclusion::Singleton)
        {
            text = "Spatial page skipped: no compatible partner fits on the same page";
        }
        else if (m_settings.spatialPages && std::string_view(text).starts_with("Array page skipped: "))
        {
            std::snprintf(message, sizeof(message), "Spatial page skipped: %s", text + sizeof("Array page skipped: ") - 1);
            text = message;
        }
        m_status = AddProcessingWarning(object, text, m_remaining, m_diagnostics);
    }

    void PackAndPublish()
    {
        if (Reserve(m_scene.images.size(), sizeof(PageMapping)))
        {
            m_mapping.resize(m_scene.images.size());
            if (m_settings.spatialPages)
            {
                AtlasPackingOptions options;
                options.maxPageDimension = m_settings.spatialMaxDimension;
                options.maxPages         = m_settings.spatialMaxPages;
                options.maxTiles = options.maxMipRegions = m_settings.spatialMaxImages;
                options.powerOfTwoPages                  = true;
                AtlasPackResult packed                   = PackAtlasTiles(m_inputs, options, m_remaining, m_cancellation);
                m_status                                 = packed.status;
                for (const AtlasTileRegions& tile : packed.tiles)
                {
                    const AtlasMipRegion& region = packed.mips[tile.firstMip];
                    m_mapping[tile.id]           = {tile.id, region.page, 0, {region.x, region.y, region.width, region.height}};
                }
                m_remaining += packed.ownedBytes - FinishPacking(packed.pages);
            }
            else
            {
                ArrayPackingOptions options{m_settings.arrayMaxDimension, m_settings.arrayMaxLayers, m_settings.arrayMaxPages,
                                            m_settings.arrayMaxImages};
                ArrayPackResult     packed = PackArrayPages(m_inputs, options, m_remaining, m_cancellation);
                m_status                   = packed.status;
                for (const ArrayTileLayer& tile : packed.tiles)
                {
                    m_mapping[tile.id] = {tile.id, tile.page, tile.layer, {}};
                    if (tile.page == UINT32_MAX)
                    {
                        Exclude(tile.id, Exclusion::Singleton);
                    }
                }
                m_remaining += packed.ownedBytes - FinishPacking(packed.pages);
            }
        }
    }

    uint64_t FinishPacking(std::span<const AtlasPage> pages)
    {
        uint64_t retainedPixels = 0;
        if (Reserve(pages.size(), sizeof(uint32_t)))
        {
            m_pageImages.resize(pages.size(), 0);
            for (const PageMapping& mapping : m_mapping)
            {
                if (mapping.page != UINT32_MAX)
                {
                    ++m_pageImages[mapping.page];
                }
            }
            for (uint32_t image = 0; Active() && image < m_mapping.size(); ++image)
            {
                PageMapping& mapping = m_mapping[image];
                if (m_settings.spatialPages && mapping.page != UINT32_MAX && m_pageImages[mapping.page] == 1)
                {
                    // A singleton adds padding without reducing the image count.
                    Exclude(image, Exclusion::Singleton);
                    mapping = {};
                }
            }
            for (size_t page = 0; Active() && page < pages.size(); ++page)
            {
                if (m_pageImages[page] > 1)
                {
                    retainedPixels += pages[page].ownedBytes;
                }
            }
        }
        for (uint32_t image = 0; Active() && image < m_states.size(); ++image)
        {
            if (m_states[image].reason != Exclusion::None)
            {
                Warn(image, m_states[image].reason);
            }
        }
        if (Active() && retainedPixels != 0)
        {
            ReplaceImages(pages);
        }
        return m_published ? retainedPixels : 0;
    }

    void ReplaceImages(std::span<const AtlasPage> pages)
    {
        if (Reserve(m_scene.images.size(), sizeof(ImageAsset)))
        {
            m_images.reserve(m_scene.images.size());
            std::fill(m_pageImages.begin(), m_pageImages.end(), UINT32_MAX);
        }
        uint64_t retainedRecords = m_scene.images.size() * sizeof(ImageAsset);
        uint64_t oldRecords      = retainedRecords;
        for (uint32_t image = 0; Active() && image < m_scene.images.size(); ++image)
        {
            const ImageAsset& original  = m_scene.images[image];
            oldRecords                 += original.name.size() + 1;
            PageMapping& mapping        = m_mapping[image];
            const bool   standalone     = mapping.page == UINT32_MAX;
            const bool   first          = !standalone && m_pageImages[mapping.page] == UINT32_MAX;
            if ((standalone || first) && Reserve(original.name.size() + 1, 1))
            {
                retainedRecords       += original.name.size() + 1;
                const uint32_t target  = static_cast<uint32_t>(m_images.size());
                if (first)
                {
                    m_pageImages[mapping.page] = target;
                }
                m_images.push_back({original.name, standalone ? original.pixels : pages[mapping.page].pixels});
                mapping.id = target;
            }
            else if (!standalone && !first)
            {
                mapping.id = m_pageImages[mapping.page];
            }
        }
        if (Active())
        {
            // Complete all fallible work before changing the scene's references.
            // The existing copied image table is already charged by CookScene.
            m_scratch -= retainedRecords > oldRecords ? retainedRecords - oldRecords : 0;
            m_scene.images.swap(m_images);
            for (TextureAsset& texture : m_scene.textures)
            {
                const PageMapping& mapping = m_mapping[texture.image.value];
                texture.image              = ImageId{mapping.id};
                if (mapping.page != UINT32_MAX)
                {
                    texture.arrayLayer  = mapping.layer;
                    texture.atlasRegion = mapping.region;
                }
            }
            m_published = true;
        }
    }

    Scene&                      m_scene;
    const TextureSettings&      m_settings;
    uint64_t&                   m_remaining;
    std::vector<Diagnostic>&    m_diagnostics;
    const Cancellation*         m_cancellation;
    Status                      m_status;
    uint64_t                    m_scratch{0};
    uint64_t                    m_useCount{0};
    bool                        m_published{false};
    std::vector<ImageState>     m_states;
    std::vector<uint8_t>        m_textureSeen;
    std::vector<uint8_t>        m_materialUsed;
    std::vector<uint8_t>        m_uvMorph;
    std::vector<uint32_t>       m_animated;
    std::vector<uint32_t>       m_meshOffset;
    std::vector<PrimitiveUse>   m_uses;
    std::vector<AtlasTileInput> m_inputs;
    std::vector<ImageAsset>     m_images;
    std::vector<PageMapping>    m_mapping;
    std::vector<uint32_t>       m_pageImages;
};
} // namespace

Status ProcessTexturePages(Scene&                   scene,
                           const TextureSettings&   settings,
                           uint64_t&                remaining,
                           std::vector<Diagnostic>& diagnostics,
                           const Cancellation*      cancellation)
{
    return TexturePageProcessor(scene, settings, remaining, diagnostics, cancellation).Run();
}
} // namespace zenasset::detail
