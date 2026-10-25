#pragma once
#include "ZenAsset/Processor/Import.h"
#include "ZenAsset/Processor/Write.h"
#include <span>

namespace zenasset
{
struct SceneBuildJob
{
    std::string source;
    // Relative .zenasset path below SceneBuildOptions::outputDirectory.
    std::string         output;
    std::string         profile{"reference"};
    SourceSettings      sourceSettings;
    ProcessingSettings  processing;
    CompressionSettings compression;
};

enum class SceneBuildStage : uint32_t
{
    Waiting,
    Preflight,
    Importing,
    Cooking,
    Writing,
    Publishing,
    Reused,
    Complete
};
struct SceneBuildCallbacks
{
    void* user{nullptr};
    // May run concurrently on workers. Waiting uses job == UINT32_MAX, before
    // admission. Other events use request-order indices. Do not invoke a batch here.
    void (*progress)(void*, uint32_t job, SceneBuildStage){nullptr};
};
struct SceneBuildOptions
{
    std::string         outputDirectory;
    uint32_t            workerCount{1};
    uint32_t            maxJobs{1024};
    uint64_t            maxManifestBytes{1ull << 20};
    uint64_t            maxPlanningBytes{64ull << 20};
    uint64_t            maxJobMetadataBytes{4ull << 20};
    uint32_t            maxDiagnosticsPerJob{128};
    uint64_t            maxReservedMemoryBytes{4ull << 30};
    SceneImportContext  importContext{1, 128ull << 20, 256ull << 20, 256ull << 20, 256ull << 20};
    SceneReadLimits     limits;
    uint64_t            maxEncoderBytes{64ull << 20};
    bool                force{false};
    bool                durable{true};
    SceneBuildCallbacks callbacks;

    SceneBuildOptions()
    {
        limits.maxAllocationBytes      = 256ull << 20;
        limits.maxRuntimeGeometryBytes = 256ull << 20;
    }
};
enum class SceneBuildDisposition : uint32_t
{
    NotBuilt,
    Built,
    Reused
};
struct SceneBuildJobResult
{
    Status                  status{StatusCode::InvalidRequest, "Job was not executed"};
    SceneBuildDisposition   disposition{SceneBuildDisposition::NotBuilt};
    bool                    published{false};
    std::string             output;
    std::string             cacheKey;
    std::vector<Diagnostic> diagnostics;
    uint32_t                diagnosticsOmitted{0};
};
struct SceneBuildResult
{
    Status status;
    // Always in request order; an unsuccessful job never returns a partial Scene.
    std::vector<SceneBuildJobResult> jobs;
};
struct SceneBuildManifest
{
    Status                     status;
    std::string                outputDirectory;
    std::vector<SceneBuildJob> jobs;
};

// Admission reservation: source + decoded image + max(glTF/FBX geometry, OBJ
// geometry) limits, two scene allocation limits, hash scratch and job metadata.
// One source backend executes per job. Geometry includes conservative requested
// canonical/deformation storage and conversion scratch, not parser/codec-private
// allocations or allocator bookkeeping. This is not a process RSS bound.
uint64_t SceneBuildReservationBytes(const SceneBuildOptions& options);
// Strict JSON version 1. Relative source/output-directory paths use the manifest's
// parent. Every output remains a relative path within that directory.
SceneBuildManifest ReadSceneBuildManifest(const std::string&       path,
                                          const SceneBuildOptions& limits       = {},
                                          const Cancellation*      cancellation = nullptr);
// Synchronous: all workers drain before return. Same-process batches serialize
// with cancellable waiting; external processes must coordinate their writers.
// Preflight discovers all dependencies before publication. A changed job imports
// twice so preflight does not retain every scene. Publication is atomic per job,
// not a transaction over the entire batch. Prepublication failures preserve old
// output; published distinguishes a committed output with a durability error.
SceneBuildResult BuildSceneBatch(std::span<const SceneBuildJob> jobs,
                                 const SceneBuildOptions&       options,
                                 const Cancellation*            cancellation = nullptr);
} // namespace zenasset
