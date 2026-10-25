#include "Processing/SceneAssetProcessing.h"
#include "Diagnostics/StageMeasurements.h"
#include <algorithm>

namespace zenasset
{
namespace
{
// Target count of each mesh, the largest over its morph primitives.
std::vector<uint32_t> MorphTargetCounts(const Scene& asset)
{
    std::vector<uint32_t> counts(asset.meshes.size(), 0u);
    for (const MorphPrimitiveAsset& morph : asset.morphPrimitives)
    {
        if (morph.mesh < counts.size())
        {
            counts[morph.mesh] = std::max(counts[morph.mesh], static_cast<uint32_t>(morph.targets.size()));
        }
    }
    return counts;
}

void Warn(SceneImportResult& result, const std::string& source, const std::string& object, std::string message)
{
    result.diagnostics.push_back(
        {SceneDiagnosticSeverity::Warning, SceneImportStatus::InvalidData, source, object, std::move(message)});
}
} // namespace

bool ProcessImportedAsset(Scene& asset, SceneImportResult& result, const std::string& source, GeometryBudget* geometry)
{
    ZENASSET_MEASURE_SCOPE(ImportedNormalize);
    GeometryReservation scratch(geometry);
    bool                valid = scratch.Reserve(asset.meshes.size(), sizeof(uint32_t))
                             && scratch.Reserve(asset.animations.size(), sizeof(AnimationAsset));
    if (!valid)
    {
        SetSceneImportFailure(result, SceneImportStatus::ResourceLimit, source,
                              "Geometry normalization scratch exceeds maxGeometryBytes");
    }
    else
    {
        const std::vector<uint32_t> targets = MorphTargetCounts(asset);
        for (uint32_t meshIndex = 0; meshIndex < asset.meshes.size(); ++meshIndex)
        {
            MeshAsset& mesh = asset.meshes[meshIndex];
            if (!mesh.morphWeights.empty() && mesh.morphWeights.size() != targets[meshIndex])
            {
                Warn(result, source, mesh.name, "Mesh morph weights do not match its targets and were ignored");
                mesh.morphWeights.clear();
            }
        }
        for (NodeAsset& node : asset.nodes)
        {
            if (!node.morphWeights.empty()
                && (!node.mesh.IsValid() || node.mesh.value >= targets.size()
                    || node.morphWeights.size() != targets[node.mesh.value]))
            {
                Warn(result, source, node.name, "Node morph weights do not match its mesh targets; the mesh defaults apply");
                node.morphWeights.clear();
            }
        }
        std::vector<AnimationAsset> playable;
        playable.reserve(asset.animations.size());
        for (AnimationAsset& animation : asset.animations)
        {
            std::string reason;
            if (ValidateAnimationAsset(asset, animation, reason))
            {
                playable.push_back(std::move(animation));
            }
            else
            {
                Warn(result, source, animation.name, "Animation was skipped: " + reason);
            }
        }
        asset.animations = std::move(playable);
        std::string reason;
        valid = ValidateSceneAsset(asset, 16ull * 1024 * 1024 * 1024, &reason);
        if (!valid)
        {
            SetSceneImportFailure(result, SceneImportStatus::InvalidData, source, "Imported scene is invalid: " + reason);
        }
    }
    return valid;
}
} // namespace zenasset
