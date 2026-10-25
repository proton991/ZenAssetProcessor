#include "ZenAsset/Processor/Build.h"
#include "Processor/BuildInternal.h"
#include "Importers/AssetHash.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace
{
using namespace zenasset;
uint32_t              checks   = 0;
constexpr const char* triangle = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
bool                  Check(bool value, const char* message)
{
    ++checks;
    if (!value)
    {
        std::fprintf(stderr, "%s\n", message);
    }
    return value;
}
void Save(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream file(path, std::ios::binary);
    file << text;
}
std::string Bytes(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}
SceneBuildOptions Options(const std::filesystem::path& directory)
{
    SceneBuildOptions options;
    options.outputDirectory                = directory.string();
    options.workerCount                    = 2;
    options.durable                        = false;
    options.importContext                  = {1, 8ull << 20, 8ull << 20, 8ull << 20, 8ull << 20};
    options.limits.maxAllocationBytes      = 16ull << 20;
    options.limits.maxRuntimeGeometryBytes = 16ull << 20;
    options.maxEncoderBytes                = 8ull << 20;
    options.maxJobMetadataBytes            = 64ull << 10;
    options.maxReservedMemoryBytes         = 128ull << 20;
    return options;
}
bool Incremental(const std::filesystem::path& directory)
{
    bool valid = true;
    Save(directory / "a.obj", triangle);
    Save(directory / "b.obj", triangle);
    SceneBuildOptions          options = Options(directory / "out");
    std::vector<SceneBuildJob> jobs{{(directory / "a.obj").string(), "a.zenasset"},
                                    {(directory / "b.obj").string(), "sub/b.zenasset"}};
    SceneBuildResult           result = BuildSceneBatch(jobs, options);
    valid &= Check(result.status.Succeeded() && result.jobs.size() == 2 && result.jobs[0].published && result.jobs[1].published,
                   "Two concurrent jobs build in ordered results");
    const std::string     original = Bytes(directory / "out/a.zenasset");
    const SceneReadResult read     = ReadScene((directory / "out/a.zenasset").string());
    valid  &= Check(read.status.Succeeded() && !read.metadata.inputs.empty() && !read.metadata.cacheKey.empty(),
                    "Batch output stores complete source provenance and identity");
    result  = BuildSceneBatch(jobs, options);
    valid  &= Check(result.status.Succeeded() && result.jobs[0].disposition == SceneBuildDisposition::Reused
                        && result.jobs[1].disposition == SceneBuildDisposition::Reused,
                    "Unchanged jobs reuse fully read packages");
    std::error_code                       error;
    const std::filesystem::file_time_type stamp = std::filesystem::last_write_time(directory / "a.obj", error);
    std::filesystem::last_write_time(directory / "a.obj", stamp + std::chrono::seconds(3), error);
    result  = BuildSceneBatch(jobs, options);
    valid  &= Check(result.status.Succeeded() && result.jobs[0].disposition == SceneBuildDisposition::Reused
                        && Bytes(directory / "out/a.zenasset") == original,
                    "Unchanged touch reuses without rewriting import stamps");
    Save(directory / "a.obj", "v 0 0 0\nv 2 0 0\nv 0 1 0\nf 1 2 3\n");
    std::filesystem::last_write_time(directory / "a.obj", stamp, error);
    result  = BuildSceneBatch(jobs, options);
    valid  &= Check(result.status.Succeeded() && result.jobs[0].disposition == SceneBuildDisposition::Built
                        && result.jobs[1].disposition == SceneBuildDisposition::Reused,
                    "Same-size same-time content edit invalidates");
    Save(directory / "out/a.zenasset", "corrupted");
    result  = BuildSceneBatch(jobs, options);
    valid  &= Check(result.status.Succeeded() && result.jobs[0].disposition == SceneBuildDisposition::Built,
                    "Corrupt package is rebuilt, never reused");
    jobs[0].sourceSettings.animationBake.translationTolerance *= 2;
    result                                                     = BuildSceneBatch(jobs, options);
    valid &= Check(result.status.Succeeded() && result.jobs[0].disposition == SceneBuildDisposition::Built,
                   "Source setting invalidates even when irrelevant to fixture");
    jobs[0].processing.textures.colorMinPsnr += 1;
    jobs[0].compression.zstandardLevel        = 4;
    jobs[0].profile                           = "desktop";
    result                                    = BuildSceneBatch(jobs, options);
    valid &= Check(result.status.Succeeded() && result.jobs[0].disposition == SceneBuildDisposition::Built,
                   "Disabled processing/compression values and profile participate in identity");
    const SceneReadResult stale = ReadScene((directory / "out/a.zenasset").string());
    if (stale.status.Succeeded())
    {
        SceneWriteOptions write;
        write.metadata                  = stale.metadata;
        write.metadata.buildFingerprint = "different-build";
        write.compression               = stale.metadata.compression;
        valid  &= Check(WriteScene((directory / "out/a.zenasset").string(), *stale.scene, write).Succeeded(),
                        "Author stale build fixture");
        result  = BuildSceneBatch(jobs, options);
        valid  &= Check(result.status.Succeeded() && result.jobs[0].disposition == SceneBuildDisposition::Built,
                        "Different library build invalidates reuse");
    }
    options.force = true;
    result        = BuildSceneBatch(jobs, options);
    valid &=
        Check(result.status.Succeeded() && result.jobs[0].published && result.jobs[1].published, "Force rebuilds all jobs");
    options.maxReservedMemoryBytes = SceneBuildReservationBytes(options) - 1;
    result                         = BuildSceneBatch(jobs, options);
    valid &= Check(result.status.code == StatusCode::ResourceLimit, "Insufficient admission reservation rejects preflight");
    options.maxJobMetadataBytes = UINT64_MAX;
    valid &= Check(SceneBuildReservationBytes(options) == UINT64_MAX, "Reservation overflow fails closed");
    return valid;
}
bool Dependencies(const std::filesystem::path& directory)
{
    bool            valid = true;
    std::error_code error;
    std::filesystem::create_directories(directory / "materials", error);
    Save(directory / "material.zenasset", "newmtl a\nKd 1 0 0\n");
    Save(directory / "dep.obj", "mtllib material.zenasset\nusemtl a\n" + std::string(triangle));
    SceneBuildOptions          options = Options(directory / "dep-out");
    std::vector<SceneBuildJob> jobs{{(directory / "dep.obj").string(), "dep.zenasset"}};
    SceneBuildResult           result            = BuildSceneBatch(jobs, options);
    valid                                       &= Check(result.status.Succeeded(), "Dependency scene builds");
    const std::filesystem::file_time_type stamp  = std::filesystem::last_write_time(directory / "material.zenasset", error);
    Save(directory / "material.zenasset", "newmtl a\nKd 0 1 0\n");
    std::filesystem::last_write_time(directory / "material.zenasset", stamp, error);
    result  = BuildSceneBatch(jobs, options);
    valid  &= Check(result.status.Succeeded() && result.jobs[0].published, "Dependency edit with same size/time invalidates");
    options.outputDirectory  = directory.string();
    jobs[0].output           = "material.zenasset";
    const std::string before = Bytes(directory / "material.zenasset");
    result                   = BuildSceneBatch(jobs, options);
    valid &= Check(result.status.code == StatusCode::InvalidArgument && Bytes(directory / "material.zenasset") == before,
                   "Output cannot overwrite a dependency");
    jobs   = {{(directory / "a.obj").string(), "duplicate.zenasset"}, {(directory / "b.obj").string(), "duplicate.zenasset"}};
    valid &= Check(BuildSceneBatch(jobs, options).status.code == StatusCode::InvalidArgument,
                   "Duplicate outputs rejected before builds");
    jobs[1].output = "../escape.zenasset";
    valid &= Check(BuildSceneBatch(jobs, options).status.code == StatusCode::InvalidArgument, "Output traversal rejected");
    std::filesystem::create_hard_link(directory / "a.obj", directory / "hard.zenasset", error);
    if (!error)
    {
        jobs.resize(1);
        jobs[0].output = "hard.zenasset";
        valid &=
            Check(BuildSceneBatch(jobs, options).status.code == StatusCode::InvalidArgument, "Source hardlink alias rejected");
    }
    std::string tga(22, '\0');
    tga[2]  = 2;
    tga[12] = 1;
    tga[14] = 1;
    tga[16] = 32;
    tga[17] = 0x28;
    tga[21] = static_cast<char>(255);
    Save(directory / "color.tga", tga);
    Save(directory / "materials/fallback.mtl", "newmtl a\nmap_Kd color.tga\n");
    Save(directory / "fallback.obj", "mtllib materials/fallback.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                     "vt 0 0\nvt 1 0\nvt 0 1\nusemtl a\nf 1/1 2/2 3/3\n");
    options.outputDirectory  = (directory / "fallback-out").string();
    jobs                     = {{(directory / "fallback.obj").string(), "fallback.zenasset"}};
    result                   = BuildSceneBatch(jobs, options);
    valid                   &= Check(result.status.Succeeded(), "Fallback texture source builds");
    Save(directory / "materials/color.tga", tga);
    result = BuildSceneBatch(jobs, options);
    valid &=
        Check(result.status.Succeeded() && result.jobs[0].published, "Missing preferred texture appearance invalidates reuse");
    return valid;
}
struct Change
{
    std::filesystem::path path;
    Cancellation*         cancellation{nullptr};
    bool                  changed{false};
};
void Mutate(void* user, uint32_t, SceneBuildStage stage)
{
    Change& change = *static_cast<Change*>(user);
    if (!change.changed && stage == SceneBuildStage::Publishing)
    {
        change.changed = true;
        if (change.cancellation != nullptr)
        {
            change.cancellation->Cancel();
        }
        else
        {
            std::error_code                       error;
            const std::filesystem::file_time_type stamp = std::filesystem::last_write_time(change.path, error);
            Save(change.path, "v 0 0 0\nv 9 0 0\nv 0 1 0\nf 1 2 3\n");
            std::filesystem::last_write_time(change.path, stamp, error);
        }
    }
}
bool Publication(const std::filesystem::path& directory)
{
    bool valid = true;
    Save(directory / "publish.obj", triangle);
    SceneBuildOptions   options = Options(directory / "publication");
    const SceneBuildJob job{(directory / "publish.obj").string(), "out.zenasset"};
    valid                    &= Check(BuildSceneBatch(std::span(&job, 1), options).status.Succeeded(), "Publication baseline");
    const std::string before  = Bytes(directory / "publication/out.zenasset");
    Change            change{directory / "publish.obj"};
    options.force            = true;
    options.callbacks        = {&change, &Mutate};
    SceneBuildResult result  = BuildSceneBatch(std::span(&job, 1), options);
    valid                   &= Check(change.changed && !result.status.Succeeded() && !result.jobs[0].published
                                         && Bytes(directory / "publication/out.zenasset") == before,
                                     "Final content check rejects same-time edit and preserves output");
    Cancellation cancellation;
    change             = {directory / "publish.obj", &cancellation};
    result             = BuildSceneBatch(std::span(&job, 1), options, &cancellation);
    valid             &= Check(result.status.code == StatusCode::Cancelled && !result.jobs[0].published
                                   && Bytes(directory / "publication/out.zenasset") == before,
                               "Prepublication cancellation preserves output");
    options.callbacks  = {};
    Save(directory / "publish.obj", "not a valid model\n");
    result  = BuildSceneBatch(std::span(&job, 1), options);
    valid  &= Check(!result.status.Succeeded() && Bytes(directory / "publication/out.zenasset") == before,
                    "Failed preflight preserves previous package");
    return valid;
}
bool Manifest(const std::filesystem::path& directory)
{
    bool                        valid = true;
    const std::filesystem::path path  = directory / "build.json";
    Save(
        path,
        R"({"version":1,"outputDirectory":"out","jobs":[{"source":"a.obj","output":"a.zenasset","profile":"desktop","compression":{"transport":"zstd","meshoptimizer":true},"sourceSettings":{"animationBake":{"sampleRate":30}},"processing":{"textures":{"hdrMaxRelativeRmse":0.01,"arrayPages":true,"arrayMaxDimension":512,"arrayMaxLayers":4,"arrayMaxPages":8,"arrayMaxImages":32}}}]})");
    const SceneBuildManifest read  = ReadSceneBuildManifest(path.string());
    valid                         &= Check(
        read.status.Succeeded() && read.jobs.size() == 1 && read.jobs[0].sourceSettings.animationBake.sampleRate == 30
            && read.jobs[0].compression.meshoptimizer && read.jobs[0].processing.textures.arrayPages
            && read.jobs[0].processing.textures.arrayMaxDimension == 512 && read.jobs[0].processing.textures.arrayMaxLayers == 4
            && read.jobs[0].processing.textures.arrayMaxPages == 8 && read.jobs[0].processing.textures.arrayMaxImages == 32,
        "Strict manifest resolves paths and typed settings");
    const char* malformed[]{
        "[]",
        "null",
        R"({"version":1,"outputDirectory":"out","jobs":1})",
        R"({"version":1,"outputDirectory":"out","jobs":[1]})",
        R"({"version":1,"outputDirectory":"out","jobs":[{"source":"a.obj","output":"a.zenasset","processing":1}]})",
        R"({"version":1,"outputDirectory":"out","jobs":[{"source":"a.obj","output":"a.zenasset","processing":{"textures":null}}]})",
        R"({"version":1,"outputDirectory":"out","jobs":[{"source":"a.obj","output":"a.zenasset","sourceSettings":[]}]})",
        R"({"version":2,"outputDirectory":"out","jobs":[]})",
        R"({"version":1,"version":1,"outputDirectory":"out","jobs":[]})",
        R"({"version":1,"outputDirectory":"out","jobs":[{"source":"a.obj","output":"a.zenasset","unknown":1}]})",
        R"({"version":1,"outputDirectory":"out","jobs":[{"source":"a.obj","output":"a.zenasset","processing":{"textures":{"compressBC7":1}}}]})",
        R"({"version":1,"outputDirectory":"out","jobs":[{"source":"a.obj","output":"a.zenasset","sourceSettings":{"sceneIndex":1.5}}]})"};
    for (const char* value : malformed)
    {
        Save(path, value);
        const SceneBuildManifest bad = ReadSceneBuildManifest(path.string());
        valid &= Check(!bad.status.Succeeded() && bad.jobs.empty(), "Malformed manifest exposes no partial jobs");
    }
    SceneBuildOptions limits;
    limits.maxManifestBytes  = 8;
    valid                   &= Check(ReadSceneBuildManifest(path.string(), limits).status.code == StatusCode::ResourceLimit,
                                     "Manifest bytes bounded before parsing");
    return valid;
}
struct Gate
{
    std::mutex              mutex;
    std::condition_variable changed;
    bool                    entered{false};
    bool                    release{false};
};
bool CountCancel(void* user)
{
    uint32_t& polls = *static_cast<uint32_t*>(user);
    return ++polls >= 4;
}
bool PlanningCancellation(const std::filesystem::path& directory)
{
    bool                valid = true;
    uint32_t            polls = 0;
    Cancellation        cancelPaths(&CountCancel, &polls);
    const SceneBuildJob jobs[]{{(directory / "a.obj").string(), "cancel-a.zenasset"},
                               {(directory / "b.obj").string(), "cancel-b.zenasset"}};
    valid &= Check(BuildSceneBatch(jobs, Options(directory), &cancelPaths).status.code == StatusCode::Cancelled,
                   "Cancellation interrupts path/collision preflight");
    const std::filesystem::path path = directory / "cancel.json";
    Save(path, R"({"version":1,"outputDirectory":"out","jobs":[{"source":"a.obj","output":"a.zenasset"}]})");
    polls = 0;
    Cancellation             cancelManifest(&CountCancel, &polls);
    const SceneBuildManifest manifest  = ReadSceneBuildManifest(path.string(), {}, &cancelManifest);
    valid                             &= Check(manifest.status.code == StatusCode::Cancelled && manifest.jobs.empty(),
                                               "Manifest cancellation after bytes exposes no partial jobs");
    SceneBuildOptions limits;
    limits.maxJobs = 0;
    valid &=
        Check(!ReadSceneBuildManifest(path.string(), limits).status.Succeeded(), "Oversized jobs rejected before iteration");
    return valid;
}
bool Identity()
{
    bool                       valid = true;
    SceneBuildJob              base{"a.obj", "a.zenasset"};
    const std::string          key = detail::BuildSettingsKey(base, "obj", 1);
    std::vector<SceneBuildJob> variants(13, base);
    variants[0].sourceSettings.sceneIndex                          = 0;
    variants[1].sourceSettings.fallbackMissingNamedUVSets          = false;
    variants[2].sourceSettings.animationBake.sampleRate            = 30;
    variants[3].sourceSettings.animationBake.translationTolerance *= 2;
    variants[4].sourceSettings.animationBake.rotationTolerance    *= 2;
    variants[5].sourceSettings.animationBake.scaleTolerance       *= 2;
    variants[6].sourceSettings.animationBake.valueTolerance       *= 2;
    variants[7].sourceSettings.animationBake.maxSubdivisions      += 1;
    variants[8].sourceSettings.animationBake.maxValues            += 1;
    variants[9].profile                                            = "desktop";
    variants[10].compression.transport                             = TransportCodec::Zstandard;
    variants[11].compression.zstandardLevel                        = 4;
    variants[12].compression.meshoptimizer                         = true;
    for (const SceneBuildJob& variant : variants)
    {
        valid &= Check(detail::BuildSettingsKey(variant, "obj", 1) != key,
                       "Every source/compression/profile field affects identity");
    }
    valid &= Check(detail::BuildSettingsKey(base, "obj", 2) != key && detail::BuildSettingsKey(base, "gltf", 1) != key,
                   "Importer format/version affect identity");
    return valid;
}
void Hold(void* user, uint32_t, SceneBuildStage stage)
{
    Gate& gate = *static_cast<Gate*>(user);
    if (stage == SceneBuildStage::Publishing)
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        gate.entered = true;
        gate.changed.notify_all();
        gate.changed.wait(lock, [&gate] { return gate.release; });
    }
}
void BuildHeld(const SceneBuildJob& job, const SceneBuildOptions& options, SceneBuildResult& result)
{
    result = BuildSceneBatch(std::span(&job, 1), options);
}
bool CancelWait(void* user)
{
    const std::chrono::steady_clock::time_point& deadline = *static_cast<const std::chrono::steady_clock::time_point*>(user);
    return std::chrono::steady_clock::now() >= deadline;
}
bool Waiting(const std::filesystem::path& directory)
{
    Save(directory / "wait.obj", triangle);
    const SceneBuildJob job{(directory / "wait.obj").string(), "wait.zenasset"};
    SceneBuildOptions   options = Options(directory / "waiting");
    Gate                gate;
    options.callbacks = {&gate, &Hold};
    SceneBuildResult first;
    std::thread      worker(&BuildHeld, std::cref(job), std::cref(options), std::ref(first));
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        gate.changed.wait(lock, [&gate] { return gate.entered; });
    }
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(40);
    Cancellation                          cancellation(&CancelWait, &deadline);
    SceneBuildOptions                     waiting = options;
    waiting.callbacks                             = {};
    const SceneBuildResult cancelled              = BuildSceneBatch(std::span(&job, 1), waiting, &cancellation);
    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        gate.release = true;
    }
    gate.changed.notify_all();
    worker.join();
    return Check(first.status.Succeeded() && cancelled.status.code == StatusCode::Cancelled,
                 "Second same-process batch cancels while first owns publication gate; workers drain");
}
} // namespace
int main()
{
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path()
        / ("zenasset-build-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    bool valid  = !error;
    valid      &= Incremental(directory);
    valid      &= Dependencies(directory);
    valid      &= Publication(directory);
    valid      &= Manifest(directory);
    valid      &= PlanningCancellation(directory);
    valid      &= Waiting(directory);
    valid      &= Identity();
    std::filesystem::remove_all(directory, error);
    std::printf("%u build checks\n", checks);
    return valid ? 0 : 1;
}
