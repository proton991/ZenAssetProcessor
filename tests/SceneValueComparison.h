#pragma once
#include "ZenAsset/Format/Scene.h"
#include <cstdio>
#include <type_traits>

// Test-only comparison of the public object model. This deliberately does not
// call the binary field visitor: a symmetric serializer omission must still fail.
namespace scene_test
{
using namespace zenasset;
template <class T> struct RecordEqual
{
    static bool Same(const T& a, const T& b)
    {
        return a == b;
    }
};
template <class T> bool Same(const T& a, const T& b)
{
    return RecordEqual<T>::Same(a, b);
}
template <class T> struct RecordEqual<std::vector<T>>
{
    static bool Same(const std::vector<T>& a, const std::vector<T>& b)
    {
        bool equal = a.size() == b.size();
        for (size_t index = 0; equal && index < a.size(); ++index)
        {
            equal = scene_test::Same(a[index], b[index]);
        }
        return equal;
    }
};
template <class T, size_t N> struct RecordEqual<T[N]>
{
    static bool Same(const T (&a)[N], const T (&b)[N])
    {
        bool equal = true;
        for (size_t index = 0; equal && index < N; ++index)
        {
            equal = scene_test::Same(a[index], b[index]);
        }
        return equal;
    }
};
template <class T> struct RecordEqual<std::shared_ptr<const T>>
{
    static bool Same(const std::shared_ptr<const T>& a, const std::shared_ptr<const T>& b)
    {
        return bool(a) == bool(b) && (!a || scene_test::Same(*a, *b));
    }
};
#define FIELD(member) equal &= scene_test::Same(a.member, b.member);
#define RECORD(Type, members)                          \
    template <> struct RecordEqual<Type>               \
    {                                                  \
        static bool Same(const Type& a, const Type& b) \
        {                                              \
            bool equal = true;                         \
            members return equal;                      \
        }                                              \
    };

RECORD(Vertex, FIELD(pos) FIELD(normal) FIELD(tangent) FIELD(uv0) FIELD(uv1) FIELD(joint0) FIELD(weight0) FIELD(color))
RECORD(NodeAsset,
       FIELD(name) FIELD(parent) FIELD(children) FIELD(translation) FIELD(rotation) FIELD(scale) FIELD(baseMatrix)
           FIELD(matrixTransform) FIELD(mesh) FIELD(materials) FIELD(skin) FIELD(camera) FIELD(light) FIELD(morphWeights)
               FIELD(morphWeightsSource) FIELD(visible) FIELD(selectable) FIELD(hoverable) FIELD(renderMesh))
RECORD(VertexJointInfluence, FIELD(joint) FIELD(weight))
RECORD(MeshPrimitiveAsset,
       FIELD(name) FIELD(firstVertex) FIELD(vertexCount) FIELD(firstIndex) FIELD(indexCount) FIELD(material) FIELD(materialSlot)
           FIELD(topology) FIELD(bounds) FIELD(flatNormals) FIELD(sourceVertices) FIELD(influences))
RECORD(MeshAsset, FIELD(name) FIELD(primitives) FIELD(morphWeights))
RECORD(SkinAsset, FIELD(name) FIELD(skeleton) FIELD(joints) FIELD(inverseBindMatrices))
RECORD(CameraAsset,
       FIELD(name) FIELD(orthographic) FIELD(infiniteFar) FIELD(fixedAspect) FIELD(aspect) FIELD(verticalFov) FIELD(nearPlane)
           FIELD(farPlane) FIELD(xmag) FIELD(ymag))
RECORD(LightAsset,
       FIELD(name) FIELD(kind) FIELD(color) FIELD(intensity) FIELD(range) FIELD(innerConeAngle) FIELD(outerConeAngle))
RECORD(MaterialAsset,
       FIELD(name) FIELD(alphaMode) FIELD(doubleSided) FIELD(alphaCutoff) FIELD(metallicFactor) FIELD(roughnessFactor)
           FIELD(normalScale) FIELD(occlusionStrength) FIELD(textureTransforms) FIELD(features) FIELD(baseColorFactor)
               FIELD(emissiveFactor) FIELD(m_pBaseColorTexture) FIELD(m_pMetallicRoughnessTexture) FIELD(m_pNormalTexture)
                   FIELD(m_pOcclusionTexture) FIELD(m_pEmissiveTexture) FIELD(unlit) FIELD(emissiveStrength)
                       FIELD(texCoordSets.baseColor) FIELD(texCoordSets.metallicRoughness)
                           FIELD(texCoordSets.specularGlossiness) FIELD(texCoordSets.normal) FIELD(texCoordSets.occlusion)
                               FIELD(texCoordSets.emissive) FIELD(extension.pSpecularGlossinessTexture)
                                   FIELD(extension.pDiffuseTexture) FIELD(extension.diffuseFactor)
                                       FIELD(extension.specularFactor) FIELD(pbrWorkflows.metallicRoughness)
                                           FIELD(pbrWorkflows.specularGlossiness))
RECORD(ImagePixels, FIELD(width) FIELD(height) FIELD(format) FIELD(bytes) FIELD(mips) FIELD(arrayLayers))
RECORD(ImageAsset, FIELD(name) FIELD(pixels))
RECORD(TextureAsset,
       FIELD(name) FIELD(image) FIELD(sampler) FIELD(linearSource) FIELD(format) FIELD(flatNormal) FIELD(interpretation)
           FIELD(arrayLayer) FIELD(atlasRegion))
RECORD(SamplerAsset, FIELD(name) FIELD(minFilter) FIELD(magFilter) FIELD(mipFilter) FIELD(useMipmaps) FIELD(wrapS) FIELD(wrapT))
RECORD(MorphColorAttributeAsset, FIELD(set) FIELD(values))
RECORD(MorphTargetAsset, FIELD(positions) FIELD(normals) FIELD(tangents) FIELD(texCoords) FIELD(colors) FIELD(extraColors))
RECORD(MorphPrimitiveAsset, FIELD(mesh) FIELD(primitive) FIELD(weights) FIELD(targets))
RECORD(MaterialVariantPrimitiveAsset, FIELD(mesh) FIELD(primitive) FIELD(materials))
RECORD(VertexAttributeAsset,
       FIELD(mesh) FIELD(primitive) FIELD(semantic) FIELD(morphTarget) FIELD(components) FIELD(componentType) FIELD(normalized)
           FIELD(domain) FIELD(values))
RECORD(EnvironmentCubePixels, FIELD(format) FIELD(size) FIELD(mipLevels) FIELD(rgba32FloatMipFaces) FIELD(blockMipFaces))
RECORD(ImageBasedLightAsset, FIELD(name) FIELD(intensity) FIELD(rotation) FIELD(irradianceCoefficients) FIELD(specular))
RECORD(AnimationBakeSettings,
       FIELD(sampleRate) FIELD(translationTolerance) FIELD(rotationTolerance) FIELD(scaleTolerance) FIELD(valueTolerance)
           FIELD(maxSubdivisions) FIELD(maxValues))
RECORD(AnimationTarget, FIELD(object) FIELD(index) FIELD(property) FIELD(dimensions) FIELD(element) FIELD(textureSlot))
RECORD(AnimationSampler, FIELD(interpolation) FIELD(components) FIELD(times) FIELD(values))
RECORD(AnimationChannel, FIELD(sampler) FIELD(node) FIELD(path) FIELD(target) FIELD(sourceTarget))
RECORD(AnimationAsset, FIELD(name) FIELD(samplers) FIELD(channels) FIELD(baked) FIELD(bakeSettings))
RECORD(SceneDependency, FIELD(path) FIELD(exists) FIELD(size) FIELD(modified) FIELD(contentHash))
RECORD(Scene,
       FIELD(name) FIELD(sourcePath) FIELD(dependencies) FIELD(sourceDocument) FIELD(nodes) FIELD(roots) FIELD(meshes)
           FIELD(vertices) FIELD(indices) FIELD(vertexTexCoords) FIELD(vertexTexCoordSets) FIELD(materials) FIELD(images)
               FIELD(textures) FIELD(samplers) FIELD(skins) FIELD(animations) FIELD(morphPrimitives) FIELD(cameras)
                   FIELD(lights) FIELD(materialVariants) FIELD(variantPrimitives) FIELD(extraVertexAttributes)
                       FIELD(imageBasedLights) FIELD(imageBasedLight))
#undef RECORD
#undef FIELD
} // namespace scene_test
