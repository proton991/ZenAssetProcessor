#include "ZenAsset/Processor/Assembly.h"
#include "ZenAsset/Reader/Assembly.h"
#include "Format/PackageLayout.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace
{
using namespace zenasset;
uint32_t checks = 0;
bool     Check(bool condition, const char* message)
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
    scene.name = "Triangle";
    scene.vertices.resize(3);
    scene.vertices[1].pos.x = 1;
    scene.vertices[2].pos.y = 1;
    scene.indices           = {0, 1, 2};
    scene.meshes.resize(1);
    MeshPrimitiveAsset primitive;
    primitive.vertexCount = 3;
    primitive.indexCount  = 3;
    primitive.bounds      = Bounds(Vec3(0), Vec3(1, 1, 0));
    scene.meshes[0].primitives.push_back(primitive);
    scene.nodes.resize(1);
    scene.nodes[0].mesh   = MeshId{0};
    scene.nodes[0].camera = CameraId{0};
    scene.nodes[0].light  = LightId{0};
    scene.cameras.resize(1);
    scene.lights.resize(1);
    scene.roots = {NodeId{0}};
    return scene;
}

std::vector<uint8_t> ReadBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
}
void WriteBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}
void RefreshDirectory(std::vector<uint8_t>& bytes)
{
    detail::Write64(bytes.data() + 48, detail::HashBytes(bytes.data() + 64, 64));
    detail::Write64(bytes.data() + 56, detail::HashBytes(bytes.data(), 56));
}
Status RejectPublish(void*)
{
    return {StatusCode::InvalidData, "Publication rejected by test"};
}

bool TestRoundtrip(const std::filesystem::path& directory)
{
    bool                        okay       = true;
    const Scene                 scene      = MakeScene();
    const std::filesystem::path modelPath  = directory / "model.zenasset";
    okay                                  &= Check(WriteScene(modelPath.string(), scene).Succeeded(), "write model fixture");
    const PackageResult modelPackage       = OpenPackage(modelPath.string());
    okay                                  &= Check(modelPackage.status.Succeeded(), "open model fixture");
    SceneAssembly assembly;
    assembly.name = "Two models and a repeated instance";
    AssemblyModel model;
    model.key  = "first";
    model.path = "model.zenasset";
    if (modelPackage.package != nullptr)
    {
        model.identity = modelPackage.package->Identity();
    }
    model.validationFingerprint  = "validator-v1";
    okay                        &= Check(SummarizeAssemblyModel(scene, model).Succeeded(), "summarize source model");
    okay                        &= Check(model.counts.triangles == 1 && model.counts.lightNodes == 1 && model.hasBounds,
                                         "model summary includes bounds and light instances");
    assembly.models.push_back(model);
    model.key = "second";
    assembly.models.push_back(model);
    assembly.instances.push_back({"one", "first"});
    AssemblyInstance second{"two", "second"};
    second.transform[3].x = 10;
    second.submitLights   = false;
    assembly.instances.push_back(second);
    AssemblyInstance repeat{"three", "first"};
    repeat.transform[0].x = -2;
    repeat.transform[3].x = -3;
    assembly.instances.push_back(repeat);
    assembly.camera  = {"two", 0};
    okay            &= Check(FinalizeAssembly(assembly).Succeeded(), "finalize assembly");
    okay            &= Check(assembly.counts.vertices == 9 && assembly.counts.triangles == 3 && assembly.bounds.min.x == -5
                                 && assembly.bounds.max.x == 11,
                             "aggregate repeated instances and negative scales");
    const std::filesystem::path path  = directory / "scene.zenscene";
    okay                             &= Check(WriteAssembly(path.string(), assembly).Succeeded(), "write native assembly");
    const AssemblyReadResult read     = ReadAssembly(path.string());
    okay &=
        Check(read.status.Succeeded() && read.assembly != nullptr && *read.assembly == assembly, "roundtrip exact assembly");
    okay &= Check(VerifyAssemblyDependencies(path.string(), assembly).Succeeded(), "verify source-free native dependencies");
    okay &= Check(!ReadScene(path.string()).status.Succeeded(), "scene reader rejects assembly as scene");
    const PackageResult package  = OpenPackage(path.string());
    okay                        &= Check(package.status.Succeeded() && package.package->Resources().size() == 1
                                             && package.package->Resources()[0].type == ResourceType::Assembly,
                                         "assembly uses existing native container");
    SceneWriteOptions compressed;
    compressed.compression.transport           = TransportCodec::Zstandard;
    const std::filesystem::path compressedPath = directory / "compressed.zenscene";
    okay &= Check(WriteAssembly(compressedPath.string(), assembly, compressed).Succeeded(), "compressed assembly write");
    const AssemblyReadResult compressedRead = ReadAssembly(compressedPath.string());
    okay &= Check(compressedRead.status.Succeeded() && *compressedRead.assembly == assembly, "compressed assembly roundtrip");
    SceneReadLimits tiny;
    tiny.maxAllocationBytes = 1;
    okay &= Check(ReadAssembly(path.string(), tiny).status.code == StatusCode::ResourceLimit, "reader allocation bound");
    tiny  = {};
    tiny.maxArrayElements = 1;
    okay &= Check(ReadAssembly(path.string(), tiny).status.code == StatusCode::ResourceLimit, "reader array bound");
    if (package.package != nullptr)
    {
        tiny                       = {};
        tiny.package.maxFileBytes  = 1;
        okay                      &= Check(ReadAssembly(*package.package, tiny).status.code == StatusCode::ResourceLimit,
                                           "retained package rechecks smaller limits");
    }
    Cancellation cancelled;
    cancelled.Cancel();
    okay &= Check(ReadAssembly(path.string(), {}, &cancelled).status.code == StatusCode::Cancelled, "cancelled assembly read");
    const std::vector<uint8_t> original = ReadBytes(path);
    SceneWriteOptions          rejected;
    bool                       published = true;
    rejected.published                   = &published;
    rejected.beforePublish               = RejectPublish;
    okay &= Check(!WriteAssembly(path.string(), assembly, rejected).Succeeded() && !published && ReadBytes(path) == original,
                  "failed prepublication preserves old assembly");
    okay &= Check(WriteAssembly(path.string(), assembly, {}, &cancelled).code == StatusCode::Cancelled
                      && ReadBytes(path) == original,
                  "cancelled write preserves old assembly");
    rejected                           = {};
    rejected.limits.maxAllocationBytes = 64;
    okay &=
        Check(WriteAssembly(path.string(), assembly, rejected).code == StatusCode::ResourceLimit && ReadBytes(path) == original,
              "writer budget failure preserves old assembly");
    SceneAssembly invalid                = assembly;
    invalid.models[0].path               = "../model.zenasset";
    okay                                &= Check(!FinalizeAssembly(invalid).Succeeded(), "reject parent traversal");
    invalid                              = assembly;
    invalid.models[0].path               = "C:/model.zenasset";
    okay                                &= Check(!FinalizeAssembly(invalid).Succeeded(), "reject absolute model path");
    invalid                              = assembly;
    invalid.models[1].key                = invalid.models[0].key;
    okay                                &= Check(!FinalizeAssembly(invalid).Succeeded(), "reject duplicate model keys");
    invalid                              = assembly;
    invalid.instances[1].key             = invalid.instances[0].key;
    okay                                &= Check(!FinalizeAssembly(invalid).Succeeded(), "reject duplicate instance keys");
    invalid                              = assembly;
    invalid.instances[0].model           = "missing";
    okay                                &= Check(!FinalizeAssembly(invalid).Succeeded(), "reject dangling model reference");
    invalid                              = assembly;
    invalid.instances[0].transform[1].w  = 1;
    okay                                &= Check(!FinalizeAssembly(invalid).Succeeded(), "reject nonaffine placement");
    invalid                              = assembly;
    invalid.camera.resource              = 1;
    okay                                &= Check(!FinalizeAssembly(invalid).Succeeded(), "reject invalid camera selector");
    invalid                              = assembly;
    invalid.environment                  = {"one", 0};
    okay                                &= Check(!FinalizeAssembly(invalid).Succeeded(), "reject invalid environment selector");
    invalid                              = assembly;
    invalid.counts.vertices++;
    okay    &= Check(!WriteAssembly(path.string(), invalid).Succeeded(), "reject inconsistent aggregate metadata");
    invalid  = assembly;
    invalid.models[0].counts.vertices = UINT64_MAX;
    okay    &= Check(FinalizeAssembly(invalid).code == StatusCode::ResourceLimit, "reject aggregate overflow");
    invalid  = assembly;
    invalid.models[0].identity.headerHash ^= 1;
    okay &= Check(!VerifyAssemblyDependencies(path.string(), invalid).Succeeded(), "reject replaced dependency identity");
    const std::filesystem::path corruptPath  = directory / "corrupt.zenscene";
    std::vector<uint8_t>        corrupted    = original;
    corrupted.back()                        ^= 1;
    WriteBytes(corruptPath, corrupted);
    okay      &= Check(!ReadAssembly(corruptPath.string()).status.Succeeded(), "reject corrupted assembly payload");
    corrupted  = original;
    detail::Write32(corrupted.data() + 76, 2);
    RefreshDirectory(corrupted);
    WriteBytes(corruptPath, corrupted);
    okay      &= Check(OpenPackage(corruptPath.string()).status.code == StatusCode::UnsupportedVersion,
                       "reject future required assembly version");
    corrupted  = original;
    corrupted.pop_back();
    WriteBytes(corruptPath, corrupted);
    okay &= Check(!ReadAssembly(corruptPath.string()).status.Succeeded(), "reject truncated assembly");
    const std::filesystem::path relocated = directory / "relocated";
    std::error_code             error;
    std::filesystem::create_directory(relocated, error);
    std::filesystem::copy_file(path, relocated / "scene.zenscene", error);
    std::filesystem::copy_file(modelPath, relocated / "model.zenasset", error);
    okay &= Check(!error && VerifyAssemblyDependencies((relocated / "scene.zenscene").string(), assembly).Succeeded(),
                  "assembly relocates without original source data");
    std::filesystem::remove(relocated / "model.zenasset", error);
    okay &= Check(!VerifyAssemblyDependencies((relocated / "scene.zenscene").string(), assembly).Succeeded(),
                  "missing dependency fails");
    return okay;
}

bool TestHierarchy()
{
    bool  okay                 = true;
    Scene scene                = MakeScene();
    scene.nodes[0].translation = Vec3(2, 3, 4);
    scene.nodes.push_back({});
    scene.nodes[1].scale    = Vec3(2);
    scene.nodes[0].parent   = NodeId{1};
    scene.nodes[1].children = {NodeId{0}};
    scene.roots             = {NodeId{1}};
    AssemblyModel model;
    okay                  &= Check(SummarizeAssemblyModel(scene, model).Succeeded() && model.bounds.min == Vec3(4, 6, 8)
                                       && model.bounds.max == Vec3(6, 8, 8),
                                   "summarize hierarchy in authored coordinates");
    scene.nodes[1].parent  = NodeId{0};
    okay                  &= Check(!SummarizeAssemblyModel(scene, model).Succeeded(), "summary rejects cyclic hierarchy");
    scene.nodes[1].parent  = NodeId{99};
    okay                  &= Check(!SummarizeAssemblyModel(scene, model).Succeeded(), "summary rejects invalid parent");
    scene                  = MakeScene();
    scene.nodes.resize(10000);
    for (uint32_t index = 0; index + 1 < scene.nodes.size(); ++index)
    {
        scene.nodes[index].parent        = NodeId{index + 1};
        scene.nodes[index].translation.x = 1;
    }
    scene.roots                    = {NodeId{9999}};
    okay                          &= Check(SummarizeAssemblyModel(scene, model).Succeeded() && model.bounds.min.x == 9999,
                                           "ten-thousand-deep hierarchy uses iterative linear traversal");
    const AssemblyModel unchanged  = model;
    okay &= Check(SummarizeAssemblyModel(scene, model, 0).code == StatusCode::ResourceLimit && model == unchanged,
                  "summary scratch admission precedes allocation and preserves output");
    Cancellation cancellation;
    cancellation.Cancel();
    okay &= Check(SummarizeAssemblyModel(scene, model, 64ull << 20, &cancellation).code == StatusCode::Cancelled
                      && model == unchanged,
                  "cancelled summary does not publish partial metadata");
    return okay;
}
} // namespace

int main()
{
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path()
        / ("zenasset-assembly-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code error;
    std::filesystem::create_directory(directory, error);
    bool okay = Check(!error, "create fixture directory");
    if (okay)
    {
        okay &= TestRoundtrip(directory);
        okay &= TestHierarchy();
    }
    // Only this test's newly created, uniquely named directory is removed.
    std::filesystem::remove_all(directory, error);
    std::printf("Assembly checks: %u\n", checks);
    return okay ? 0 : 1;
}
