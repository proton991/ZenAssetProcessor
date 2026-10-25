#include "ZenAsset/Processor/Import.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
using namespace zenasset;
bool Check(bool value, const char* message)
{
    if (!value)
    {
        std::fprintf(stderr, "%s\n", message);
    }
    return value;
}
bool Save(const std::filesystem::path& path, std::string_view bytes)
{
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(file);
}
struct Observations
{
    SceneImportControl*                  control{nullptr};
    std::mutex                           mutex;
    std::vector<SceneImportFileProgress> changes;
    std::atomic<uint32_t>                active{0};
    std::atomic<bool>                    valid{true};
    bool                                 cancelOnCompletion{false};
};
void RecordProgress(void* user, SceneImportFileProgress progress)
{
    Observations& observations = *static_cast<Observations*>(user);
    if (observations.active.fetch_add(1) != 0)
    {
        observations.valid.store(false);
    }
    const SceneImportFileProgress polled = observations.control->GetFileProgress();
    if (polled.processedFiles != progress.processedFiles || polled.totalFiles != progress.totalFiles
        || polled.totalKnown != progress.totalKnown || progress.processedFiles > progress.totalFiles)
    {
        observations.valid.store(false);
    }
    {
        std::lock_guard<std::mutex> lock(observations.mutex);
        observations.changes.push_back(progress);
    }
    if (observations.cancelOnCompletion && progress.processedFiles > 0)
    {
        observations.control->Cancel();
    }
    observations.active.fetch_sub(1);
}
void CompleteFiles(SceneImportControl*             control,
                   const std::vector<std::string>* paths,
                   uint32_t                        worker,
                   std::atomic<uint32_t>*          finished)
{
    for (uint32_t index = worker; index < paths->size(); index += 4)
    {
        control->CompleteFile((*paths)[index]);
        control->CompleteFile((*paths)[index]);
    }
    finished->fetch_add(1);
}
bool Control(const std::filesystem::path& directory)
{
    bool               valid = Save(directory / "first.bin", "a") && Save(directory / "unregistered.bin", "b");
    Observations       observations;
    SceneImportControl control({&observations, nullptr, nullptr, &RecordProgress});
    observations.control = &control;
    control.CompleteFile((directory / "unregistered.bin").string());
    control.RegisterFile((directory / "first.bin").string());
    control.RegisterFile((directory / "." / "first.bin").string());
    control.RegisterFile((directory / "missing.bin").string());
    SceneImportFileProgress progress  = control.GetFileProgress();
    valid                            &= Check(progress.processedFiles == 0 && progress.totalFiles == 1 && !progress.totalKnown,
                                              "Only existing distinct files register; discovery is initially open");
    control.FinishFileDiscovery();
    control.CompleteFile((directory / "." / "first.bin").string());
    control.CompleteFile((directory / "first.bin").string());
    progress  = control.GetFileProgress();
    valid    &= Check(progress.totalKnown && progress.totalFiles == 1 && progress.processedFiles == 1,
                      "Alias and repeated completion must count one file");
    valid    &= Check(observations.valid.load() && observations.changes.size() == 3,
                      "Callbacks must expose coherent snapshots and only actual changes");

    SceneImportControl cancelled;
    cancelled.RegisterFile((directory / "first.bin").string());
    cancelled.Cancel();
    cancelled.CompleteFile((directory / "first.bin").string());
    cancelled.RegisterFile((directory / "unregistered.bin").string());
    cancelled.FinishFileDiscovery();
    progress  = cancelled.GetFileProgress();
    valid    &= Check(progress.processedFiles == 0 && progress.totalFiles == 1 && !progress.totalKnown,
                      "Cancelled work must not add files or report completion");
    return valid;
}
bool ParallelControl(const std::filesystem::path& directory)
{
    bool               valid = true;
    Observations       observations;
    SceneImportControl control({&observations, nullptr, nullptr, &RecordProgress});
    observations.control = &control;
    std::vector<std::string> paths;
    for (uint32_t index = 0; index < 32; ++index)
    {
        const std::filesystem::path path  = directory / ("parallel-" + std::to_string(index));
        valid                            &= Save(path, "x");
        paths.push_back(path.string());
        control.RegisterFile(paths.back());
    }
    control.FinishFileDiscovery();
    std::atomic<uint32_t>    finished{0};
    std::vector<std::thread> workers;
    for (uint32_t worker = 0; worker < 4; ++worker)
    {
        workers.emplace_back(&CompleteFiles, &control, &paths, worker, &finished);
    }
    uint64_t previous = 0;
    while (finished.load() != 4)
    {
        const SceneImportFileProgress progress = control.GetFileProgress();
        valid    &= progress.totalKnown && progress.totalFiles == paths.size() && progress.processedFiles >= previous
                 && progress.processedFiles <= progress.totalFiles;
        previous  = progress.processedFiles;
        std::this_thread::yield();
    }
    for (std::thread& worker : workers)
    {
        worker.join();
    }
    const SceneImportFileProgress progress  = control.GetFileProgress();
    valid                                  &= Check(progress.processedFiles == paths.size() && observations.valid.load(),
                                                    "Concurrent completions and polling must remain coherent");
    valid                                  &= Check(observations.changes.size() == paths.size() * 2 + 1,
                                                    "Duplicate concurrent completions must not increment progress twice");
    return valid;
}
std::string Gltf(const std::string& image)
{
    return R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{}],
"buffers":[{"uri":"geometry.bin","byteLength":4}],
"images":[{"uri":")"
         + image + R"("},{"uri":"./)" + image
         + R"("},{"uri":"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jR1cAAAAASUVORK5CYII="}],
"textures":[{"source":0},{"source":0},{"source":1},{"source":2}],
"materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0},"metallicRoughnessTexture":{"index":1}},
"normalTexture":{"index":2},"emissiveTexture":{"index":3}}]})";
}
bool Inspect(const std::filesystem::path& path, uint64_t expected, bool cancel = false, bool failure = false)
{
    Observations observations;
    observations.cancelOnCompletion = cancel;
    SceneImportRequest request;
    request.path = path.string();
    request.control =
        std::make_shared<SceneImportControl>(SceneImportCallbacks{&observations, nullptr, nullptr, &RecordProgress});
    observations.control = request.control.get();
    SceneImportContext context;
    context.workerCount                    = 4;
    const SceneImportResult       result   = ImportScene(request, context);
    const SceneImportFileProgress progress = request.control->GetFileProgress();
    bool                          valid = Check(observations.valid.load(), "Importer callback snapshots must remain coherent");
    for (const SceneImportFileProgress& update : observations.changes)
    {
        valid &= Check(update.totalFiles == 0 || update.processedFiles < update.totalFiles || update.totalKnown,
                       "Root input must stay pending until dependency discovery and normalization complete");
    }
    if (cancel || failure)
    {
        valid &=
            Check(!result.asset && result.status != StatusCode::Success && (!cancel || result.status == StatusCode::Cancelled),
                  "Failed or cancelled imports must not publish a scene");
        valid &= Check(progress.processedFiles < progress.totalFiles,
                       "Failed or cancelled input processing must leave progress incomplete");
    }
    else
    {
        if (result.status != StatusCode::Success)
        {
            std::fprintf(stderr, "%s: %s\n", request.path.c_str(), result.error.c_str());
        }
        valid &= Check(result.asset && result.status == StatusCode::Success, "Fixture import must succeed");
        valid &= Check(progress.totalKnown && progress.totalFiles == expected && progress.processedFiles == expected,
                       "Completed import must count only distinct physical input files");
    }
    return valid;
}
bool Fixtures(const std::filesystem::path& directory)
{
    const uint8_t png[]{137, 80, 78, 71, 13, 10, 26,  10,  0,  0,  0, 13, 73, 72, 68, 82, 0,  0,  0,   1,   0,  0,   0,
                        1,   8,  4,  0,  0,  0,  181, 28,  12, 2,  0, 0,  0,  11, 73, 68, 65, 84, 120, 218, 99, 252, 255,
                        31,  0,  3,  3,  2,  0,  239, 163, 71, 87, 0, 0,  0,  0,  73, 69, 78, 68, 174, 66,  96, 130};
    bool          valid  = Save(directory / "shared.png", std::string_view(reinterpret_cast<const char*>(png), sizeof(png)));
    valid               &= Save(directory / "geometry.bin", std::string(4, '\0'));
    valid               &= Save(directory / "scene.gltf", Gltf("shared.png"));
    valid               &= Save(directory / "bad.png", "not an image");
    valid               &= Save(directory / "invalid.gltf", Gltf("bad.png"));
    std::error_code error;
    std::filesystem::create_directory(directory / "materials", error);
    valid &= Check(!error, "Fixture material directory must be created");
    valid &= Save(directory / "materials/scene.mtl", "newmtl surface\nmap_Kd shared.png\nmap_Ke ./shared.png\n");
    valid &= Save(directory / "scene.obj", "mtllib materials/scene.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                           "vt 0 0\nvt 1 0\nvt 0 1\nusemtl surface\nf 1/1 2/2 3/3\n");
    valid &= Save(directory / "self.obj", "mtllib self.obj\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    if (valid)
    {
        valid &= Inspect(directory / "self.obj", 1);
        valid &= Inspect(directory / "scene.gltf", 3);
        valid &= Inspect(directory / "scene.obj", 3);
        valid &= Inspect(directory / "scene.gltf", 3, true);
        valid &= Inspect(directory / "invalid.gltf", 3, false, true);
    }
    return valid;
}
} // namespace
int main(int argc, char** argv)
{
    bool valid = true;
    if (argc > 1)
    {
        for (int index = 1; index < argc; ++index)
        {
            valid &= Inspect(std::filesystem::path(argv[index]), 1);
        }
    }
    else
    {
        std::error_code             error;
        const std::filesystem::path temporary = std::filesystem::temp_directory_path(error);
        const std::filesystem::path directory =
            temporary
            / ("zenasset-file-progress-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        valid &= Check(!error && std::filesystem::create_directory(directory, error) && !error,
                       "Temporary test directory must be created");
        if (valid)
        {
            valid &= Control(directory);
            valid &= ParallelControl(directory);
            valid &= Fixtures(directory);
            std::filesystem::remove_all(directory, error);
            valid &= Check(!error, "Temporary test files must be removed");
        }
    }
    return valid ? 0 : 1;
}