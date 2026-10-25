#pragma once
#include "Processing/TextureImage.h"
#include "ZenAsset/Format/Material.h"

namespace zenasset::detail
{
struct MaterialTextureUse
{
    TextureId*              texture;
    TextureImageRole        role;
    uint32_t                texCoord;
    const TextureTransform* transform;
    uint32_t                slot;
    bool                    opacity;
    float                   factor;
    uint8_t                 animationMask;
};

inline TextureImageRole FeatureTextureRole(MaterialFeatureTexture feature)
{
    TextureImageRole role = TextureImageRole::Data;
    switch (feature)
    {
        case MaterialFeatureTexture::SpecularColor:
        case MaterialFeatureTexture::Diffuse:
        case MaterialFeatureTexture::SpecularGlossiness:
        case MaterialFeatureTexture::SheenColor:
        case MaterialFeatureTexture::DiffuseTransmissionColor: role = TextureImageRole::Color; break;
        case MaterialFeatureTexture::ClearcoatNormal: role = TextureImageRole::Normal; break;
        default: break;
    }
    return role;
}

// One shared enumeration for compression and page eligibility, including legacy
// references and every extension slot. The record pointers borrow the material.
inline constexpr size_t MaterialTextureUseCount = 7 + MaterialFeatures::textureMembers.size();
inline std::array<MaterialTextureUse, MaterialTextureUseCount> MaterialTextureUses(MaterialAsset& material)
{
    std::array<MaterialTextureUse, MaterialTextureUseCount> uses{};
    uses[0] = {&material.m_pBaseColorTexture,
               TextureImageRole::Color,
               material.texCoordSets.baseColor,
               &material.textureTransforms[0],
               0,
               true,
               material.baseColorFactor.w,
               1};
    uses[1] = {&material.m_pMetallicRoughnessTexture,
               TextureImageRole::Data,
               material.texCoordSets.metallicRoughness,
               &material.textureTransforms[1],
               1,
               false,
               1,
               0};
    uses[2] = {&material.m_pNormalTexture,
               TextureImageRole::Normal,
               material.texCoordSets.normal,
               &material.textureTransforms[2],
               2,
               false,
               material.normalScale,
               4};
    uses[3] = {&material.m_pOcclusionTexture,
               TextureImageRole::Occlusion,
               material.texCoordSets.occlusion,
               &material.textureTransforms[3],
               3,
               false,
               1,
               0};
    uses[4] = {&material.m_pEmissiveTexture,
               TextureImageRole::Color,
               material.texCoordSets.emissive,
               &material.textureTransforms[4],
               4,
               false,
               1,
               0};
    for (uint32_t slot = 0; slot < MaterialFeatures::textureMembers.size(); ++slot)
    {
        const MaterialFeatureTexture feature = static_cast<MaterialFeatureTexture>(slot);
        MaterialTextureBinding&      binding = material.features.*MaterialFeatures::textureMembers[slot];
        const bool                   normal  = feature == MaterialFeatureTexture::ClearcoatNormal;
        uses[5 + slot]                       = {&binding.texture,
                                                FeatureTextureRole(feature),
                                                binding.texCoord,
                                                &binding.transform,
                                                5 + slot,
                                                feature == MaterialFeatureTexture::Diffuse,
                                                normal ? binding.scale : material.extension.diffuseFactor.w,
                                                uint8_t(normal ? 8 : 2)};
    }
    const size_t legacy = 5 + MaterialFeatures::textureMembers.size();
    uses[legacy]        = {&material.extension.pDiffuseTexture,
                           TextureImageRole::Color,
                           material.texCoordSets.baseColor,
                           &material.textureTransforms[0],
                           5 + uint32_t(MaterialFeatureTexture::Diffuse),
                           true,
                           material.extension.diffuseFactor.w,
                           2};
    uses[legacy + 1]    = {&material.extension.pSpecularGlossinessTexture,
                           TextureImageRole::Color,
                           material.texCoordSets.specularGlossiness,
                           &material.textureTransforms[1],
                           5 + uint32_t(MaterialFeatureTexture::SpecularGlossiness),
                           false,
                           1,
                           0};
    return uses;
}
} // namespace zenasset::detail
