#pragma once
#include "ZenAsset/Reader/Scene.h"
#include <array>

namespace zenasset::detail
{
inline constexpr size_t ResourceTypeCount = static_cast<size_t>(ResourceType::SourceDocument) + 1;
using ResourceCounts                      = std::array<uint64_t, ResourceTypeCount>;
struct SceneManifest
{
    std::string           name;
    std::vector<NodeId>   roots;
    std::vector<uint64_t> texCoordSets;
    int32_t               imageBasedLight{-1};
    ResourceCounts        counts{};
};
struct SceneMetadata
{
    BuildMetadata                build;
    std::string                  sourcePath;
    std::vector<SceneDependency> dependencies;
};
struct SceneManifestView
{
    const std::string&           name;
    const std::vector<NodeId>&   roots;
    const std::vector<uint64_t>& texCoordSets;
    const int32_t&               imageBasedLight;
    const ResourceCounts&        counts;
};
struct SceneMetadataView
{
    const BuildMetadata&                build;
    const std::string&                  sourcePath;
    const std::vector<SceneDependency>& dependencies;
    const CompressionSettings&          compression;
};
inline uint64_t ResourceId(ResourceType type, uint32_t index = 0)
{
    return (uint64_t(type) << 32) | (uint64_t(index) + 1);
}
inline bool FitsValidationBudget(const Scene& scene, uint64_t budget)
{
    // Reference validation holds parent/visited/root marks simultaneously;
    // geometry validation separately sorts a copy of the coordinate-set list.
    return scene.nodes.size() <= budget / (sizeof(uint32_t) + 2 * sizeof(uint8_t))
        && scene.vertexTexCoordSets.size() <= budget / sizeof(uint64_t);
}
inline SceneReadLimits ResourceLimits(ResourceType type, const SceneReadLimits& limits)
{
    SceneReadLimits result = limits;
    if (type == ResourceType::SourceDocument)
    {
        result.maxStringBytes = limits.maxSourceDocumentBytes;
    }
    return result;
}

// Resource ordering and the division between resources are shared by the
// manifest counter, reader and writer. Visit in ascending ResourceType order.
template <class Visitor, class SceneType> void VisitSceneResources(Visitor& visitor, SceneType& scene)
{
    visitor.Array(ResourceType::Mesh, scene.meshes);
    visitor.Array(ResourceType::Material, scene.materials);
    visitor.Array(ResourceType::Texture, scene.textures);
    visitor.Array(ResourceType::Animation, scene.animations);
    visitor.Array(ResourceType::Skin, scene.skins);
    visitor.Array(ResourceType::Morph, scene.morphPrimitives);
    visitor.Array(ResourceType::Environment, scene.imageBasedLights);
    visitor.Metadata(scene);
    visitor.Single(ResourceType::VertexStream, scene.vertices);
    visitor.Single(ResourceType::IndexStream, scene.indices);
    visitor.Array(ResourceType::CustomAttributes, scene.extraVertexAttributes);
    visitor.Array(ResourceType::Image, scene.images);
    visitor.Array(ResourceType::Node, scene.nodes);
    visitor.Array(ResourceType::Sampler, scene.samplers);
    visitor.Array(ResourceType::Camera, scene.cameras);
    visitor.Array(ResourceType::Light, scene.lights);
    visitor.Single(ResourceType::MaterialVariants, scene.materialVariants);
    visitor.Array(ResourceType::MaterialVariant, scene.variantPrimitives);
    visitor.Single(ResourceType::TextureCoordinates, scene.vertexTexCoords);
    visitor.Single(ResourceType::SourceDocument, scene.sourceDocument);
}

template <class Archive, class Manifest> void ManifestFields(Archive& ar, Manifest& value)
{
    ar(value.name, value.roots, value.texCoordSets, value.imageBasedLight);
    for (size_t index = 0; index < value.counts.size(); ++index)
    {
        ar(value.counts[index]);
    }
}
template <class Archive, class Metadata> void MetadataFields(Archive& ar, Metadata& value)
{
    ar(value.build, value.sourcePath, value.dependencies);
}
template <class Archive> void Fields(Archive& ar, typename Archive::template Value<SceneManifest>& value)
{
    ManifestFields(ar, value);
}
template <class Archive> void Fields(Archive& ar, const SceneManifestView& value)
{
    ManifestFields(ar, value);
}
template <class Archive> void Fields(Archive& ar, typename Archive::template Value<SceneMetadata>& value)
{
    MetadataFields(ar, value);
}
template <class Archive> void Fields(Archive& ar, const SceneMetadataView& value)
{
    BuildMetadataFields(ar, value.build, value.compression);
    ar(value.sourcePath, value.dependencies);
}
} // namespace zenasset::detail
