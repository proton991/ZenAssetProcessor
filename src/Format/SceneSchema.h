#pragma once
#include "ZenAsset/Reader/Scene.h"
#include "ZenAsset/Format/Utf8.h"
#include <bit>
#include <cmath>
#include <cstring>
#include <type_traits>

namespace zenasset::detail
{
// Each list below is the schema, shared by the reader and writer. Integers and
// IEEE float bit patterns use little-endian order, bools one byte, enums/IDs u32,
// and arrays/UTF-8 strings a u64 count followed by their semantic elements.
template <class Archive> void Fields(Archive& ar, typename Archive::template Value<Vec2>& v)
{
    ar(v.x, v.y);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<Vec3>& v)
{
    ar(v.x, v.y, v.z);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<Vec4>& v)
{
    ar(v.x, v.y, v.z, v.w);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<Quat>& v)
{
    ar(v.w, v.x, v.y, v.z);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<Vertex>& v)
{
    ar(v.pos, v.normal, v.tangent, v.uv0, v.uv1, v.joint0, v.weight0, v.color);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<NodeAsset>& v)
{
    ar(v.name, v.parent, v.children, v.translation, v.rotation, v.scale, v.baseMatrix, v.matrixTransform, v.mesh, v.materials,
       v.skin, v.camera, v.light, v.morphWeights, v.morphWeightsSource, v.visible, v.selectable, v.hoverable, v.renderMesh);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<MeshPrimitiveAsset>& v)
{
    ar(v.name, v.firstVertex, v.vertexCount, v.firstIndex, v.indexCount, v.material, v.materialSlot, v.topology, v.bounds,
       v.flatNormals, v.sourceVertices, v.influences);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<MeshAsset>& v)
{
    ar(v.name, v.primitives, v.morphWeights);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<CameraAsset>& v)
{
    ar(v.name, v.orthographic, v.infiniteFar, v.fixedAspect, v.aspect, v.verticalFov, v.nearPlane, v.farPlane, v.xmag, v.ymag);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<LightAsset>& v)
{
    ar(v.name, v.kind, v.color, v.intensity, v.range, v.innerConeAngle, v.outerConeAngle);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<SkinAsset>& v)
{
    ar(v.name, v.skeleton, v.joints, v.inverseBindMatrices);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<AnimationTarget>& v)
{
    ar(v.object, v.index, v.property, v.dimensions, v.element, v.textureSlot);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<AnimationSampler>& v)
{
    ar(v.interpolation, v.components, v.times, v.values);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<AnimationChannel>& v)
{
    ar(v.sampler, v.node, v.path, v.target, v.sourceTarget);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<AnimationBakeSettings>& v)
{
    ar(v.sampleRate, v.translationTolerance, v.rotationTolerance, v.scaleTolerance, v.valueTolerance, v.maxSubdivisions,
       v.maxValues);
    if (!std::isfinite(v.sampleRate) || v.sampleRate <= 0 || !std::isfinite(v.translationTolerance)
        || v.translationTolerance < 0 || !std::isfinite(v.rotationTolerance) || v.rotationTolerance < 0
        || !std::isfinite(v.scaleTolerance) || v.scaleTolerance < 0 || !std::isfinite(v.valueTolerance) || v.valueTolerance < 0
        || v.maxValues == 0)
    {
        ar.Fail(StatusCode::InvalidData, "Scene animation bake settings are invalid");
    }
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<AnimationAsset>& v)
{
    ar(v.name, v.samplers, v.channels, v.baked, v.bakeSettings);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<MorphColorAttributeAsset>& v)
{
    ar(v.set, v.values);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<MorphTargetAsset>& v)
{
    ar(v.positions, v.normals, v.tangents, v.texCoords, v.colors, v.extraColors);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<MorphPrimitiveAsset>& v)
{
    ar(v.mesh, v.primitive, v.weights, v.targets);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<VertexJointInfluence>& v)
{
    ar(v.joint, v.weight);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<MaterialVariantPrimitiveAsset>& v)
{
    ar(v.mesh, v.primitive, v.materials);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<VertexAttributeAsset>& v)
{
    ar(v.mesh, v.primitive, v.semantic, v.morphTarget, v.components, v.componentType, v.normalized, v.domain, v.values);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<EnvironmentCubePixels>& v)
{
    if (ar.Version() >= 2)
    {
        ar(v.format);
    }
    ar(v.size, v.mipLevels);
    if (v.format == PixelFormat::RGBA32Float)
    {
        ar(v.rgba32FloatMipFaces);
    }
    else if (v.format == PixelFormat::BC6HUfloat && ar.Version() >= 2)
    {
        ar(v.blockMipFaces);
    }
    else
    {
        ar.Fail(StatusCode::UnsupportedFeature, "Unsupported environment cube format");
    }
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<ImageBasedLightAsset>& v)
{
    ar(v.name, v.intensity, v.rotation, v.irradianceCoefficients);
    if (ar.Reserve(1, sizeof(EnvironmentCubePixels)))
    {
        if constexpr (Archive::Reading)
        {
            std::shared_ptr<EnvironmentCubePixels> pixels(new EnvironmentCubePixels);
            ar(*pixels);
            v.specular = std::move(pixels);
        }
        else if (v.specular)
        {
            ar(*v.specular);
        }
        else
        {
            ar.Fail(StatusCode::InvalidData, "Environment cube storage is missing");
        }
    }
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<TextureTransform>& v)
{
    ar(v.row0, v.row1, v.uvOffset, v.uvScale, v.uvRotation);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<MaterialTextureBinding>& v)
{
    ar(v.texture, v.texCoord, v.transform, v.scale);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<MaterialFeatureFactors>& v)
{
    ar(v.ior, v.dispersion, v.specular, v.specularColor, v.glossiness, v.clearcoat, v.clearcoatRoughness, v.sheenColor,
       v.sheenRoughness, v.transmission, v.thickness, v.attenuationColor, v.attenuationDistance, v.iridescence,
       v.iridescenceIor, v.iridescenceThicknessMin, v.iridescenceThicknessMax, v.anisotropy, v.anisotropyRotation,
       v.diffuseTransmission, v.diffuseTransmissionColor, v.multiscatterColor, v.retroreflection);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<SamplerAsset>& v)
{
    ar(v.name, v.minFilter, v.magFilter, v.mipFilter, v.useMipmaps, v.wrapS, v.wrapT);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<TextureAsset>& v)
{
    ar(v.name, v.image, v.sampler, v.linearSource, v.format, v.flatNormal);
    if (ar.Version() >= 2)
    {
        ar(v.interpretation);
    }
    if (ar.Version() >= 3)
    {
        ar(v.arrayLayer);
    }
    if (ar.Version() >= 4)
    {
        ar(v.atlasRegion.x, v.atlasRegion.y, v.atlasRegion.width, v.atlasRegion.height);
    }
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<ImagePixels>& v)
{
    ar(v.width, v.height, v.format, v.bytes, v.mips);
    if (ar.Version() >= 2)
    {
        ar(v.arrayLayers);
    }
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<SourceSettings>& v)
{
    ar(v.sceneIndex, v.animationBake, v.fallbackMissingNamedUVSets);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<Diagnostic>& v)
{
    ar(v.severity, v.status, v.source, v.object, v.message);
    if (v.severity > DiagnosticSeverity::Error || v.status > StatusCode::UnsupportedFeature)
    {
        ar.Fail(StatusCode::InvalidData, "Scene diagnostic has an unknown severity or status");
    }
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<SceneDependency>& v)
{
    ar(v.path, v.exists, v.size, v.modified, v.contentHash);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<CompressionSettings>& v)
{
    ar(v.transport, v.zstandardLevel);
    if (ar.Version() >= 3)
    {
        ar(v.meshoptimizer);
    }
    if ((v.transport != TransportCodec::None && v.transport != TransportCodec::Zstandard) || v.zstandardLevel < 1
        || v.zstandardLevel > 19)
    {
        ar.Fail(StatusCode::InvalidData, "Scene compression settings are invalid");
    }
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<GeometrySettings>& v)
{
    ar(v.optimizeVertexFetch, v.positionQuantizationBits, v.positionMaxRelativeError, v.positionMaxAbsoluteError);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<TextureSettings>& v)
{
    ar(v.generateMipmaps, v.compressBC7, v.bc7Quality, v.maxDimension, v.colorMinPsnr, v.colorMaxRmse, v.scalarMaxError,
       v.normalRmsAngleDegrees, v.normalMaxAngleDegrees, v.alphaCoverageMaxError);
    if (ar.Version() >= 6)
    {
        ar(v.compressBC5Normals, v.compressBC4Occlusion, v.compressBC6HEnvironment, v.hdrMaxRelativeRmse);
    }
    if (ar.Version() >= 7)
    {
        ar(v.arrayPages, v.arrayMaxDimension, v.arrayMaxLayers, v.arrayMaxPages, v.arrayMaxImages);
    }
    if (ar.Version() >= 8)
    {
        ar(v.spatialPages, v.spatialMaxDimension, v.spatialMaxPages, v.spatialMaxImages);
    }
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<ProcessingSettings>& v)
{
    ar(v.geometry);
    if (ar.Version() >= 5)
    {
        ar(v.textures);
    }
    if (!ValidateProcessingSettings(v).Succeeded())
    {
        ar.Fail(StatusCode::InvalidData, "Scene processing settings are invalid");
    }
}

template <class Archive, class Metadata, class Compression>
void BuildMetadataFields(Archive& ar, Metadata& v, Compression& compression)
{
    ar(v.sourceFormat, v.importerVersion, v.processorVersion);
    if constexpr (Archive::Reading)
    {
        ar(v.buildFingerprint);
    }
    else
    {
        const std::string fingerprint = v.buildFingerprint.empty() ? std::string(LibraryBuildFingerprint()) : std::string{};
        ar(v.buildFingerprint.empty() ? fingerprint : v.buildFingerprint);
    }
    ar(v.profile, v.settings, v.cacheKey, v.inputs, v.diagnostics);
    if (ar.Version() >= 2)
    {
        ar(compression);
    }
    if (ar.Version() >= 4)
    {
        ar(v.processing);
    }
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<BuildMetadata>& v)
{
    BuildMetadataFields(ar, v, v.compression);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<Mat4>& v)
{
    for (uint32_t column = 0; column < 4; ++column)
    {
        ar(v[column]);
    }
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<Bounds>& v)
{
    ar(v.min, v.max);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<MaterialAsset>& v)
{
    ar(v.name, v.alphaMode, v.doubleSided, v.alphaCutoff, v.metallicFactor, v.roughnessFactor, v.normalScale,
       v.occlusionStrength);
    for (uint32_t slot = 0; slot < 5; ++slot)
    {
        ar(v.textureTransforms[slot]);
    }
    ar(static_cast<typename Archive::template Value<MaterialFeatureFactors>&>(v.features));
    for (MaterialFeatures::TextureMember member : MaterialFeatures::textureMembers)
    {
        ar(v.features.*member);
    }
    ar(v.baseColorFactor, v.emissiveFactor, v.m_pBaseColorTexture, v.m_pMetallicRoughnessTexture, v.m_pNormalTexture,
       v.m_pOcclusionTexture, v.m_pEmissiveTexture, v.texCoordSets.baseColor, v.texCoordSets.metallicRoughness,
       v.texCoordSets.specularGlossiness, v.texCoordSets.normal, v.texCoordSets.occlusion, v.texCoordSets.emissive,
       v.extension.pSpecularGlossinessTexture, v.extension.pDiffuseTexture, v.extension.diffuseFactor,
       v.extension.specularFactor, v.pbrWorkflows.metallicRoughness, v.pbrWorkflows.specularGlossiness, v.unlit,
       v.emissiveStrength);
}

template <class Archive> void Fields(Archive& ar, typename Archive::template Value<ImageAsset>& v)
{
    ar(v.name);
    if (ar.Reserve(1, sizeof(ImagePixels)))
    {
        if constexpr (Archive::Reading)
        {
            std::shared_ptr<ImagePixels> pixels(new ImagePixels);
            ar(*pixels);
            v.pixels = std::move(pixels);
        }
        else
        {
            ar(*v.pixels);
        }
    }
}


} // namespace zenasset::detail
