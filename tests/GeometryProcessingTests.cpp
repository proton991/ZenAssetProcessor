#include "Processing/GeometryProcessing.h"
#include "SceneValueComparison.h"
#include <cstdio>
#include <cmath>

namespace
{
using namespace zenasset;
uint32_t checks = 0;
bool     CancelDuringProcessing(void* user)
{
    uint32_t& polls = *static_cast<uint32_t*>(user);
    return ++polls >= 5;
}
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
    scene.vertices.resize(5);
    scene.vertexTexCoordSets = {0, 5};
    scene.vertexTexCoords.resize(5);
    for (uint32_t index = 0; index < 5; ++index)
    {
        scene.vertices[index].pos    = Vec4(float(index), float(index) * 0.1f, 0, 1);
        scene.vertices[index].normal = Vec4(float(index), 0, 1, 0);
        scene.vertexTexCoords[index] = {Vec2(float(index), 1), Vec2(float(index), 2)};
    }
    scene.indices = {3, 1, 3, 0};
    scene.meshes.resize(1);
    MeshPrimitiveAsset primitive;
    primitive.vertexCount = 5;
    primitive.indexCount  = 4;
    primitive.topology    = MeshTopology::Points;
    primitive.bounds      = Bounds(Vec3(0), Vec3(4, 0.4f, 0));
    scene.meshes[0].primitives.push_back(primitive);
    scene.nodes.resize(1);
    scene.nodes[0].mesh = MeshId{0};
    scene.roots         = {NodeId{0}};
    return scene;
}

bool TestFetch()
{
    Scene               scene     = MakeScene();
    MeshPrimitiveAsset& primitive = scene.meshes[0].primitives[0];
    primitive.influences.resize(5);
    for (uint32_t vertex = 0; vertex < 5; ++vertex)
    {
        for (uint32_t joint = 0; joint < 6; ++joint)
        {
            primitive.influences[vertex].push_back({joint, float(vertex + joint + 1)});
        }
    }
    MorphPrimitiveAsset morph;
    morph.targets.resize(1);
    morph.targets[0].positions = {Vec3(0), Vec3(1), Vec3(2), Vec3(3), Vec3(4)};
    scene.morphPrimitives.push_back(morph);
    scene.extraVertexAttributes.push_back(
        {0, 0, "_SOURCE", -1, 1, 5125, false, VertexAttributeDomain::SourceVertex, {100, 101, 102, 103, 104}});
    scene.extraVertexAttributes.push_back(
        {0, 0, "_CORNER", -1, 2, 5125, false, VertexAttributeDomain::PrimitiveVertex, {0, 10, 1, 11, 2, 12, 3, 13, 4, 14}});
    scene.materialVariants = {"untouched"};
    scene.variantPrimitives.push_back({0, 0, {-1}});
    const Scene      original = scene;
    GeometrySettings settings;
    settings.optimizeVertexFetch      = true;
    uint64_t                remaining = 1ull << 20;
    std::vector<Diagnostic> diagnostics;
    const Status            status = detail::ProcessGeometry(scene, settings, remaining, diagnostics, nullptr);
    bool                    valid  = Check(status.Succeeded(), status.message.c_str());
    const uint32_t          order[]{3, 1, 0, 2, 4};
    for (uint32_t vertex = 0; vertex < 5; ++vertex)
    {
        valid &= Check(scene_test::Same(scene.vertices[vertex], original.vertices[order[vertex]])
                           && scene_test::Same(scene.vertexTexCoords[vertex], original.vertexTexCoords[order[vertex]])
                           && scene_test::Same(scene.meshes[0].primitives[0].influences[vertex],
                                               original.meshes[0].primitives[0].influences[order[vertex]])
                           && scene.meshes[0].primitives[0].sourceVertices[vertex] == order[vertex],
                       "fetch permutation preserves complete vertex, UV, influence and source domains");
        valid &= Check(scene.extraVertexAttributes[1].values[vertex * 2] == double(order[vertex])
                           && scene.extraVertexAttributes[1].values[vertex * 2 + 1] == double(order[vertex] + 10),
                       "expanded corner attributes follow final vertex remapping");
    }
    valid &= Check(scene.indices == std::vector<uint32_t>({0, 1, 0, 2}), "fetch remapping preserves exact corner order");
    valid &= Check(scene_test::Same(scene.morphPrimitives, original.morphPrimitives)
                       && scene_test::Same(scene.extraVertexAttributes[0], original.extraVertexAttributes[0])
                       && scene_test::Same(scene.variantPrimitives, original.variantPrimitives),
                   "authored morph/custom values and material variants remain unchanged");
    valid &= Check(diagnostics.empty() && remaining == (1ull << 20) - 5 * sizeof(uint32_t),
                   "scratch is released while persistent source map storage remains charged");
    const Scene once  = scene;
    valid            &= Check(detail::ProcessGeometry(scene, settings, remaining, diagnostics, nullptr).Succeeded()
                                  && scene_test::Same(scene, once),
                              "fetch optimization is stable on already optimized storage");
    return valid;
}

bool TestSharedAndOverlapping()
{
    bool               valid  = true;
    Scene              scene  = MakeScene();
    MeshPrimitiveAsset shared = scene.meshes[0].primitives[0];
    shared.firstIndex         = 1;
    shared.indexCount         = 2;
    shared.topology           = MeshTopology::Lines;
    shared.sourceVertices     = {4, 3, 2, 1, 0};
    scene.meshes[0].primitives.push_back(shared);
    GeometrySettings settings;
    settings.optimizeVertexFetch      = true;
    uint64_t                remaining = 1ull << 20;
    std::vector<Diagnostic> diagnostics;
    Status                  status = detail::ProcessGeometry(scene, settings, remaining, diagnostics, nullptr);
    valid         &= Check(status.Succeeded() && scene.indices == std::vector<uint32_t>({0, 1, 0, 2})
                               && scene.meshes[0].primitives[1].sourceVertices == std::vector<uint32_t>({1, 3, 4, 2, 0})
                               && scene.meshes[0].primitives[1].topology == MeshTopology::Lines,
                           "identical shared ranges use one permutation and overlapping index ranges are rewritten once");
    scene          = MakeScene();
    scene.indices  = {3, 1, 2, 4};
    scene.meshes[0].primitives[0].vertexCount = 4;
    scene.meshes[0].primitives[0].indexCount  = 2;
    shared                                    = scene.meshes[0].primitives[0];
    shared.firstVertex                        = 2;
    shared.vertexCount                        = 3;
    shared.firstIndex                         = 2;
    scene.meshes[0].primitives.push_back(shared);
    const Scene original = scene;
    diagnostics.clear();
    status  = detail::ProcessGeometry(scene, settings, remaining, diagnostics, nullptr);
    valid  &= Check(status.Succeeded() && scene_test::Same(scene, original) && diagnostics.size() == 2
                        && diagnostics[0].object != diagnostics[1].object,
                    "partial overlaps are retained with identifiable diagnostics");
    return valid;
}

bool TestQuantization()
{
    bool  valid                     = true;
    Scene scene                     = MakeScene();
    scene.vertices[1].pos.x         = 0.37f;
    scene.nodes[0].baseMatrix[1][0] = 0.6f;
    scene.nodes[0].scale            = Vec3(2, 3, 1);
    scene.nodes[0].rotation         = Quat(0.9238795f, 0, 0, 0.3826834f);
    GeometrySettings settings;
    settings.positionQuantizationBits = 8;
    settings.positionMaxRelativeError = 0;
    settings.positionMaxAbsoluteError = 0.1f;
    uint64_t                remaining = 1ull << 20;
    std::vector<Diagnostic> diagnostics;
    const Scene             original = scene;
    Status                  status   = detail::ProcessGeometry(scene, settings, remaining, diagnostics, nullptr);
    valid &= Check(status.Succeeded() && diagnostics.empty() && scene.vertices[1].pos.x != original.vertices[1].pos.x,
                   "static affine instances accept quantization within the declared world-space bound");
    for (size_t index = 0; index < scene.vertices.size(); ++index)
    {
        valid &= Check(scene.vertices[index].normal == original.vertices[index].normal
                           && scene.vertices[index].pos.w == original.vertices[index].pos.w,
                       "position quantization preserves normals and homogeneous position bits");
    }
    scene = original;
    scene.nodes.push_back(scene.nodes[0]);
    scene.nodes[1].scale = Vec3(10000);
    scene.roots.push_back(NodeId{1});
    const Scene scaled  = scene;
    status              = detail::ProcessGeometry(scene, settings, remaining, diagnostics, nullptr);
    valid              &= Check(status.Succeeded() && scene_test::Same(scene, scaled) && !diagnostics.empty(),
                                "a single high-scale instance rejects quantization for all owners");
    scene               = original;
    scene.animations.resize(1);
    scene.animations[0].channels.push_back({0, 0, AnimationPath::Translation});
    const Scene animated = scene;
    diagnostics.clear();
    status               = detail::ProcessGeometry(scene, settings, remaining, diagnostics, nullptr);
    valid               &= Check(status.Succeeded() && scene_test::Same(scene, animated) && diagnostics.size() == 1,
                                 "animated transforms retain original positions with a diagnostic");
    scene                = original;
    scene.nodes[0].skin  = SkinId{0};
    const Scene skinned  = scene;
    diagnostics.clear();
    status  = detail::ProcessGeometry(scene, settings, remaining, diagnostics, nullptr);
    valid  &= Check(status.Succeeded() && scene_test::Same(scene, skinned) && diagnostics.size() == 1,
                    "skinned instances retain original positions");
    scene   = original;
    scene.morphPrimitives.resize(1);
    const Scene morphed = scene;
    diagnostics.clear();
    status  = detail::ProcessGeometry(scene, settings, remaining, diagnostics, nullptr);
    valid  &= Check(status.Succeeded() && scene_test::Same(scene, morphed) && diagnostics.size() == 1,
                    "morph payload owners retain original positions");
    scene   = original;
    scene.nodes.resize(2);
    scene.nodes[0].parent   = NodeId{1};
    scene.nodes[1].children = {NodeId{0}};
    scene.nodes[1].scale    = Vec3(10000);
    scene.roots             = {NodeId{1}};
    const Scene parentScale = scene;
    diagnostics.clear();
    status                = detail::ProcessGeometry(scene, settings, remaining, diagnostics, nullptr);
    valid                &= Check(status.Succeeded() && scene_test::Same(scene, parentScale) && diagnostics.size() == 1,
                                  "ancestor transforms participate in world error checks");
    scene.nodes[1].scale  = Vec3(1);
    scene.animations.resize(1);
    scene.animations[0].channels.push_back(
        {0, UINT32_MAX, AnimationPath::Property, {AnimatedObject::Node, 1, AnimatedProperty::Rotation, 4}});
    const Scene parentAnimated = scene;
    diagnostics.clear();
    status                   = detail::ProcessGeometry(scene, settings, remaining, diagnostics, nullptr);
    valid                   &= Check(status.Succeeded() && scene_test::Same(scene, parentAnimated) && diagnostics.size() == 1,
                                     "typed animation on an ancestor prevents static quantization");
    scene                    = MakeScene();
    scene.vertices[1].pos.x  = 0.37f;
    const Scene maxBound     = scene;
    settings.positionMaxAbsoluteError = 0.004f;
    settings.positionMaxRelativeError = 0.001f;
    diagnostics.clear();
    status  = detail::ProcessGeometry(scene, settings, remaining, diagnostics, nullptr);
    valid  &= Check(status.Succeeded() && scene_test::Same(scene, maxBound) && diagnostics.size() == 1,
                    "error tolerance uses maximum of absolute and relative bounds instead of their sum");
    return valid;
}

bool TestLimits()
{
    bool             valid = true;
    Scene            scene = MakeScene();
    GeometrySettings settings;
    settings.optimizeVertexFetch      = true;
    uint64_t                remaining = 1;
    std::vector<Diagnostic> diagnostics;
    valid &= Check(detail::ProcessGeometry(scene, settings, remaining, diagnostics, nullptr).code == StatusCode::ResourceLimit,
                   "geometry scratch rejects inadequate allocation budget");
    Cancellation cancelled;
    cancelled.Cancel();
    remaining = 1ull << 20;
    valid &= Check(detail::ProcessGeometry(scene, settings, remaining, diagnostics, &cancelled).code == StatusCode::Cancelled,
                   "geometry processing honors cancellation");
    settings.positionQuantizationBits = 25;
    valid &=
        Check(detail::ProcessGeometry(scene, settings, remaining, diagnostics, nullptr).code == StatusCode::InvalidArgument,
              "invalid position quantization precision rejected");
    settings.positionQuantizationBits = 0;
    scene.vertices.resize(20000);
    scene.vertexTexCoords.clear();
    scene.indices.resize(20000);
    scene.meshes[0].primitives[0].vertexCount = 20000;
    scene.meshes[0].primitives[0].indexCount  = 20000;
    for (uint32_t index = 0; index < 20000; ++index)
    {
        scene.indices[index] = 19999 - index;
    }
    uint32_t     polls = 0;
    Cancellation duringProcessing(CancelDuringProcessing, &polls);
    remaining = 1ull << 20;
    valid &=
        Check(detail::ProcessGeometry(scene, settings, remaining, diagnostics, &duringProcessing).code == StatusCode::Cancelled
                  && polls >= 5 && remaining == (1ull << 20),
              "mid-pass cancellation releases all temporary geometry storage");
    return valid;
}
} // namespace

int main()
{
    bool valid  = TestFetch();
    valid      &= TestSharedAndOverlapping();
    valid      &= TestQuantization();
    valid      &= TestLimits();
    std::printf("Geometry processing: %u checks, %s\n", checks, valid ? "passed" : "failed");
    return valid ? 0 : 1;
}
