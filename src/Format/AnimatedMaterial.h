#pragma once
#include "ZenAsset/Format/Scene.h"
#include <span>

namespace zenasset
{
template <class TextureReference>
std::span<float> GetAnimatedMaterialProperty(MaterialParametersT<TextureReference>& material, AnimatedProperty property)
{
    std::span<float> result;
    switch (property)
    {
        case AnimatedProperty::BaseColor: result = std::span<float>(&material.baseColorFactor[0], 4); break;
        case AnimatedProperty::Metallic: result = std::span<float>(&material.metallicFactor, 1); break;
        case AnimatedProperty::Roughness: result = std::span<float>(&material.roughnessFactor, 1); break;
        case AnimatedProperty::Emissive: result = std::span<float>(&material.emissiveFactor[0], 3); break;
        case AnimatedProperty::AlphaCutoff: result = std::span<float>(&material.alphaCutoff, 1); break;
        case AnimatedProperty::NormalScale: result = std::span<float>(&material.normalScale, 1); break;
        case AnimatedProperty::OcclusionStrength: result = std::span<float>(&material.occlusionStrength, 1); break;
        case AnimatedProperty::EmissiveStrength: result = std::span<float>(&material.emissiveStrength, 1); break;
        case AnimatedProperty::Ior: result = std::span<float>(&material.features.ior, 1); break;
        case AnimatedProperty::Dispersion: result = std::span<float>(&material.features.dispersion, 1); break;
        case AnimatedProperty::Specular: result = std::span<float>(&material.features.specular, 1); break;
        case AnimatedProperty::SpecularColor: result = std::span<float>(&material.features.specularColor[0], 3); break;
        case AnimatedProperty::Diffuse: result = std::span<float>(&material.extension.diffuseFactor[0], 4); break;
        case AnimatedProperty::SpecularGlossinessColor:
            result = std::span<float>(&material.extension.specularFactor[0], 3);
            break;
        case AnimatedProperty::Glossiness: result = std::span<float>(&material.features.glossiness, 1); break;
        case AnimatedProperty::Clearcoat: result = std::span<float>(&material.features.clearcoat, 1); break;
        case AnimatedProperty::ClearcoatRoughness: result = std::span<float>(&material.features.clearcoatRoughness, 1); break;
        case AnimatedProperty::ClearcoatNormalScale:
            result = std::span<float>(&material.features.clearcoatNormalTexture.scale, 1);
            break;
        case AnimatedProperty::SheenColor: result = std::span<float>(&material.features.sheenColor[0], 3); break;
        case AnimatedProperty::SheenRoughness: result = std::span<float>(&material.features.sheenRoughness, 1); break;
        case AnimatedProperty::Transmission: result = std::span<float>(&material.features.transmission, 1); break;
        case AnimatedProperty::Thickness: result = std::span<float>(&material.features.thickness, 1); break;
        case AnimatedProperty::AttenuationDistance: result = std::span<float>(&material.features.attenuationDistance, 1); break;
        case AnimatedProperty::AttenuationColor: result = std::span<float>(&material.features.attenuationColor[0], 3); break;
        case AnimatedProperty::Iridescence: result = std::span<float>(&material.features.iridescence, 1); break;
        case AnimatedProperty::IridescenceIor: result = std::span<float>(&material.features.iridescenceIor, 1); break;
        case AnimatedProperty::IridescenceThicknessMinimum:
            result = std::span<float>(&material.features.iridescenceThicknessMin, 1);
            break;
        case AnimatedProperty::IridescenceThicknessMaximum:
            result = std::span<float>(&material.features.iridescenceThicknessMax, 1);
            break;
        case AnimatedProperty::Anisotropy: result = std::span<float>(&material.features.anisotropy, 1); break;
        case AnimatedProperty::AnisotropyRotation: result = std::span<float>(&material.features.anisotropyRotation, 1); break;
        case AnimatedProperty::DiffuseTransmission: result = std::span<float>(&material.features.diffuseTransmission, 1); break;
        case AnimatedProperty::DiffuseTransmissionColor:
            result = std::span<float>(&material.features.diffuseTransmissionColor[0], 3);
            break;
        case AnimatedProperty::MultiscatterColor: result = std::span<float>(&material.features.multiscatterColor[0], 3); break;
        case AnimatedProperty::Retroreflection: result = std::span<float>(&material.features.retroreflection, 1); break;
        default: break;
    }
    return result;
}
} // namespace zenasset
