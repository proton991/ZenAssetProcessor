#include "ZenAsset/Processor/Build.h"
#include "SceneValueComparison.h"
#include "Importers/ImportErrors.h"
#include <draco/compression/encode.h>
#include <draco/mesh/triangle_soup_mesh_builder.h>
#include <meshoptimizer.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <thread>

namespace
{
using namespace zenasset;
uint32_t checks = 0;
bool     Check(bool value, const char* message)
{
    ++checks;
    if (!value)
    {
        std::fprintf(stderr, "%s\n", message);
    }
    return value;
}
bool Save(const std::filesystem::path& path, const void* data, size_t size)
{
    std::ofstream file(path, std::ios::binary);
    file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return bool(file);
}
std::string Join(const std::vector<std::string>& items)
{
    std::string result;
    for (const std::string& item : items)
    {
        if (!result.empty())
        {
            result += ',';
        }
        result += item;
    }
    return result;
}
struct Fixture
{
    std::vector<uint8_t>     bytes;
    std::vector<std::string> views;
    std::vector<std::string> accessors;
    size_t                   AddView(const void* data, size_t size, std::string extra = {})
    {
        while (bytes.size() % 4 != 0)
        {
            bytes.push_back(0);
        }
        const size_t   offset = bytes.size();
        const uint8_t* first  = static_cast<const uint8_t*>(data);
        bytes.insert(bytes.end(), first, first + size);
        views.push_back("{\"buffer\":0,\"byteOffset\":" + std::to_string(offset) + ",\"byteLength\":" + std::to_string(size)
                        + extra + "}");
        return views.size() - 1;
    }
    size_t Add(const void* data, size_t size, size_t count, uint32_t component, const char* type)
    {
        const size_t view = AddView(data, size);
        accessors.push_back("{\"bufferView\":" + std::to_string(view) + ",\"componentType\":" + std::to_string(component)
                            + ",\"count\":" + std::to_string(count) + ",\"type\":\"" + type + "\"}");
        return accessors.size() - 1;
    }
    void Bounds(size_t index, const std::string& minimum, const std::string& maximum)
    {
        accessors[index].pop_back();
        accessors[index] += ",\"min\":" + minimum + ",\"max\":" + maximum + "}";
    }
    bool Write(const std::filesystem::path& path, const std::string& body)
    {
        std::filesystem::path binary = path;
        binary.replace_extension(".bin");
        const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"uri\":\"" + binary.filename().string()
                               + "\",\"byteLength\":" + std::to_string(bytes.size()) + "}],\"bufferViews\":[" + Join(views)
                               + "],\"accessors\":[" + Join(accessors) + "]," + body + "}";
        return Save(binary, bytes.data(), bytes.size()) && Save(path, json.data(), json.size());
    }
};
constexpr float    positions[]{0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0};
constexpr float    normals[]{0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
constexpr uint16_t indices[]{0, 1, 2, 2, 1, 3};
std::string        Attribute(const char* name, size_t accessor)
{
    return "\"" + std::string(name) + "\":" + std::to_string(accessor);
}
bool MakeFixture(const std::filesystem::path& path,
                 uint32_t                     copies,
                 uint32_t                     mode,
                 bool                         flat,
                 bool                         rich,
                 bool                         indexed   = true,
                 bool                         nonfinite = false)
{
    Fixture               fixture;
    std::array<float, 12> source;
    std::memcpy(source.data(), positions, sizeof(positions));
    if (nonfinite)
    {
        source[0] = std::numeric_limits<float>::infinity();
    }
    const size_t position = fixture.Add(source.data(), sizeof(source), 4, 5126, "VEC3");
    fixture.Bounds(position, "[0,0,0]", "[1,1,0]");
    const size_t   normal = fixture.Add(normals, sizeof(normals), 4, 5126, "VEC3");
    const uint16_t strip[]{0, 1, 2, 3};
    const size_t   index      = mode == 4 ? fixture.Add(indices, sizeof(indices), 6, 5123, "SCALAR")
                                          : fixture.Add(strip, sizeof(strip), 4, 5123, "SCALAR");
    std::string    attributes = Attribute("POSITION", position);
    if (!flat)
    {
        attributes += ',' + Attribute("NORMAL", normal);
    }
    std::string target;
    std::string extras;
    std::string node = "{\"mesh\":0}";
    if (rich)
    {
        const float    uv[]{0, 0, 1, 0, 0, 1, 1, 1};
        const float    weights[]{.125f, .125f, .125f, .125f, .125f, .125f, .125f, .125f,
                                 .125f, .125f, .125f, .125f, .125f, .125f, .125f, .125f};
        const uint16_t joints[16]{};
        const float    custom[]{1, 2, 3, 4};
        const float    delta[]{0, 0, .1f, 0, 0, .2f, 0, 0, .3f, 0, 0, .4f};
        const float    matrix[]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        const float    times[]{0, 1};
        const float    translations[]{0, 0, 0, 1, 0, 0};
        const size_t   uvId           = fixture.Add(uv, sizeof(uv), 4, 5126, "VEC2");
        const size_t   weightsId      = fixture.Add(weights, sizeof(weights), 4, 5126, "VEC4");
        const size_t   jointsId       = fixture.Add(joints, sizeof(joints), 4, 5123, "VEC4");
        const size_t   customId       = fixture.Add(custom, sizeof(custom), 4, 5126, "SCALAR");
        const size_t   deltaId        = fixture.Add(delta, sizeof(delta), 4, 5126, "VEC3");
        const size_t   matrixId       = fixture.Add(matrix, sizeof(matrix), 1, 5126, "MAT4");
        const size_t   timesId        = fixture.Add(times, sizeof(times), 2, 5126, "SCALAR");
        const size_t   translationsId = fixture.Add(translations, sizeof(translations), 2, 5126, "VEC3");
        fixture.Bounds(deltaId, "[0,0,0.1]", "[0,0,0.4]");
        fixture.Bounds(timesId, "[0]", "[1]");
        attributes += ',' + Attribute("TEXCOORD_0", uvId) + ',' + Attribute("TEXCOORD_2", uvId) + ','
                    + Attribute("JOINTS_0", jointsId) + ',' + Attribute("WEIGHTS_0", weightsId) + ','
                    + Attribute("JOINTS_1", jointsId) + ',' + Attribute("WEIGHTS_1", weightsId) + ','
                    + Attribute("_FEATURE", customId);
        target      = ",\"targets\":[{" + Attribute("POSITION", deltaId) + "}]";
        node        = "{\"mesh\":0,\"skin\":0,\"weights\":[0]},{}";
        extras      = ",\"skins\":[{\"joints\":[1],\"inverseBindMatrices\":" + std::to_string(matrixId)
                    + "}],\"animations\":[{\"samplers\":[{\"input\":" + std::to_string(timesId)
                    + ",\"output\":" + std::to_string(translationsId)
                    + "}],\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"translation\"}}]}]";
    }
    std::vector<std::string> primitives;
    for (uint32_t copy = 0; copy < copies; ++copy)
    {
        primitives.push_back("{\"attributes\":{" + attributes + "},\"mode\":" + std::to_string(mode)
                             + (indexed ? ",\"indices\":" + std::to_string(index) : "") + target + "}");
    }
    return fixture.Write(path, "\"meshes\":[{\"primitives\":[" + Join(primitives) + "]" + (rich ? ",\"weights\":[0]" : "")
                                   + "}],\"nodes\":[" + node + "],\"scenes\":[{\"nodes\":[0]}],\"scene\":0" + extras);
}
bool MakeMeshopt(const std::filesystem::path& path)
{
    Fixture              fixture;
    std::vector<uint8_t> encoded(meshopt_encodeVertexBufferBound(4, 12));
    const size_t         length = meshopt_encodeVertexBuffer(encoded.data(), encoded.size(), positions, 4, 12);
    const size_t         normal = fixture.Add(normals, sizeof(normals), 4, 5126, "VEC3");
    const size_t         index  = fixture.Add(indices, sizeof(indices), 6, 5123, "SCALAR");
    while (fixture.bytes.size() % 4 != 0)
    {
        fixture.bytes.push_back(0);
    }
    const size_t offset = fixture.bytes.size();
    fixture.bytes.insert(fixture.bytes.end(), encoded.begin(), encoded.begin() + length);
    const size_t view = fixture.views.size();
    fixture.views.push_back(
        "{\"buffer\":0,\"byteLength\":48,\"byteStride\":12,\"extensions\":{\"EXT_meshopt_compression\":{\"buffer\":0,\"byteOffset\":"
        + std::to_string(offset) + ",\"byteLength\":" + std::to_string(length)
        + ",\"byteStride\":12,\"count\":4,\"mode\":\"ATTRIBUTES\"}}}");
    const size_t position = fixture.accessors.size();
    fixture.accessors.push_back("{\"bufferView\":" + std::to_string(view)
                                + ",\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"}");
    fixture.Bounds(position, "[0,0,0]", "[1,1,0]");
    return length != 0
        && fixture.Write(
            path,
            "\"extensionsUsed\":[\"EXT_meshopt_compression\"],\"extensionsRequired\":[\"EXT_meshopt_compression\"],\"meshes\":[{\"primitives\":[{\"attributes\":{"
                + Attribute("POSITION", position) + ',' + Attribute("NORMAL", normal) + "},\"indices\":" + std::to_string(index)
                + "}]}],\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0");
}
bool MakeDraco(const std::filesystem::path& path)
{
    draco::TriangleSoupMeshBuilder builder;
    builder.Start(2);
    const int position = builder.AddAttribute(draco::GeometryAttribute::POSITION, 3, draco::DT_FLOAT32);
    const int normal   = builder.AddAttribute(draco::GeometryAttribute::NORMAL, 3, draco::DT_FLOAT32);
    builder.SetAttributeUniqueId(position, 0);
    builder.SetAttributeUniqueId(normal, 1);
    for (uint32_t face = 0; face < 2; ++face)
    {
        builder.SetAttributeValuesForFace(position, draco::FaceIndex(face), positions + indices[face * 3] * 3,
                                          positions + indices[face * 3 + 1] * 3, positions + indices[face * 3 + 2] * 3);
        builder.SetAttributeValuesForFace(normal, draco::FaceIndex(face), normals, normals, normals);
    }
    const std::unique_ptr<draco::Mesh> mesh = builder.Finalize();
    draco::Encoder                     encoder;
    draco::EncoderBuffer               encoded;
    const bool                         valid = mesh && encoder.EncodeMeshToBuffer(*mesh, &encoded).ok();
    Fixture                            fixture;
    if (valid)
    {
        fixture.AddView(encoded.data(), encoded.size());
        fixture.accessors = {"{\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"}",
                             "{\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"}",
                             "{\"componentType\":5125,\"count\":6,\"type\":\"SCALAR\"}"};
    }
    if (valid)
    {
        fixture.Bounds(0, "[0,0,0]", "[1,1,0]");
    }
    return valid
        && fixture.Write(
            path,
            "\"extensionsUsed\":[\"KHR_draco_mesh_compression\"],\"extensionsRequired\":[\"KHR_draco_mesh_compression\"],\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},\"indices\":2,\"extensions\":{\"KHR_draco_mesh_compression\":{\"bufferView\":0,\"attributes\":{\"POSITION\":0,\"NORMAL\":1}}}}]}],\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0");
}
SceneImportResult Import(const std::filesystem::path&        path,
                         uint64_t                            budget,
                         uint32_t                            workers = 1,
                         std::shared_ptr<SceneImportControl> control = {})
{
    SceneImportContext context;
    context.workerCount      = workers;
    context.maxGeometryBytes = budget;
    return ImportScene({path.string(), {}, std::move(control)}, context);
}
uint64_t Boundary(const std::filesystem::path& path, uint32_t workers, bool& valid)
{
    uint64_t                low      = 0;
    uint64_t                high     = 4ull << 20;
    const SceneImportResult baseline = Import(path, high, workers);
    valid &=
        Check(baseline.status == StatusCode::Success && baseline.asset, "Budget fixture must import with generous allowance");
    if (baseline.asset)
    {
        while (low < high)
        {
            const uint64_t          middle = low + (high - low) / 2;
            const SceneImportResult result = Import(path, middle, workers);
            if (result.status == StatusCode::Success)
            {
                high = middle;
            }
            else
            {
                valid &= Check(result.status == StatusCode::ResourceLimit && !result.asset
                                   && result.error.find("maxGeometryBytes") != std::string::npos,
                               "Only geometry-budget rejection is valid during admission search");
                low    = middle + 1;
            }
        }
        const SceneImportResult exact = Import(path, high, workers);
        const SceneImportResult below = Import(path, high - 1, workers);
        valid &= Check(high > 0 && exact.asset && exact.status == StatusCode::Success && !below.asset
                           && below.status == StatusCode::ResourceLimit,
                       "Exact declared admission succeeds; one byte below returns ResourceLimit without an asset");
        if (exact.asset)
        {
            valid &= Check(scene_test::Same(*baseline.asset, *exact.asset),
                           "Exact admitted quota preserves every canonical scene value");
        }
        std::printf("%s workers=%u requested-admission=%llu\n", path.filename().string().c_str(), workers,
                    static_cast<unsigned long long>(high));
    }
    else
    {
        std::fprintf(stderr, "%s: %s\n", path.string().c_str(), baseline.error.c_str());
    }
    return high;
}
struct CancelState
{
    std::atomic<uint32_t> polls{0};
    std::atomic<bool>     active{false};
    std::atomic<bool>     cancelled{false};
    std::thread::id       caller{std::this_thread::get_id()};
    uint32_t              limit{0};
};
void Stage(void* user, SceneImportStage stage)
{
    if (stage == SceneImportStage::ProcessingMeshes)
    {
        static_cast<CancelState*>(user)->active.store(true);
    }
}
bool Cancel(void* user)
{
    CancelState& state = *static_cast<CancelState*>(user);
    if (state.active.load() && std::this_thread::get_id() != state.caller && state.polls.fetch_add(1) >= state.limit)
    {
        state.cancelled.store(true);
    }
    return state.cancelled.load();
}
bool CheckCancellation(const std::filesystem::path& path)
{
    bool valid = true;
    for (uint32_t limit : {0u, 16u, 64u, 256u})
    {
        CancelState state;
        state.limit = limit;
        const std::shared_ptr<SceneImportControl> control =
            std::make_shared<SceneImportControl>(SceneImportCallbacks{&state, &Cancel, &Stage});
        const SceneImportResult result  = Import(path, 4ull << 20, 4, control);
        valid                          &= Check(result.status == StatusCode::Cancelled && !result.asset && state.active.load(),
                                                "Cancellation after mesh dispatch drains workers and publishes no candidate");
    }
    valid &=
        Check(Import(path, 4ull << 20, 4).asset != nullptr, "A fresh import succeeds after cancelled worker results drain");
    return valid;
}
bool ContainerLimit()
{
    GeometryBudget budget(UINT64_MAX);
    ImportErrors   errors;
    errors.Reset(nullptr, nullptr, &budget);
    std::vector<Vertex> vertices;
    return Check(!ReserveGeometryVector(vertices, vertices.max_size() + 1, errors)
                     && errors.Status() == StatusCode::ResourceLimit && vertices.empty() && budget.Used() == 0,
                 "Impossible vector capacity is rejected before reservation or allocation");
}
bool Batch(const std::filesystem::path& path, const std::filesystem::path& directory, uint64_t required)
{
    SceneBuildOptions options;
    options.outputDirectory = (directory / "batch").string();
    options.importContext   = {1, 1ull << 20, 1ull << 20, 1ull << 20, required};
    options.durable         = false;
    options.force           = true;
    const SceneBuildJob job{path.string(), "scene.zenasset"};
    const uint64_t      objReservation        = SceneBuildReservationBytes(options);
    options.importContext.maxGeometryBytes    = 2ull << 20;
    bool valid                                = Check(SceneBuildReservationBytes(options) == objReservation + (1ull << 20),
                                                      "Batch reserves common geometry when it is larger than OBJ allowance");
    options.importContext.maxObjGeometryBytes = 3ull << 20;
    valid &= Check(SceneBuildReservationBytes(options) == objReservation + (2ull << 20),
                   "Batch reserves OBJ when it is larger; common geometry is not double counted");
    options.importContext.maxGeometryBytes  = required;
    options.maxReservedMemoryBytes          = SceneBuildReservationBytes(options);
    SceneBuildResult result                 = BuildSceneBatch(std::span(&job, 1), options);
    valid                                  &= Check(result.status.Succeeded() && result.jobs[0].published,
                                                    "Batch admits exact geometry allowance and aggregate reservation");
    const std::filesystem::path output      = directory / "batch/scene.zenasset";
    std::ifstream               prior(output, std::ios::binary);
    const std::string           before{std::istreambuf_iterator<char>(prior), std::istreambuf_iterator<char>()};
    prior.close();
    options.importContext.maxGeometryBytes = required - 1;
    result                                 = BuildSceneBatch(std::span(&job, 1), options);
    std::ifstream     current(output, std::ios::binary);
    const std::string after{std::istreambuf_iterator<char>(current), std::istreambuf_iterator<char>()};
    valid &= Check(result.status.code == StatusCode::ResourceLimit && !result.jobs[0].published && before == after,
                   "Import geometry failure cannot replace a published batch artifact");
    options.importContext.maxGeometryBytes = required;
    options.maxReservedMemoryBytes         = SceneBuildReservationBytes(options) - 1;
    valid &= Check(BuildSceneBatch(std::span(&job, 1), options).status.code == StatusCode::ResourceLimit,
                   "Aggregate reservation rejects one-byte shortage before dispatch");
    return valid;
}
} // namespace
int main()
{
    const std::filesystem::path directory =
        std::filesystem::current_path()
        / ("geometry-budget-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code error;
    bool            valid  = std::filesystem::create_directory(directory, error) && !error;
    valid                 &= MakeFixture(directory / "base.gltf", 1, 4, false, false);
    valid                 &= MakeFixture(directory / "flat.gltf", 1, 4, true, false);
    valid                 &= MakeFixture(directory / "strip.gltf", 1, 5, false, false);
    valid                 &= MakeFixture(directory / "line.gltf", 1, 3, false, false);
    valid                 &= MakeFixture(directory / "rich.gltf", 1, 4, false, true);
    valid                 &= MakeFixture(directory / "many.gltf", 32, 4, false, true);
    valid                 &= MakeFixture(directory / "nonfinite.gltf", 1, 0, false, false, false, true);
    valid                 &= MakeMeshopt(directory / "meshopt.gltf");
    valid                 &= MakeDraco(directory / "draco.gltf");
    valid                 &= ContainerLimit();
    if (valid)
    {
        const uint64_t base  = Boundary(directory / "base.gltf", 1, valid);
        const uint64_t flat  = Boundary(directory / "flat.gltf", 1, valid);
        const uint64_t strip = Boundary(directory / "strip.gltf", 1, valid);
        Boundary(directory / "line.gltf", 1, valid);
        const uint64_t rich     = Boundary(directory / "rich.gltf", 1, valid);
        const uint64_t many     = Boundary(directory / "many.gltf", 4, valid);
        const uint64_t meshopt  = Boundary(directory / "meshopt.gltf", 1, valid);
        const uint64_t draco    = Boundary(directory / "draco.gltf", 1, valid);
        valid                  &= Check(
            flat > base && strip > base && rich > base && many > rich && meshopt > base && draco > base,
            "Expanded corners/topology, deformation, cumulative primitives and decoded codec buffers require additional admission");
        valid &= Check(Import(directory / "many.gltf", rich, 4).status == StatusCode::ResourceLimit,
                       "Shared source accessors do not evade cumulative primitive admission");
        valid &= Check(meshopt - base == sizeof(positions)
                           && draco - base == sizeof(positions) + sizeof(normals) + 6 * sizeof(uint32_t),
                       "Real codec inputs charge exactly their additional converted buffers");
        const SceneImportResult richScene = Import(directory / "rich.gltf", rich);
        valid &= Check(richScene.asset && !richScene.asset->skins.empty() && !richScene.asset->morphPrimitives.empty()
                           && !richScene.asset->extraVertexAttributes.empty() && !richScene.asset->animations.empty()
                           && richScene.asset->meshes[0].primitives[0].influences[0].size() == 8,
                       "Rich fixture actually retains skin, morph, custom, animation and all eight influences");
        const SceneImportResult invalid = Import(directory / "nonfinite.gltf", 4ull << 20);
        valid &=
            Check(!invalid.asset || invalid.asset->vertices.empty(), "Nonindexed allocation must not revive invalid positions");
        valid &= CheckCancellation(directory / "many.gltf");
        valid &= Batch(directory / "rich.gltf", directory, rich);
    }
    std::filesystem::remove_all(directory, error);
    valid &= Check(!error, "Fixture outputs cleaned after imports drain");
    std::printf("%u glTF geometry admission checks\n", checks);
    return valid ? 0 : 1;
}
