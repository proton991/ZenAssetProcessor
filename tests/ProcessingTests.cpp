#include "ZenAsset/Processor/Import.h"
#include "ZenAsset/Format/Validation.h"
#include "Processing/SceneAssetProcessing.h"
#include "Processing/SurfaceGeometry.h"
#include <bit>
#include <cstdio>

namespace
{
bool Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "%s\n", message);
    }
    return condition;
}

zenasset::Scene MinimalMorphScene()
{
    zenasset::Scene scene;
    scene.nodes.resize(1);
    scene.nodes[0].mesh = zenasset::MeshId{0};
    scene.roots         = {zenasset::NodeId{0}};
    scene.meshes.resize(1);
    scene.meshes[0].primitives.resize(1);
    scene.morphPrimitives.resize(1);
    scene.morphPrimitives[0].targets.resize(1);
    scene.animations.resize(1);
    scene.animations[0].samplers.push_back({zenasset::AnimationInterpolation::Linear, 1, {0, 1}, {0, 1}});
    scene.animations[0].channels.push_back({0, 0, zenasset::AnimationPath::Weights, {}, "/nodes/0/weights"});
    return scene;
}

bool CheckProcessing(const zenasset::Scene& source)
{
    bool valid = Check(!source.animations.empty() && !source.animations[0].samplers.empty()
                           && source.animations[0].samplers[0].times.size() > 1,
                       "Fixture must contain a multi-key clip");
    if (valid)
    {
        zenasset::Scene processed                    = source;
        const size_t    clips                        = processed.animations.size();
        processed.animations[0].name                 = "Broken";
        processed.animations[0].samplers[0].times[1] = processed.animations[0].samplers[0].times[0];
        zenasset::NodeAsset* morphed                 = nullptr;
        for (zenasset::NodeAsset& node : processed.nodes)
        {
            if (node.mesh.IsValid() && morphed == nullptr)
            {
                morphed = &node;
            }
        }
        valid &= Check(morphed != nullptr, "Fixture must contain a mesh node");
        if (valid)
        {
            morphed->morphWeights.assign(17, 0.25f);
            zenasset::SceneImportResult report;
            valid        &= Check(zenasset::ProcessImportedAsset(processed, report, "scene.fbx"), report.error.c_str());
            valid        &= Check(processed.animations.size() == clips - 1, "Invalid clips must be dropped individually");
            valid        &= Check(morphed->morphWeights.empty(), "Mismatched default weights must be cleared");
            bool skipped  = false;
            for (const zenasset::SceneImportDiagnostic& diagnostic : report.diagnostics)
            {
                skipped |= diagnostic.object == "Broken" && diagnostic.message.find("strictly increasing") != std::string::npos;
            }
            valid &= Check(skipped, "Dropped clips must retain a named warning");
            valid &= Check(zenasset::ValidateSceneAsset(processed), "Processed scene must be semantically valid");
        }
        zenasset::Scene invalid   = source;
        invalid.nodes[0].rotation = zenasset::Quat(0, 0, 0, 0);
        zenasset::SceneImportResult failed;
        valid &= Check(!zenasset::ProcessImportedAsset(invalid, failed, "scene.fbx"), "Invalid transforms must fail");
        valid &= Check(failed.error.find("node transforms") != std::string::npos,
                       "Processing errors must identify invalid transforms");
    }
    return valid;
}

bool CheckReferenceNormal()
{
    // DamagedHelmet vertex 1 and its frozen v2 canonical normal. Multiplying by
    // a rounded reciprocal instead of dividing changes every component here.
    zenasset::Vec3 normal(std::bit_cast<float>(0xbf6b2dd6u), std::bit_cast<float>(0x3ec77d8fu),
                          std::bit_cast<float>(0xbd845109u));
    bool           valid = Check(zenasset::NormalizeSurfaceNormal(normal), "Reference normal must be valid");
    valid &= Check(std::bit_cast<uint32_t>(normal.x) == 0xbf6b2fbeu && std::bit_cast<uint32_t>(normal.y) == 0x3ec77f2du
                       && std::bit_cast<uint32_t>(normal.z) == 0xbd84521bu,
                   "Surface normalization must preserve the frozen canonical float values");
    return valid;
}
} // namespace

int main(int argc, char** argv)
{
    bool valid = false;
    if (argc == 1)
    {
        valid = CheckProcessing(MinimalMorphScene());
    }
    else if (argc == 2)
    {
        zenasset::SceneImportContext context;
        context.workerCount                        = 2;
        const zenasset::SceneImportResult imported = zenasset::ImportScene({argv[1]}, context);
        valid                                      = Check(bool(imported.asset), imported.error.c_str());
        if (valid)
        {
            valid = CheckProcessing(*imported.asset);
        }
    }
    valid &= CheckReferenceNormal();
    return valid ? 0 : 1;
}
