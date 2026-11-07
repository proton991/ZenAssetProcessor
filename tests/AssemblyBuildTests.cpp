#include "ZenAsset/Processor/Build.h"
#include "ZenAsset/Reader/Assembly.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace
{
using namespace zenasset;
constexpr const char* triangle = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
uint32_t              checks   = 0;
bool                  Check(bool value, const char* description)
{
    ++checks;
    if (!value)
    {
        std::fprintf(stderr, "%s\n", description);
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
struct Events
{
    uint32_t              imports{0};
    bool                  cancel{false};
    std::filesystem::path mutate;
};
void Progress(void* user, uint32_t, SceneBuildStage stage)
{
    Events& events = *static_cast<Events*>(user);
    if (stage == SceneBuildStage::Importing)
    {
        ++events.imports;
    }
    if (stage == SceneBuildStage::Publishing && !events.mutate.empty())
    {
        Save(events.mutate, "v 0 0 0\nv 3 0 0\nv 0 1 0\nf 1 2 3\n");
    }
}
bool Cancelled(void* user)
{
    return static_cast<Events*>(user)->cancel;
}
bool Run(const std::filesystem::path& directory)
{
    bool valid = true;
    Save(directory / "a.obj", triangle);
    Save(directory / "b.obj", triangle);
    const std::string text =
        R"({"version":2,"outputDirectory":"out","assembly":"world.zenscene","jobs":[{"key":"a","source":"a.obj"},{"key":"b","source":"b.obj"}],"instances":[{"key":"one","model":"a"},{"key":"two","model":"b","submitLights":false}]})";
    Save(directory / "build.json", text);
    Events            events;
    SceneBuildOptions options;
    options.durable                        = false;
    options.importContext                  = {1, 8ull << 20, 8ull << 20, 8ull << 20, 8ull << 20};
    options.limits.maxAllocationBytes      = 16ull << 20;
    options.limits.maxRuntimeGeometryBytes = 16ull << 20;
    options.maxEncoderBytes                = 8ull << 20;
    options.maxJobMetadataBytes            = 64ull << 10;
    options.maxReservedMemoryBytes         = 128ull << 20;
    options.callbacks                      = {&events, &Progress};
    SceneBuildManifest manifest            = ReadSceneBuildManifest((directory / "build.json").string(), options);
    valid                   &= Check(manifest.status.Succeeded() && manifest.version == 2, "Parse version 2 strict manifest");
    options.outputDirectory  = manifest.outputDirectory;
    SceneBuildResult result  = BuildSceneAssembly(manifest, options);
    valid                   &= Check(result.status.Succeeded() && result.assemblyPublished && events.imports == 2,
                                     "Two models import exactly once and assembly publishes");
    if (!result.status.Succeeded())
    {
        std::fprintf(stderr, "%s\n", result.status.message.c_str());
    }
    const std::filesystem::path output   = directory / "out/world.zenscene";
    const std::string           original = Bytes(output);
    const AssemblyReadResult    first    = ReadAssembly(output.string());
    valid &=
        Check(first.status.Succeeded() && first.assembly->counts.triangles == 2, "Assembly summarizes both model triangles");
    result  = BuildSceneAssembly(manifest, options);
    valid  &= Check(result.status.Succeeded() && !result.assemblyPublished && events.imports == 2 && Bytes(output) == original,
                    "Unchanged build reuses models and leaves assembly bytes unchanged");
    std::error_code                       error;
    const std::filesystem::file_time_type timestamp = std::filesystem::last_write_time(directory / "a.obj", error);
    Save(directory / "a.obj", "v 0 0 0\nv 2 0 0\nv 0 1 0\nf 1 2 3\n");
    std::filesystem::last_write_time(directory / "a.obj", timestamp, error);
    result  = BuildSceneAssembly(manifest, options);
    valid  &= Check(result.status.Succeeded() && result.jobs[0].disposition == SceneBuildDisposition::Built
                        && result.jobs[1].disposition == SceneBuildDisposition::Reused && events.imports == 3,
                    "Same-size same-time edit rebuilds only changed model");
    const std::string beforeFailure               = Bytes(output);
    options.force                                 = true;
    manifest.assembly.instances[0].transform[3].x = 1;
    events.mutate                                 = directory / "a.obj";
    result                                        = BuildSceneAssembly(manifest, options);
    valid &= Check(!result.status.Succeeded() && !result.assemblyPublished && Bytes(output) == beforeFailure,
                   "Source mutation at publication preserves prior assembly");
    events.mutate.clear();
    options.force        = false;
    options.assemblyOnly = true;
    std::filesystem::rename(directory / "a.obj", directory / "a.hidden", error);
    std::filesystem::rename(directory / "b.obj", directory / "b.hidden", error);
    manifest.assembly.instances[0].transform[3].x = 3;
    const uint32_t previousImports                = events.imports;
    result                                        = BuildSceneAssembly(manifest, options);
    valid &= Check(result.status.Succeeded() && result.assemblyPublished && !result.sourceFreshnessChecked
                       && events.imports == previousImports,
                   "Source-free placement update reads no sources and imports nothing");
    const AssemblyReadResult sourceFree = ReadAssembly(output.string());
    valid &=
        Check(sourceFree.status.Succeeded() && VerifyAssemblyDependencies(output.string(), *sourceFree.assembly).Succeeded(),
              "Published assembly dependencies verify without sources");
    if (sourceFree.status.Succeeded())
    {
        const std::filesystem::path model = directory / "out" / sourceFree.assembly->models[0].path;
        Save(model, "corrupt");
        const std::string preserved = Bytes(output);
        result                      = BuildSceneAssembly(manifest, options);
        valid &= Check(!result.status.Succeeded() && Bytes(output) == preserved, "Source-free update rejects corrupt identity");
    }
    options.assemblyOnly = false;
    std::filesystem::rename(directory / "a.hidden", directory / "a.obj", error);
    std::filesystem::rename(directory / "b.hidden", directory / "b.obj", error);
    result  = BuildSceneAssembly(manifest, options);
    valid  &= Check(result.status.Succeeded(), "Normal build recovers corruption without overwriting immutable model");
    const std::string validOutputName      = manifest.assemblyOutput;
    const std::string beforeInvalidPath    = Bytes(output);
    const uint32_t    beforeInvalidImports = events.imports;
    manifest.assemblyOutput                = std::string("\xc3\x28", 2) + ".zenscene";
    result                                 = BuildSceneAssembly(manifest, options);
    valid                   &= Check(result.status.code == StatusCode::InvalidArgument && events.imports == beforeInvalidImports
                                         && Bytes(output) == beforeInvalidPath,
                                     "Direct API malformed UTF-8 output returns status before path conversion or import");
    manifest.assemblyOutput  = std::string("world\0other.zenscene", 20);
    result                   = BuildSceneAssembly(manifest, options);
    valid                   &= Check(result.status.code == StatusCode::InvalidArgument && events.imports == beforeInvalidImports
                                         && Bytes(output) == beforeInvalidPath,
                                     "Direct API embedded NUL output returns status without publication");
    manifest.assemblyOutput  = validOutputName;
    events.cancel            = true;
    const Cancellation cancellation(&Cancelled, &events);
    result  = BuildSceneAssembly(manifest, options, &cancellation);
    valid  &= Check(result.status.code == StatusCode::Cancelled, "Cancelled assembly build reports cancellation");
    Save(
        directory / "bad.json",
        R"({"version":2,"outputDirectory":"out","assembly":"world.zenscene","jobs":[{"key":"a","source":"a.obj","output":"bad.zenasset"}],"instances":[{"key":"one","model":"a"}]})");
    valid &= Check(!ReadSceneBuildManifest((directory / "bad.json").string()).status.Succeeded(),
                   "Assembly model output names cannot be overridden");
    Save(
        directory / "bad.json",
        R"({"version":2,"version":2,"outputDirectory":"out","assembly":"world.zenscene","jobs":[{"key":"a","source":"a.obj"}],"instances":[{"key":"one","model":"a"}]})");
    valid &= Check(!ReadSceneBuildManifest((directory / "bad.json").string()).status.Succeeded(),
                   "Repeated manifest fields rejected");
    return valid;
}
} // namespace
int main()
{
    const std::filesystem::path directory =
        std::filesystem::current_path()
        / ("assembly-build-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    const bool valid = !error && Run(directory);
    // Retain owned fixture evidence for failures.
    std::printf("%u assembly build checks\n", checks);
    return valid ? 0 : 1;
}
