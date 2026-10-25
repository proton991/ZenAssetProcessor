#include "Importers/FbxSceneImporter.h"
#include "ZenAsset/Format/Validation.h"
#include "SceneValueComparison.h"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace
{
using namespace zenasset;
constexpr uint64_t Ample = 64ull * 1024 * 1024;

bool Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
    }
    return condition;
}

SceneImportResult Read(const std::filesystem::path&        path,
                       GeometryBudget&                     budget,
                       WorkerPool*                         pool    = nullptr,
                       std::shared_ptr<SceneImportControl> control = {})
{
    SceneImportRequest request;
    request.path    = path.string();
    request.control = std::move(control);
    BackendContext context;
    context.processing = pool;
    context.geometry   = &budget;
    return FbxSceneImporter().Import(request, context);
}

bool IsBudgetFailure(const SceneImportResult& result)
{
    return result.status == SceneImportStatus::ResourceLimit && !result.asset
        && result.error.find("maxGeometryBytes") != std::string::npos;
}

bool Boundary(const std::filesystem::path& path)
{
    GeometryBudget          budget(Ample);
    const SceneImportResult baseline = Read(path, budget);
    bool valid  = Check(baseline.status == SceneImportStatus::Success && baseline.asset, baseline.error.c_str());
    valid      &= Check(budget.Used() == 0 && budget.Peak() > 0, "FBX import must refund its complete declared reservation");
    if (valid)
    {
        GeometryBudget          exact(budget.Peak()), shortBudget(budget.Peak() - 1), empty(0);
        const SceneImportResult accepted = Read(path, exact);
        const SceneImportResult rejected = Read(path, shortBudget);
        const SceneImportResult zero     = Read(path, empty);
        valid &=
            Check(accepted.status == SceneImportStatus::Success && accepted.asset, "Exact serial declared peak must succeed");
        valid &= Check(IsBudgetFailure(rejected) && IsBudgetFailure(zero),
                       "One-byte-short and zero budgets must reject without publishing an asset");
        valid &= Check(exact.Used() == 0 && shortBudget.Used() == 0 && empty.Used() == 0,
                       "Success and every rejection must refund reservations");
        valid &= Check(exact.Peak() == budget.Peak() && shortBudget.Peak() <= budget.Peak() - 1,
                       "Reservation peak must be deterministic and never exceed allowance");
        if (accepted.asset)
        {
            valid &= Check(scene_test::Same(*baseline.asset, *accepted.asset) && ValidateSceneAsset(*accepted.asset),
                           "Tight allowance must preserve all geometry/deformation/animation values");
        }
        std::cout << path.filename().string() << " declared_peak=" << budget.Peak() << '\n';
    }
    return valid;
}

bool WriteMeshes(const std::filesystem::path& path, uint32_t count)
{
    std::ofstream file(path, std::ios::binary);
    file << "FBXHeaderExtension: { FBXHeaderVersion: 1003\n FBXVersion: 7400\n}\nObjects: {\n";
    for (uint32_t i = 0; i < count; ++i)
    {
        file << "Geometry: " << 1 + i * 2 << ", \"Geometry::Triangle" << i << "\", \"Mesh\" {\n"
             << "Vertices: *9 { a: 0,0,0,1,0,0,0,1,0 }\nPolygonVertexIndex: *3 { a: 0,1,-3 }\n}\n"
             << "Model: " << 2 + i * 2 << ", \"Model::Triangle" << i << "\", \"Mesh\" { Version: 232 }\n";
    }
    file << "}\nConnections: {\n";
    for (uint32_t i = 0; i < count; ++i)
    {
        file << "C: \"OO\"," << 1 + i * 2 << ',' << 2 + i * 2 << "\nC: \"OO\"," << 2 + i * 2 << ",0\n";
    }
    file << "}\n";
    return file.good();
}

struct CancelAfterCharge
{
    GeometryBudget*               budget{nullptr};
    SceneImportStage              target{SceneImportStage::ProcessingMeshes};
    std::atomic<SceneImportStage> stage{SceneImportStage::Queued};
    std::atomic<uint32_t>         polls{0};
    std::thread::id               caller{std::this_thread::get_id()};
    bool                          workerOnly{false};
    std::atomic<bool>             cancelled{false};
};

void ObserveStage(void* user, SceneImportStage stage)
{
    static_cast<CancelAfterCharge*>(user)->stage.store(stage);
}

bool CancelCharged(void* user)
{
    CancelAfterCharge& state          = *static_cast<CancelAfterCharge*>(user);
    const bool         eligibleThread = !state.workerOnly || std::this_thread::get_id() != state.caller;
    if (eligibleThread && state.stage.load() == state.target && state.budget->Used() > 0 && state.polls.fetch_add(1) >= 32)
    {
        state.cancelled.store(true);
    }
    return state.cancelled.load();
}

bool Ping()
{
    return true;
}

bool CheckCancellation(const std::filesystem::path& path, SceneImportStage stage)
{
    GeometryBudget    budget(Ample);
    WorkerPool        pool(4);
    CancelAfterCharge state;
    state.budget     = &budget;
    state.target     = stage;
    state.workerOnly = stage == SceneImportStage::ProcessingMeshes;
    SceneImportCallbacks callbacks;
    callbacks.user                  = &state;
    callbacks.cancelled             = CancelCharged;
    callbacks.stageChanged          = ObserveStage;
    const SceneImportResult result  = Read(path, budget, &pool, std::make_shared<SceneImportControl>(callbacks));
    bool                    valid   = Check(state.cancelled && result.status == SceneImportStatus::Cancelled && !result.asset,
                                            "Cancellation after charged work must publish no partial FBX asset");
    valid                          &= Check(budget.Peak() > 0 && budget.Used() == 0 && pool.Push(Ping).get(),
                                            "Cancelled workers must drain/refund before returning and leave the pool usable");
    return valid;
}

bool ParallelAndCumulative(const std::filesystem::path& directory)
{
    const std::filesystem::path single = directory / "one.fbx", many = directory / "many.fbx";
    bool valid = Check(WriteMeshes(single, 1) && WriteMeshes(many, 64), "Could not write owned FBX test fixtures");
    if (valid)
    {
        GeometryBudget          oneBudget(Ample), manyBudget(Ample);
        const SceneImportResult one = Read(single, oneBudget);
        WorkerPool              pool(4);
        const SceneImportResult manyResult  = Read(many, manyBudget, &pool);
        valid                              &= Check(one.asset && manyResult.asset && manyResult.asset->meshes.size() == 64
                                                        && manyResult.asset->vertices.size() == 64 * 3,
                                                    "Parallel fixture must preserve every emitted mesh and vertex");
        GeometryBudget          bounded(oneBudget.Peak());
        const SceneImportResult rejected = Read(many, bounded, &pool);
        valid &= Check(IsBudgetFailure(rejected) && bounded.Used() == 0 && pool.Push(Ping).get(),
                       "Cumulative parallel mesh growth must reject and drain, even when one mesh fits");
        valid &= Check(manyBudget.Peak() > oneBudget.Peak() && manyBudget.Used() == 0,
                       "All simultaneous mesh results must share one refunded allowance");
        valid  = CheckCancellation(many, SceneImportStage::ProcessingMeshes) && valid;
    }
    return valid;
}

bool PublicLimit(const std::filesystem::path& path)
{
    SceneImportRequest request;
    request.path = path.string();
    SceneImportContext context;
    context.workerCount              = 2;
    context.maxGeometryBytes         = 1;
    const SceneImportResult limited  = ImportScene(request, context);
    context.maxGeometryBytes         = Ample;
    const SceneImportResult accepted = ImportScene(request, context);
    bool valid = Check(IsBudgetFailure(limited) && accepted.status == SceneImportStatus::Success && accepted.asset,
                       "Public FBX import must forward maxGeometryBytes and recover for the next request");
    if (accepted.asset)
    {
        const Scene& scene      = *accepted.asset;
        size_t       influences = 0;
        for (const MeshAsset& mesh : scene.meshes)
        {
            for (const MeshPrimitiveAsset& primitive : mesh.primitives)
            {
                for (const std::vector<VertexJointInfluence>& vertex : primitive.influences)
                {
                    influences = std::max(influences, vertex.size());
                }
            }
        }
        valid &= Check(influences == 6 && !scene.skins.empty() && !scene.morphPrimitives.empty() && !scene.animations.empty()
                           && !scene.animations[0].samplers.empty(),
                       "Combined fixture must retain all six influences, skins, morphs and baked animation");
    }
    return valid;
}
} // namespace

int main(int argc, char** argv)
{
    bool valid = Check(argc == 2, "Expected processor-owned FBX fixture directory");
    if (valid)
    {
        const std::filesystem::path fixtures(argv[1]);
        const char*                 names[] = {"maya_cube_7500_ascii.fbx", "maya_uv_and_color_sets_7500_ascii.fbx",
                                               "combined_morph_skin_ascii.fbx", "maya_anim_interpolation_7700_ascii.fbx"};
        for (const char* name : names)
        {
            valid = Boundary(fixtures / name) && valid;
        }
        valid = PublicLimit(fixtures / "combined_morph_skin_ascii.fbx") && valid;
        valid = CheckCancellation(fixtures / "combined_morph_skin_ascii.fbx", SceneImportStage::BakingAnimations) && valid;
        const std::filesystem::path temporary = std::filesystem::temp_directory_path() / "zenasset-fbx-geometry-budget";
        std::error_code             error;
        std::filesystem::create_directories(temporary, error);
        valid &= Check(!error, "Could not create FBX budget fixture directory");
        if (!error)
        {
            valid = ParallelAndCumulative(temporary) && valid;
            std::filesystem::remove(temporary / "one.fbx", error);
            std::filesystem::remove(temporary / "many.fbx", error);
            std::filesystem::remove(temporary, error);
        }
    }
    if (valid)
    {
        std::cout << "FBX geometry/deformation/animation budget boundary and cancellation checks passed\n";
    }
    return valid ? 0 : 1;
}
