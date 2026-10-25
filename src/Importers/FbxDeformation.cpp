#include "Importers/FbxDeformation.h"
#include "Format/MathOps.h"
#include <cmath>

namespace zenasset
{
namespace
{
void ConvertInfluences(const ufbx_mesh& source, MeshAsset& mesh, std::vector<Vertex>& vertices, ImportErrors& errors)
{
    if (source.skin_deformers.count == 1)
    {
        const ufbx_skin_deformer& skin = *source.skin_deformers.data[0];
        for (MeshPrimitiveAsset& primitive : mesh.primitives)
        {
            ResizeGeometryVector(primitive.influences, primitive.vertexCount, errors);
            for (uint32_t vertex = 0; !errors.Failed() && vertex < primitive.vertexCount; ++vertex)
            {
                const uint32_t point = primitive.sourceVertices[vertex];
                if (point >= skin.vertices.count)
                {
                    errors.Fail("FBX skin has no influence record for a mesh vertex: " + mesh.name);
                }
                else
                {
                    const ufbx_skin_vertex&            binding    = skin.vertices.data[point];
                    std::vector<VertexJointInfluence>& influences = primitive.influences[vertex];
                    double                             total      = 0;
                    for (uint32_t index = 0; index < binding.num_weights; ++index)
                    {
                        const ufbx_skin_weight& weight = skin.weights.data[binding.weight_begin + index];
                        if (weight.cluster_index >= skin.clusters.count || !std::isfinite(weight.weight) || weight.weight < 0)
                        {
                            errors.Fail("Invalid FBX skin influence: " + mesh.name);
                        }
                        else if (weight.weight > 0)
                        {
                            total += weight.weight;
                        }
                    }
                    if (!std::isfinite(total))
                    {
                        errors.Fail("FBX skin influence sum is not finite: " + mesh.name);
                    }
                    else if (total <= 0)
                    {
                        // ufbx uses the first mesh instance's geometry transform for
                        // unweighted vertices. BuildFbxSkins adds that fallback joint.
                        if (ReserveGeometryVector(influences, 1, errors))
                        {
                            influences.push_back({static_cast<uint32_t>(skin.clusters.count), 1});
                        }
                    }
                    else if (ReserveGeometryVector(influences, binding.num_weights, errors))
                    {
                        for (uint32_t index = 0; index < binding.num_weights; ++index)
                        {
                            const ufbx_skin_weight& weight = skin.weights.data[binding.weight_begin + index];
                            if (weight.weight > 0)
                            {
                                influences.push_back({weight.cluster_index, static_cast<float>(weight.weight / total)});
                            }
                        }
                    }
                    if (!errors.Failed())
                    {
                        Vertex& output = vertices[primitive.firstVertex + vertex];
                        for (uint32_t index = 0; index < influences.size(); ++index)
                        {
                            if (index < 4)
                            {
                                output.joint0[index]  = static_cast<float>(influences[index].joint);
                                output.weight0[index] = influences[index].weight;
                            }
                        }
                    }
                }
            }
        }
    }
}

MorphTargetAsset ConvertShape(const ufbx_mesh& mesh, const ufbx_blend_shape& shape, ImportErrors& errors)
{
    MorphTargetAsset target;
    ResizeGeometryVector(target.positions, mesh.num_vertices, errors);
    std::fill(target.positions.begin(), target.positions.end(), Vec3(0));
    if (shape.normal_offsets.count > 0)
    {
        ResizeGeometryVector(target.normals, mesh.num_vertices, errors);
        std::fill(target.normals.begin(), target.normals.end(), Vec3(0));
    }
    for (size_t index = 0; !errors.Failed() && index < shape.num_offsets; ++index)
    {
        const uint32_t vertex = shape.offset_vertices.data[index];
        if (vertex >= mesh.num_vertices)
        {
            errors.Fail("FBX blend shape references an invalid control point: " + FbxText(shape.name));
        }
        else
        {
            const float weight = index < shape.offset_weights.count ? static_cast<float>(shape.offset_weights.data[index]) : 1;
            const Vec3  offset = FbxVector(shape.position_offsets.data[index]) * weight;
            if (!std::isfinite(offset.x) || !std::isfinite(offset.y) || !std::isfinite(offset.z))
            {
                errors.Fail("Non-finite FBX blend shape offset: " + FbxText(shape.name));
            }
            target.positions[vertex] += offset;
            if (index < shape.normal_offsets.count)
            {
                const Vec3 normal = FbxVector(shape.normal_offsets.data[index]) * weight;
                if (!std::isfinite(normal.x) || !std::isfinite(normal.y) || !std::isfinite(normal.z))
                {
                    errors.Fail("Non-finite FBX blend shape normal: " + FbxText(shape.name));
                }
                target.normals[vertex] += normal;
            }
        }
    }
    return target;
}
} // namespace

void ValidateFbxDeformers(const ufbx_scene& source, ImportErrors& errors)
{
    for (const ufbx_mesh* mesh : source.meshes)
    {
        // The supported serialized stack is morphs followed by one skin. Do
        // not silently reorder an explicitly different deformer connection list.
        bool skinSeen = false;
        for (const ufbx_element* deformer : mesh->all_deformers)
        {
            if (deformer->type == UFBX_ELEMENT_SKIN_DEFORMER)
            {
                skinSeen = true;
            }
            else if (skinSeen && deformer->type == UFBX_ELEMENT_BLEND_DEFORMER)
            {
                errors.Fail("Unsupported FBX blend deformer after skin in the connection order: " + FbxText(mesh->name),
                            SceneImportStatus::UnsupportedFeature);
            }
        }
        if (mesh->cache_deformers.count > 0 || mesh->skin_deformers.count > 1)
        {
            errors.Fail("Unsupported FBX geometry cache or multiple skin deformers: " + FbxText(mesh->name),
                        SceneImportStatus::UnsupportedFeature);
        }
        for (const ufbx_skin_deformer* skin : mesh->skin_deformers)
        {
            if (skin->skinning_method != UFBX_SKINNING_METHOD_LINEAR && skin->skinning_method != UFBX_SKINNING_METHOD_RIGID)
            {
                errors.Fail("Unsupported FBX dual-quaternion skinning: " + FbxText(skin->name),
                            SceneImportStatus::UnsupportedFeature);
            }
            for (const ufbx_skin_cluster* cluster : skin->clusters)
            {
                if (cluster->bone_node == nullptr)
                {
                    errors.Fail("FBX skin cluster has no bone: " + FbxText(cluster->name));
                }
                const ufbx_dom_node* mode =
                    cluster->element.dom_node != nullptr ? ufbx_dom_find(cluster->element.dom_node, "Mode") : nullptr;
                if (mode != nullptr && mode->values.count > 0 && FbxText(mode->values.data[0].value_str) != "Normalize")
                {
                    errors.Fail("Unsupported FBX skin cluster link mode: " + FbxText(mode->values.data[0].value_str),
                                SceneImportStatus::UnsupportedFeature);
                }
            }
        }
    }
}

void ConvertFbxMeshDeformation(const ufbx_mesh&     source,
                               MeshAsset&           mesh,
                               std::vector<Vertex>& vertices,
                               MorphPrimitiveAsset& morph,
                               ImportErrors&        errors)
{
    if (!errors.Failed())
    {
        ConvertInfluences(source, mesh, vertices, errors);
        morph.mesh      = source.typed_id;
        morph.primitive = UINT32_MAX; // One control-point payload shared by all material parts.
        for (const ufbx_blend_deformer* deformer : source.blend_deformers)
        {
            for (const ufbx_blend_channel* channel : deformer->channels)
            {
                for (const ufbx_blend_keyframe& key : channel->keyframes)
                {
                    if (key.shape == nullptr || !std::isfinite(key.target_weight) || !std::isfinite(key.effective_weight))
                    {
                        errors.Fail("Invalid FBX blend channel: " + FbxText(channel->name));
                    }
                    else if (ReserveGeometryVector(morph.targets, morph.targets.size() + 1, errors)
                             && ReserveGeometryVector(morph.weights, morph.weights.size() + 1, errors))
                    {
                        MorphTargetAsset target = ConvertShape(source, *key.shape, errors);
                        if (!errors.Failed())
                        {
                            morph.targets.push_back(std::move(target));
                            morph.weights.push_back(static_cast<float>(key.effective_weight));
                        }
                    }
                }
            }
        }
        if (ReserveGeometryVector(mesh.morphWeights, morph.weights.size(), errors))
        {
            mesh.morphWeights = morph.weights;
        }
    }
}

void BuildFbxSkins(const ufbx_scene& source, Scene& asset, ImportErrors& errors)
{
    ReserveGeometryVector(asset.skins, source.meshes.count, errors);
    for (const ufbx_mesh* mesh : source.meshes)
    {
        if (!errors.Failed() && mesh->skin_deformers.count == 1)
        {
            const ufbx_skin_deformer& sourceSkin = *mesh->skin_deformers.data[0];
            SkinAsset                 skin;
            skin.name               = FbxText(sourceSkin.name);
            const size_t jointCount = sourceSkin.clusters.count + (mesh->instances.count > 0 ? 1 : 0);
            ReserveGeometryVector(skin.joints, jointCount, errors);
            ReserveGeometryVector(skin.inverseBindMatrices, jointCount, errors);
            if (!errors.Failed())
            {
                for (const ufbx_skin_cluster* cluster : sourceSkin.clusters)
                {
                    skin.joints.push_back(cluster->bone_node->typed_id);
                    skin.inverseBindMatrices.push_back(FbxMatrix(cluster->geometry_to_bone));
                }
                if (mesh->instances.count > 0)
                {
                    skin.joints.push_back(mesh->instances.data[0]->typed_id);
                    skin.inverseBindMatrices.push_back(FbxMatrix(mesh->instances.data[0]->geometry_to_node));
                }
                for (const Mat4& matrix : skin.inverseBindMatrices)
                {
                    if (!IsFinite(matrix))
                    {
                        errors.Fail("Non-finite FBX bind matrix: " + skin.name);
                    }
                }
                const SkinId id{static_cast<uint32_t>(asset.skins.size())};
                asset.skins.push_back(std::move(skin));
                for (const ufbx_node* instance : mesh->instances)
                {
                    asset.nodes[instance->typed_id].skin = id;
                }
            }
        }
    }
}
} // namespace zenasset
