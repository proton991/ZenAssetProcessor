#pragma once
#include "ZenAsset/Format/Animation.h"
#include "ZenAsset/Format/Image.h"
#include "ZenAsset/Format/Material.h"
#include "ZenAsset/Format/Math.h"
#include <string>
#include <vector>

namespace zenasset
{
// Homogeneous position (w = 1), normal (w = 0) and tangent (w = handedness).
// joint0/weight0 retain the first four influences for compatibility; a primitive's
// complete influence arrays are authoritative when present.
struct Vertex
{
    Vec4 pos{0, 0, 0, 1};
    Vec4 normal{};
    Vec4 tangent{};
    Vec2 uv0{};
    Vec2 uv1{};
    Vec4 joint0{};
    Vec4 weight0{};
    Vec4 color{1.0f};
};
using VertexTextureCoordinates = std::vector<Vec2>;

enum class MeshTopology : uint32_t
{
    Points,
    Lines,
    Triangles
};

struct CameraParameters
{
    bool  orthographic{false};
    bool  infiniteFar{false};
    bool  fixedAspect{false};
    float aspect{1.0f};
    float verticalFov{1.2217304763960306f};
    float nearPlane{0.001f};
    float farPlane{100.0f};
    float xmag{1.0f};
    float ymag{1.0f};
};

struct SkinAsset
{
    std::string           name;
    int32_t               skeleton{-1};
    std::vector<uint32_t> joints;
    std::vector<Mat4>     inverseBindMatrices;
};

struct MorphColorAttributeAsset
{
    uint64_t          set{0};
    std::vector<Vec4> values;
};

struct MorphTargetAsset
{
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec3> tangents;
    // Coordinate sets use Scene::vertexTexCoordSets compact slots.
    std::vector<std::vector<Vec2>>        texCoords;
    std::vector<Vec4>                     colors;
    std::vector<MorphColorAttributeAsset> extraColors;
};

struct MorphPrimitiveAsset
{
    uint32_t mesh{0};
    // UINT32_MAX applies a shared control-point payload to every mesh primitive.
    uint32_t                      primitive{0};
    std::vector<float>            weights;
    std::vector<MorphTargetAsset> targets;
};

struct VertexJointInfluence
{
    uint32_t joint{0};
    float    weight{0.0f};
};

struct MaterialVariantPrimitiveAsset
{
    uint32_t mesh{0};
    uint32_t primitive{0};
    // -1 retains the primitive's default material for that variant.
    std::vector<int32_t> materials;
};

enum class VertexAttributeDomain : uint32_t
{
    SourceVertex,
    PrimitiveVertex
};

struct VertexAttributeAsset
{
    uint32_t    mesh{0};
    uint32_t    primitive{0};
    std::string semantic;
    // -1 identifies base attributes; otherwise this is a primitive-local morph target.
    int32_t  morphTarget{-1};
    uint32_t components{0};
    uint32_t componentType{0};
    bool     normalized{false};
    // SourceVertex follows the primitive's sourceVertices mapping; PrimitiveVertex
    // is already expanded to final corners (needed for FBX per-corner color sets).
    VertexAttributeDomain domain{VertexAttributeDomain::SourceVertex};
    // Double precision retains integer custom attributes exactly and decoded normalized values.
    std::vector<double> values;
};

struct ImageBasedLightAsset
{
    std::string                                  name;
    float                                        intensity{1.0f};
    Quat                                         rotation{1.0f, 0.0f, 0.0f, 0.0f};
    std::vector<Vec3>                            irradianceCoefficients;
    std::shared_ptr<const EnvironmentCubePixels> specular;
};


// Canonical coordinates are right-handed, Y-up, meters and seconds. Asset IDs
// index their owning Scene arrays; no parser or runtime object is retained.
struct NodeAsset
{
    std::string         name;
    NodeId              parent;
    std::vector<NodeId> children;
    Vec3                translation{0.0f};
    Quat                rotation{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3                scale{1.0f};
    // Local transform is T * R * S * baseMatrix, retaining affine/sheared nodes.
    Mat4   baseMatrix{1.0f};
    bool   matrixTransform{false};
    MeshId mesh;
    // Optional per-instance bindings, indexed by the mesh primitive's material slot.
    std::vector<MaterialId> materials;
    SkinId                  skin;
    CameraId                camera;
    LightId                 light;
    std::vector<float>      morphWeights;
    NodeId                  morphWeightsSource;
    bool                    visible{true};
    bool                    selectable{true};
    bool                    hoverable{true};
    // An instancing parent can carry mesh/weight state without drawing itself.
    bool renderMesh{true};
};

struct MeshPrimitiveAsset
{
    std::string  name;
    uint32_t     firstVertex{0};
    uint32_t     vertexCount{0};
    uint32_t     firstIndex{0};
    uint32_t     indexCount{0};
    MaterialId   material;
    uint32_t     materialSlot{UINT32_MAX};
    MeshTopology topology{MeshTopology::Triangles};
    Bounds       bounds;
    bool         flatNormals{false};
    // Final vertex -> authored vertex, used by morph/custom attributes.
    std::vector<uint32_t>                          sourceVertices;
    std::vector<std::vector<VertexJointInfluence>> influences;
};

struct MeshAsset
{
    std::string                     name;
    std::vector<MeshPrimitiveAsset> primitives;
    std::vector<float>              morphWeights;
};

struct CameraAsset : CameraParameters
{
    std::string name;
};

enum class LightKind : uint32_t
{
    Directional,
    Point,
    Spot
};

// KHR_lights_punctual's default intensity: candela for point and spot lights, lux for directional.
constexpr float DefaultLightIntensity = 1.0f;

struct LightAsset
{
    std::string name;
    LightKind   kind{LightKind::Point};
    Vec3        color{1.0f};
    float       intensity{1.0f};
    float       range{0.0f};
    float       innerConeAngle{0.0f};
    float       outerConeAngle{0.7853981633974483f};
};

// A file revision consumed while importing, identified by its size and last-write
// time when it was read. exists == false records a missing preferred file whose
// later appearance changes resource resolution. Cache publication fills
// contentHash (XXH64 of the bytes) only if that revision is still on disk; cache
// lookups then compare content, so touched-but-unchanged files still hit.
struct SceneDependency
{
    std::string path;
    bool        exists{true};
    uint64_t    size{0};
    int64_t     modified{0};
    uint64_t    contentHash{0};
};

struct Scene
{
    std::string name;
    std::string sourcePath;
    // External files actually consumed while importing (embedded images need no path).
    std::vector<SceneDependency> dependencies;
    // Optional source metadata for inspection/round-tripping, never geometry ownership.
    std::string                                sourceDocument;
    std::vector<NodeAsset>                     nodes;
    std::vector<NodeId>                        roots;
    std::vector<MeshAsset>                     meshes;
    std::vector<Vertex>                        vertices;
    std::vector<uint32_t>                      indices;
    std::vector<VertexTextureCoordinates>      vertexTexCoords;
    std::vector<uint64_t>                      vertexTexCoordSets;
    std::vector<MaterialAsset>                 materials;
    std::vector<ImageAsset>                    images;
    std::vector<TextureAsset>                  textures;
    std::vector<SamplerAsset>                  samplers;
    std::vector<SkinAsset>                     skins;
    std::vector<AnimationAsset>                animations;
    std::vector<MorphPrimitiveAsset>           morphPrimitives;
    std::vector<CameraAsset>                   cameras;
    std::vector<LightAsset>                    lights;
    std::vector<std::string>                   materialVariants;
    std::vector<MaterialVariantPrimitiveAsset> variantPrimitives;
    std::vector<VertexAttributeAsset>          extraVertexAttributes;
    std::vector<ImageBasedLightAsset>          imageBasedLights;
    int32_t                                    imageBasedLight{-1};
};

} // namespace zenasset
