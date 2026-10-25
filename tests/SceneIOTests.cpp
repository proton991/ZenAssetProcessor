#include "ZenAsset/Processor/Import.h"
#include "ZenAsset/Processor/Write.h"
#include "ZenAsset/Processor/Cook.h"
#include "ZenAsset/Reader/Image.h"
#include "Processing/AtlasPacking.h"
#include "ZenAsset/Format/Validation.h"
#include "Format/PackageLayout.h"
#include "Format/SceneResources.h"
#include "SceneValueComparison.h"
#include <cstdio>
#include <chrono>
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
    scene.name           = "Rich canonical scene";
    scene.sourcePath     = "source-that-does-not-exist/model.gltf";
    scene.sourceDocument = "{\"asset\":{\"version\":\"2.0\"}}";
    scene.dependencies.push_back({"missing-texture.png", false, 0, -19, 23});
    scene.vertices.resize(3);
    scene.vertices[1].pos    = Vec4(1, 0, 0, 1);
    scene.vertices[2].pos    = Vec4(0, 1, 0, 1);
    scene.indices            = {0, 1, 2};
    scene.vertexTexCoordSets = {0, 1, 71};
    scene.vertexTexCoords.resize(3, {Vec2(0.25f, 0.5f), Vec2(0.75f, 1), Vec2(2, 3)});
    scene.meshes.resize(1);
    MeshPrimitiveAsset primitive;
    primitive.name           = "triangle";
    primitive.vertexCount    = 3;
    primitive.indexCount     = 3;
    primitive.material       = MaterialId{0};
    primitive.materialSlot   = 0;
    primitive.bounds         = Bounds(Vec3(0), Vec3(1, 1, 0));
    primitive.sourceVertices = {0, 1, 2};
    primitive.influences.resize(3, {{0, 1.0f}});
    scene.meshes[0].primitives.push_back(primitive);
    scene.meshes[0].morphWeights = {0.25f};
    scene.nodes.resize(2);
    scene.nodes[0].name         = "mesh node";
    scene.nodes[0].mesh         = MeshId{0};
    scene.nodes[0].materials    = {MaterialId{0}};
    scene.nodes[0].skin         = SkinId{0};
    scene.nodes[0].camera       = CameraId{0};
    scene.nodes[0].light        = LightId{0};
    scene.nodes[0].morphWeights = {0.75f};
    scene.nodes[0].selectable   = false;
    scene.nodes[1].name         = "joint";
    scene.roots                 = {NodeId{0}, NodeId{1}};
    scene.skins.push_back({"skin", 1, {1}, {Mat4(1)}});
    scene.materials.resize(1);
    scene.materials[0].name                            = "extended material";
    scene.materials[0].m_pBaseColorTexture             = TextureId{0};
    scene.materials[0].texCoordSets.baseColor          = 2;
    scene.materials[0].features.iridescence            = 0.625f;
    scene.materials[0].features.attenuationDistance    = 12.5f;
    scene.materials[0].textureTransforms[0].uvRotation = 0.4f;
    std::shared_ptr<ImagePixels> pixels                = std::make_shared<ImagePixels>();
    pixels->width                                      = 2;
    pixels->height                                     = 2;
    pixels->bytes                                      = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    pixels->mips                                       = {pixels->bytes, {127, 127, 127, 255}};
    scene.images.push_back({"pixels", pixels});
    scene.samplers.resize(1);
    scene.samplers[0].wrapS = SamplerAddressMode::MirroredRepeat;
    scene.textures.push_back({"color", ImageId{0}, SamplerId{0}, {}, PixelFormat::RGBA8Srgb, false});
    MorphPrimitiveAsset morph;
    morph.weights = {0.25f};
    morph.targets.resize(1);
    morph.targets[0].positions.resize(3, Vec3(0, 0, 0.5f));
    morph.targets[0].texCoords.resize(3, std::vector<Vec2>(3, Vec2(0.1f, 0.2f)));
    morph.targets[0].extraColors.push_back({9, std::vector<Vec4>(3, Vec4(0.125f))});
    scene.morphPrimitives.push_back(morph);
    scene.animations.resize(1);
    scene.animations[0].name = "weight and material";
    scene.animations[0].samplers.push_back({AnimationInterpolation::Linear, 1, {0, 2}, {0.25f, 0.75f}});
    scene.animations[0].channels.push_back({0, 0, AnimationPath::Weights, {}, "/nodes/0/weights"});
    scene.animations[0].channels.push_back({0,
                                            UINT32_MAX,
                                            AnimationPath::Property,
                                            {AnimatedObject::Material, 0, AnimatedProperty::Metallic, 1},
                                            "/materials/0/pbrMetallicRoughness/metallicFactor"});
    scene.cameras.resize(1);
    scene.cameras[0].name        = "camera";
    scene.cameras[0].infiniteFar = true;
    scene.lights.resize(1);
    scene.lights[0].kind      = LightKind::Spot;
    scene.lights[0].intensity = 7.25f;
    scene.materialVariants    = {"original", "alternative"};
    scene.variantPrimitives.push_back({0, 0, {-1, 0}});
    scene.extraVertexAttributes.push_back({0,
                                           0,
                                           "_EXACT_INTEGER",
                                           -1,
                                           1,
                                           5125,
                                           false,
                                           VertexAttributeDomain::SourceVertex,
                                           {4294967295.0, 16777217.0, 123456789.0}});
    scene.imageBasedLight = 0;
    scene.imageBasedLights.resize(1);
    scene.imageBasedLights[0].name = "environment";
    scene.imageBasedLights[0].irradianceCoefficients.resize(9, Vec3(0.25f));
    std::shared_ptr<EnvironmentCubePixels> cube = std::make_shared<EnvironmentCubePixels>();
    cube->size                                  = 1;
    cube->mipLevels                             = 1;
    cube->rgba32FloatMipFaces.resize(6, std::vector<Vec4>(1, Vec4(4, 2, 1, 1)));
    scene.imageBasedLights[0].specular = std::move(cube);
    return scene;
}

std::vector<uint8_t> LoadBytes(const std::filesystem::path& path)
{
    std::ifstream        input(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return bytes;
}
bool SaveBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return bool(output);
}
size_t EntryOffset(const std::vector<uint8_t>& bytes, ResourceType type)
{
    size_t         offset = 0;
    const uint64_t count  = detail::Read64(bytes.data() + 32);
    for (uint64_t index = 0; index < count; ++index)
    {
        const size_t current = detail::HeaderBytes + static_cast<size_t>(index) * detail::DirectoryEntryBytes;
        if (detail::Read32(bytes.data() + current + 8) == static_cast<uint32_t>(type))
        {
            offset = current;
        }
    }
    return offset;
}
void RepairChecksums(std::vector<uint8_t>& bytes, size_t entry)
{
    const uint64_t offset = detail::Read64(bytes.data() + entry + 24);
    const uint64_t size   = detail::Read64(bytes.data() + entry + 32);
    detail::Write64(bytes.data() + entry + 48, detail::HashBytes(bytes.data() + offset, size));
    const uint64_t count = detail::Read64(bytes.data() + 32);
    detail::Write64(bytes.data() + 48,
                    detail::HashBytes(bytes.data() + detail::HeaderBytes, count * detail::DirectoryEntryBytes));
    detail::Write64(bytes.data() + 56, detail::HashBytes(bytes.data(), 56));
}
void AppendOptionalResource(std::vector<uint8_t>& bytes)
{
    const uint64_t count  = detail::Read64(bytes.data() + 32);
    const size_t   prefix = detail::HeaderBytes + static_cast<size_t>(count) * detail::DirectoryEntryBytes;
    bytes.insert(bytes.begin() + prefix, detail::DirectoryEntryBytes, 0);
    for (size_t index = 0; index < count; ++index)
    {
        const size_t entry = detail::HeaderBytes + index * detail::DirectoryEntryBytes;
        detail::Write64(bytes.data() + entry + 24, detail::Read64(bytes.data() + entry + 24) + detail::DirectoryEntryBytes);
    }
    bytes.resize((bytes.size() + 15) & ~size_t(15), 0);
    const uint64_t payload = bytes.size();
    bytes.push_back(73);
    detail::Write64(bytes.data() + prefix, detail::ResourceId(ResourceType::SourceDocument, 1));
    detail::Write32(bytes.data() + prefix + 8, static_cast<uint32_t>(ResourceType::SourceDocument));
    detail::Write32(bytes.data() + prefix + 12, 999);
    detail::Write64(bytes.data() + prefix + 24, payload);
    detail::Write64(bytes.data() + prefix + 32, 1);
    detail::Write64(bytes.data() + prefix + 40, 1);
    detail::Write32(bytes.data() + prefix + 56, 16);
    detail::Write32(bytes.data() + prefix + 60, 888);
    detail::Write64(bytes.data() + 32, count + 1);
    detail::Write64(bytes.data() + 40, bytes.size());
    RepairChecksums(bytes, prefix);
}
struct PollCounter
{
    uint32_t    calls{0};
    static bool Cancel(void* user)
    {
        PollCounter& counter = *static_cast<PollCounter*>(user);
        ++counter.calls;
        return counter.calls >= 40;
    }
};
bool RejectMutated(const std::filesystem::path& path, const std::vector<uint8_t>& bytes, const char* message)
{
    bool                  valid   = Check(SaveBytes(path, bytes), "Could not write corrupt fixture");
    const SceneReadResult result  = ReadScene(path.string());
    valid                        &= Check(!result.status.Succeeded() && !result.scene, message);
    return valid;
}

bool CheckContents(const Scene& decoded, const Scene& expected)
{
    bool valid  = Check(decoded.name == expected.name && decoded.sourcePath == expected.sourcePath
                            && decoded.sourceDocument == expected.sourceDocument,
                        "Scene identity and provenance must round-trip without opening sources");
    valid      &= Check(decoded.nodes.size() == 2 && !decoded.nodes[0].selectable && decoded.nodes[0].morphWeights[0] == 0.75f,
                        "Instance state must round-trip");
    valid      &= Check(decoded.vertexTexCoordSets == expected.vertexTexCoordSets && decoded.vertexTexCoords[2][2] == Vec2(2, 3)
                            && decoded.meshes[0].primitives[0].influences[2][0].weight == 1.0f,
                        "Geometry streams and full influences must round-trip");
    valid      &= Check(decoded.images[0].pixels->bytes == expected.images[0].pixels->bytes
                            && decoded.images[0].pixels->mips[1] == expected.images[0].pixels->mips[1],
                        "Pixels and authored mips must round-trip");
    valid      &= Check(decoded.materials[0].features.iridescence == 0.625f
                            && decoded.materials[0].textureTransforms[0].uvRotation == 0.4f,
                        "Extended materials and transforms must round-trip");
    valid &=
        Check(decoded.skins[0].joints[0] == 1 && decoded.animations[0].channels[1].target.property == AnimatedProperty::Metallic
                  && decoded.animations[0].samplers[0].times[1] == 2,
              "Skin and typed animation targets must round-trip");
    valid &= Check(decoded.morphPrimitives[0].targets[0].extraColors[0].set == 9
                       && decoded.extraVertexAttributes[0].values[0] == 4294967295.0,
                   "Morph attributes and exact custom integers must round-trip");
    valid &= Check(decoded.cameras[0].infiniteFar && decoded.lights[0].intensity == 7.25f
                       && decoded.imageBasedLights[0].specular->rgba32FloatMipFaces[5][0].x == 4,
                   "Camera, light and environment payloads must round-trip");
    return valid;
}

bool TestRoundTrip(const std::filesystem::path& directory)
{
    const std::filesystem::path path  = directory / "rich.zenasset";
    const std::filesystem::path copy  = directory / "copy.zenasset";
    const std::filesystem::path bad   = directory / "invalid.zenasset";
    Scene                       scene = MakeScene();
    std::string                 failure;
    bool                        valid = Check(ValidateSceneAsset(scene, 1ull << 30, &failure), failure.c_str());
    SceneWriteOptions           options;
    options.metadata.sourceFormat        = "synthetic";
    options.metadata.buildFingerprint    = "fixed-test-build";
    options.metadata.cacheKey            = "feed0123456789ab";
    options.metadata.settings.sceneIndex = 3;
    options.metadata.inputs.push_back({scene.sourcePath, true, 99, -137, 0xfedcba9876543210});
    options.metadata.diagnostics.push_back(
        {DiagnosticSeverity::Warning, StatusCode::UnsupportedFeature, "source", "object", "retained warning"});
    const Status written                 = WriteScene(path.string(), scene, options);
    valid                               &= Check(written.Succeeded(), written.message.c_str());
    const std::vector<uint8_t> original  = LoadBytes(path);
    if (!original.empty())
    {
        const SceneReadResult read          = ReadScene(path.string());
        valid                              &= Check(read.status.Succeeded() && read.scene, read.status.message.c_str());
        const SceneMetadataResult metadata  = ReadSceneMetadata(path.string());
        valid &= Check(metadata.status.Succeeded() && metadata.metadata.cacheKey == options.metadata.cacheKey
                           && metadata.sourcePath == scene.sourcePath && metadata.dependencies.size() == 1
                           && metadata.dependencies[0].modified == -19 && metadata.metadata.inputs.size() == 1
                           && metadata.metadata.inputs[0].contentHash == 0xfedcba9876543210,
                       "Metadata-only reads must preserve build identity and source revisions");
        if (read.scene)
        {
            valid &= Check(scene_test::Same(scene, *read.scene),
                           "All public scene field values must round-trip independently of the binary schema");
            valid &= CheckContents(*read.scene, scene);
            valid &= Check(read.metadata.cacheKey == options.metadata.cacheKey
                               && read.metadata.diagnostics[0].message == "retained warning"
                               && read.metadata.settings.sceneIndex == 3,
                           "Build identity, settings and diagnostics must round-trip");
            SceneWriteOptions copied;
            copied.metadata  = read.metadata;
            valid           &= Check(WriteScene(copy.string(), *read.scene, copied).Succeeded() && LoadBytes(copy) == original,
                                     "Reading and rewriting all fields must preserve deterministic bytes");
        }
        const PackageResult retained  = OpenPackage(path.string());
        valid                        &= Check(retained.status.Succeeded(), "Package directory must open");
        if (retained.package)
        {
            SceneReadLimits limits;
            limits.package.maxDecodedBytes = 1;
            valid                     &= Check(ReadScene(*retained.package, limits).status.code == StatusCode::ResourceLimit,
                                               "Retained packages must honor stricter aggregate transport limits");
            limits                     = {};
            limits.maxAllocationBytes  = sizeof(Scene) + retained.package->Resources().size() * sizeof(ResourceRecord);
            valid                     &= Check(ReadScene(*retained.package, limits).status.code == StatusCode::ResourceLimit,
                                               "Allocation budget must include directory, resource buffer and decoded output");
            Scene changed              = scene;
            changed.name               = "atomic replacement";
            valid                     &= Check(WriteScene(path.string(), changed, options).Succeeded(),
                                               "Atomic replacement with retained reader must succeed");
            const SceneReadResult old  = ReadScene(*retained.package);
            const SceneReadResult current = ReadScene(path.string());
            valid &= Check(old.scene && current.scene && old.scene->name == scene.name && current.scene->name == changed.name,
                           "A retained reader must preserve the old package after atomic publication");
        }
        const std::vector<uint8_t> published = LoadBytes(path);
        Scene                      invalid   = scene;
        invalid.nodes[0].mesh                = MeshId{99};
        valid &= Check(!WriteScene(path.string(), invalid, options).Succeeded() && LoadBytes(path) == published,
                       "Invalid writes must preserve an existing package");
        SceneWriteOptions limited         = options;
        limited.limits.maxAllocationBytes = 1;
        valid &=
            Check(WriteScene(path.string(), scene, limited).code == StatusCode::ResourceLimit && LoadBytes(path) == published,
                  "Writer allocations must obey the aggregate budget before creating output");
        limited                           = options;
        limited.limits.maxAllocationBytes = detail::Read64(original.data() + 32) * sizeof(ResourceRecord) + 8;
        valid &=
            Check(WriteScene(path.string(), scene, limited).code == StatusCode::ResourceLimit && LoadBytes(path) == published,
                  "Directory storage and the exact-sized chunk buffer must share one allocation budget");
        limited                              = options;
        limited.limits.package.maxChunkBytes = 1;
        valid &=
            Check(WriteScene(path.string(), scene, limited).code == StatusCode::ResourceLimit && LoadBytes(path) == published,
                  "Mid-write limits must preserve an existing package");
        Cancellation cancelled;
        cancelled.Cancel();
        valid &= Check(WriteScene(path.string(), scene, options, &cancelled).code == StatusCode::Cancelled
                           && LoadBytes(path) == published,
                       "Cancelled writes must preserve an existing package");
        valid &= Check(ReadScene(path.string(), {}, &cancelled).status.code == StatusCode::Cancelled,
                       "Cancelled reads must publish no scene");
        PollCounter        counter;
        const Cancellation duringWrite(PollCounter::Cancel, &counter);
        valid &= Check(WriteScene(path.string(), scene, options, &duringWrite).code == StatusCode::Cancelled
                           && counter.calls >= 40 && LoadBytes(path) == published,
                       "Cancellation during serialization must preserve old output");
        SceneReadLimits limitedRead;
        limitedRead.maxSourceDocumentBytes  = 1;
        valid                              &= Check(ReadSceneMetadata(path.string(), limitedRead).status.Succeeded()
                                                        && !ReadScene(path.string(), limitedRead).scene,
                                                    "Metadata lookup must not load the separate source document");
        limitedRead                         = {};
        limitedRead.maxArrayElements        = 1;
        valid &= Check(!ReadScene(path.string(), limitedRead).scene, "Nested array counts must obey limits");
        std::vector<uint8_t> bytes = original;
        bytes.pop_back();
        valid                     &= RejectMutated(bad, bytes, "Truncated packages must fail");
        bytes                      = original;
        const size_t vertexEntry   = EntryOffset(bytes, ResourceType::VertexStream);
        const size_t vertexOffset  = static_cast<size_t>(detail::Read64(bytes.data() + vertexEntry + 24));
        detail::Write32(bytes.data() + vertexOffset + 8, 0x7fc00000);
        valid &=
            Check(SaveBytes(bad, bytes) && ReadSceneMetadata(bad.string()).status.Succeeded() && !ReadScene(bad.string()).scene,
                  "Metadata-only reads must avoid unrelated corrupt runtime payloads");
        RepairChecksums(bytes, vertexEntry);
        valid &= RejectMutated(bad, bytes, "Valid checksums must not permit nonfinite geometry");
        bytes  = original;
        detail::Write64(bytes.data() + vertexOffset, UINT64_MAX);
        RepairChecksums(bytes, vertexEntry);
        valid                    &= RejectMutated(bad, bytes, "Oversized nested counts must fail before allocation");
        bytes                     = original;
        const size_t sceneEntry   = EntryOffset(bytes, ResourceType::Scene);
        const size_t sceneOffset  = static_cast<size_t>(detail::Read64(bytes.data() + sceneEntry + 24));
        bytes[sceneOffset + 8]    = 0xff;
        RepairChecksums(bytes, sceneEntry);
        valid                     &= RejectMutated(bad, bytes, "Malformed UTF-8 must fail before scene publication");
        bytes                      = original;
        const uint64_t sceneBytes  = detail::Read64(bytes.data() + sceneEntry + 32);
        const size_t   countsOffset =
            sceneOffset + static_cast<size_t>(sceneBytes) - detail::ResourceTypeCount * sizeof(uint64_t);
        detail::Write64(bytes.data() + countsOffset + static_cast<size_t>(ResourceType::Mesh) * 8, 2);
        RepairChecksums(bytes, sceneEntry);
        valid                   &= RejectMutated(bad, bytes, "Manifest references must agree with directory resources");
        bytes                    = original;
        const size_t imageEntry  = EntryOffset(bytes, ResourceType::Image);
        detail::Write32(bytes.data() + imageEntry + 16, 0);
        RepairChecksums(bytes, imageEntry);
        valid &= RejectMutated(bad, bytes, "Referenced runtime records cannot be marked optional");
        bytes  = original;
        AppendOptionalResource(bytes);
        valid &= Check(SaveBytes(bad, bytes) && ReadScene(bad.string()).status.Succeeded(),
                       "Unknown optional versions of known resource types must be skipped");
    }
    return valid;
}

bool TestBlockModeValidation(const std::filesystem::path& directory)
{
    Scene                        scene;
    std::shared_ptr<ImagePixels> pixels = std::make_shared<ImagePixels>();
    pixels->width                       = 5;
    pixels->height                      = 7;
    pixels->format                      = PixelFormat::BC7Srgb;
    pixels->bytes.resize(64, 0x40);
    pixels->mips = {pixels->bytes, std::vector<uint8_t>(16, 0x40), std::vector<uint8_t>(16, 0x40)};
    scene.images.push_back({"blocks", pixels});
    const std::filesystem::path path    = directory / "valid-bc7.zenasset";
    const std::filesystem::path bad     = directory / "reserved-bc7.zenasset";
    const Status                written = WriteScene(path.string(), scene);
    bool                        valid   = Check(written.Succeeded(), "Valid BC7 package can be written");
    if (written.Succeeded())
    {
        const SceneReadResult read  = ReadScene(path.string());
        valid                      &= Check(read.status.Succeeded() && read.scene && scene_test::Same(scene, *read.scene),
                                            "Valid BC7 package retains all block bytes");
        const std::vector<uint8_t> original  = LoadBytes(path);
        const size_t               entry     = EntryOffset(original, ResourceType::Image);
        const size_t               payload   = static_cast<size_t>(detail::Read64(original.data() + entry + 24));
        const size_t               nameBytes = static_cast<size_t>(detail::Read64(original.data() + payload));
        const size_t               baseCount = payload + 8 + nameBytes + 12;
        const size_t               base      = baseCount + 8;
        const size_t               baseBytes = static_cast<size_t>(detail::Read64(original.data() + baseCount));
        size_t                     cursor    = base + baseBytes + 8; // mip vector count.
        std::array<size_t, 3>      mips{};
        for (size_t level = 0; level < mips.size(); ++level)
        {
            const size_t count = static_cast<size_t>(detail::Read64(original.data() + cursor));
            mips[level]        = cursor + 8;
            cursor             = mips[level] + count;
        }
        std::vector<uint8_t> changed = original;
        changed[base + 16] = changed[mips[0] + 16] = 0;
        RepairChecksums(changed, entry);
        valid                    &= Check(SaveBytes(bad, changed), "Can write reserved nonfirst block fixture");
        SceneReadResult rejected  = ReadScene(bad.string());
        valid                    &= Check(rejected.status.code == StatusCode::InvalidData && !rejected.scene,
                                          "Valid-checksum package rejects a nonfirst reserved BC7 block");
        changed                   = original;
        changed[mips[2]]          = 0;
        RepairChecksums(changed, entry);
        valid    &= Check(SaveBytes(bad, changed), "Can write reserved tiny mip fixture");
        rejected  = ReadScene(bad.string());
        valid    &= Check(rejected.status.code == StatusCode::InvalidData && !rejected.scene,
                          "Valid-checksum package rejects a reserved tiny BC7 mip");
    }
    pixels->width = pixels->height = 1;
    pixels->format                 = PixelFormat::RGBA8Unorm;
    pixels->bytes.assign(4, 0);
    pixels->mips.clear();
    const std::filesystem::path rgbaPath    = directory / "zero-rgba.zenasset";
    const Status                rgbaWritten = WriteScene(rgbaPath.string(), scene);
    valid &=
        Check(rgbaWritten.Succeeded() && ReadScene(rgbaPath.string()).status.Succeeded(), "Zero RGBA payload remains readable");
    return valid;
}

bool TestEnvironmentEncoding(const std::filesystem::path& directory)
{
    Scene scene;
    scene.imageBasedLight = 0;
    scene.imageBasedLights.resize(1);
    ImageBasedLightAsset& light = scene.imageBasedLights[0];
    light.name                  = "authored HDR";
    light.irradianceCoefficients.assign(9, Vec3(0.25f));
    std::shared_ptr<EnvironmentCubePixels> cube = std::make_shared<EnvironmentCubePixels>();
    cube->size                                  = 5;
    cube->mipLevels                             = 2;
    for (uint32_t face = 0; face < 12; ++face)
    {
        const uint32_t extent = face < 6 ? 5 : 2;
        cube->rgba32FloatMipFaces.emplace_back(extent * extent, Vec4(float(face) + 0.125f, -2.0f, 65505.0f, 0.5f));
    }
    light.specular                   = cube;
    const std::filesystem::path path = directory / "environment-version.zenasset";
    bool valid = Check(WriteScene(path.string(), scene).Succeeded(), "Typed float environment version two writes");
    if (valid)
    {
        const SceneReadResult current  = ReadScene(path.string());
        valid                         &= Check(current.scene && scene_test::Same(scene, *current.scene),
                                               "Float environment preserves HDR range, sign, alpha and authored partial mips");
        std::vector<uint8_t> bytes     = LoadBytes(path);
        const size_t         entry     = EntryOffset(bytes, ResourceType::Environment);
        const size_t         payload   = static_cast<size_t>(detail::Read64(bytes.data() + entry + 24));
        const size_t         count     = static_cast<size_t>(detail::Read64(bytes.data() + entry + 32));
        const size_t         formatOffset = payload + 8 + light.name.size() + 4 + 16 + 8 + 9 * 12;
        valid &= Check(detail::Read32(bytes.data() + entry + 12) == 2, "Environment resource uses version two");
        // Version one stored size/mips/typed floats directly after the light fields.
        // Keep later resource offsets intact; unused alignment bytes are permitted.
        std::move(bytes.begin() + formatOffset + 4, bytes.begin() + payload + count, bytes.begin() + formatOffset);
        detail::Write32(bytes.data() + entry + 12, 1);
        detail::Write64(bytes.data() + entry + 32, count - 4);
        detail::Write64(bytes.data() + entry + 40, count - 4);
        RepairChecksums(bytes, entry);
        valid                        &= Check(SaveBytes(path, bytes), "Legacy environment fixture writes");
        const SceneReadResult legacy  = ReadScene(path.string());
        valid                        &= Check(legacy.scene && scene_test::Same(scene, *legacy.scene),
                                              "Environment version one retains exact typed float data and all light fields");
        detail::Write32(bytes.data() + formatOffset + 8 + 8 + 8, 0x7fc00000);
        RepairChecksums(bytes, entry);
        valid &= RejectMutated(path, bytes, "Legacy environment rejects nonfinite pixels with valid checksums");
    }
    cube->format = PixelFormat::BC6HUfloat;
    cube->rgba32FloatMipFaces.clear();
    for (uint32_t face = 0; face < 12; ++face)
    {
        cube->blockMipFaces.emplace_back(face < 6 ? 64 : 16, 0);
    }
    const Status written  = WriteScene(path.string(), scene);
    valid                &= Check(written.Succeeded(), "BC6H odd-sized faces and tiny authored mips write");
    if (written.Succeeded())
    {
        const SceneReadResult read  = ReadScene(path.string());
        valid                      &= Check(read.scene && scene_test::Same(scene, *read.scene)
                                                && read.scene->imageBasedLights[0].specular->rgba32FloatMipFaces.empty(),
                                            "BC6H resource roundtrip retains blocks without a decoded float copy");
        const std::vector<uint8_t> original     = LoadBytes(path);
        const size_t               entry        = EntryOffset(original, ResourceType::Environment);
        const size_t               payload      = static_cast<size_t>(detail::Read64(original.data() + entry + 24));
        const size_t               formatOffset = payload + 8 + light.name.size() + 4 + 16 + 8 + 9 * 12;
        const size_t               firstFace    = formatOffset + 4 + 8 + 8 + 8;
        for (uint8_t reserved : {19, 23, 27, 31})
        {
            std::vector<uint8_t> bytes = original;
            bytes[firstFace + 16]      = reserved;
            RepairChecksums(bytes, entry);
            valid &= RejectMutated(path, bytes, "Reserved BC6H mode in a nonfirst block is rejected");
        }
        std::vector<uint8_t> bytes     = original;
        const size_t         finalFace = firstFace + 6 * (64 + 8) + 5 * (16 + 8);
        bytes[finalFace]               = 31;
        RepairChecksums(bytes, entry);
        valid &= RejectMutated(path, bytes, "Reserved BC6H mode in the final tiny mip face is rejected");
        for (uint32_t format : {static_cast<uint32_t>(PixelFormat::BC6HSfloat), UINT32_MAX})
        {
            bytes = original;
            detail::Write32(bytes.data() + formatOffset, format);
            RepairChecksums(bytes, entry);
            valid                          &= Check(SaveBytes(path, bytes), "Unsupported environment format fixture writes");
            const SceneReadResult rejected  = ReadScene(path.string());
            valid                          &= Check(!rejected.scene && rejected.status.code == StatusCode::UnsupportedFeature,
                                                    "Unsupported environment formats fail before interpreting payload arrays");
        }
        bytes = original;
        detail::Write64(bytes.data() + firstFace - 8, 63);
        RepairChecksums(bytes, entry);
        valid &= RejectMutated(path, bytes, "Malformed environment face length is rejected");
    }
    cube->rgba32FloatMipFaces.emplace_back(1, Vec4(1));
    valid &= Check(WriteScene(path.string(), scene).code == StatusCode::InvalidData,
                   "A cube cannot retain both float and block payloads");
    cube->rgba32FloatMipFaces.clear();
    cube->blockMipFaces.pop_back();
    valid &=
        Check(WriteScene(path.string(), scene).code == StatusCode::InvalidData, "Incomplete cubemap face sets are rejected");
    return valid;
}

bool TestTextureInterpretation(const std::filesystem::path& directory)
{
    Scene                        scene;
    std::shared_ptr<ImagePixels> pixels = std::make_shared<ImagePixels>();
    pixels->width = pixels->height = 1;
    pixels->bytes                  = {128, 128, 255, 255};
    scene.images.push_back({"texture", pixels});
    TextureAsset texture;
    texture.image = ImageId{0};
    scene.textures.push_back(texture);
    const std::filesystem::path path  = directory / "texture-version.zenasset";
    bool                        valid = Check(WriteScene(path.string(), scene).Succeeded(), "Current texture resource writes");
    if (valid)
    {
        std::vector<uint8_t> bytes = LoadBytes(path);
        const size_t         entry = EntryOffset(bytes, ResourceType::Texture);
        valid &= Check(detail::Read32(bytes.data() + entry + 12) == 2, "Texture interpretation uses resource version two");
        const uint64_t count = detail::Read64(bytes.data() + entry + 32);
        detail::Write32(bytes.data() + entry + 12, 1);
        detail::Write64(bytes.data() + entry + 32, count - 4);
        detail::Write64(bytes.data() + entry + 40, count - 4);
        RepairChecksums(bytes, entry);
        valid                      &= Check(SaveBytes(path, bytes), "Legacy texture fixture writes");
        const SceneReadResult read  = ReadScene(path.string());
        valid                      &= Check(read.scene && scene_test::Same(scene, *read.scene),
                                            "Texture version one retains default interpretation and exact fields");
    }
    for (PixelFormat format : {PixelFormat::BC4Unorm, PixelFormat::BC5Unorm})
    {
        pixels->format = format;
        pixels->width  = 5;
        pixels->height = 7;
        pixels->bytes.assign(format == PixelFormat::BC4Unorm ? 32 : 64, 0);
        pixels->mips             = {pixels->bytes, std::vector<uint8_t>(format == PixelFormat::BC4Unorm ? 8 : 16, 0)};
        scene.textures[0].format = format;
        scene.textures[0].interpretation =
            format == PixelFormat::BC5Unorm ? TextureInterpretation::NormalXYPositiveZ : TextureInterpretation::Default;
        SceneWriteOptions options;
        options.metadata.profile                                  = "desktop";
        options.metadata.processing.textures.compressBC5Normals   = format == PixelFormat::BC5Unorm;
        options.metadata.processing.textures.compressBC4Occlusion = format == PixelFormat::BC4Unorm;
        const Status written                                      = WriteScene(path.string(), scene, options);
        valid &= Check(written.Succeeded(), "BC4/BC5 odd base and tiny mip writes");
        if (written.Succeeded())
        {
            const SceneReadResult read   = ReadScene(path.string());
            valid                       &= Check(read.scene && scene_test::Same(scene, *read.scene),
                                                 "BC4/BC5 payload and interpretation round trip exactly");
            valid                       &= Check(read.metadata.processing == options.metadata.processing,
                                                 "Metadata version six retains active channel compression settings");
            std::vector<uint8_t> bytes   = LoadBytes(path);
            const size_t         entry   = EntryOffset(bytes, ResourceType::Texture);
            const size_t         offset  = static_cast<size_t>(detail::Read64(bytes.data() + entry + 24));
            const size_t         count   = static_cast<size_t>(detail::Read64(bytes.data() + entry + 32));
            detail::Write32(bytes.data() + offset + count - 4, UINT32_MAX);
            RepairChecksums(bytes, entry);
            valid                          &= Check(SaveBytes(path, bytes), "Unknown texture interpretation fixture writes");
            const SceneReadResult rejected  = ReadScene(path.string());
            valid                          &= Check(!rejected.scene && rejected.status.code == StatusCode::InvalidData,
                                                    "Valid-checksum package rejects unknown texture interpretation");
        }
        scene.textures[0].interpretation =
            format == PixelFormat::BC5Unorm ? TextureInterpretation::Default : TextureInterpretation::NormalXYPositiveZ;
        valid &= Check(!ValidateSceneAsset(scene), "Texture format and normal interpretation must agree");
    }
    return valid;
}

bool TestSpatialCorruption(const std::filesystem::path& directory, const Scene& scene)
{
    const std::filesystem::path path     = directory / "spatial-raw.zenasset";
    const Status                written  = WriteScene(path.string(), scene);
    bool                        valid    = Check(written.Succeeded(), "Raw spatial baseline writes before corruption checks");
    const std::vector<uint8_t>  baseline = written.Succeeded() ? LoadBytes(path) : std::vector<uint8_t>{};
    valid &= Check(baseline.size() >= detail::HeaderBytes, "Raw spatial baseline is available before mutation");
    if (valid)
    {
        const size_t entry  = EntryOffset(baseline, ResourceType::Texture);
        const size_t offset = static_cast<size_t>(detail::Read64(baseline.data() + entry + 24));
        const size_t count  = static_cast<size_t>(detail::Read64(baseline.data() + entry + 32));
        for (uint32_t field = 0; field < 4; ++field)
        {
            for (uint32_t value : {0u, 3u, UINT32_MAX})
            {
                std::vector<uint8_t> bytes = baseline;
                detail::Write32(bytes.data() + offset + count - 16 + field * 4, value);
                RepairChecksums(bytes, entry);
                const std::filesystem::path bad  = directory / "invalid-spatial.zenasset";
                valid                           &= Check(SaveBytes(bad, bytes), "Mutated spatial fixture writes");
                const PackageResult framing      = OpenPackage(bad.string());
                valid                          &= Check(framing.package && framing.package->ValidatePayloads().Succeeded(),
                                                        "Malformed spatial fields retain valid framing and resource checksums");
                const SceneReadResult rejected  = ReadScene(bad.string());
                valid &= Check(rejected.status.code == StatusCode::InvalidData && !rejected.scene,
                               "Malformed spatial bounds, alignment and extents fail before scene publication");
            }
        }
        for (uint32_t version : {3u, 5u})
        {
            std::vector<uint8_t> bytes = baseline;
            detail::Write32(bytes.data() + entry + 12, version);
            RepairChecksums(bytes, entry);
            valid &= RejectMutated(directory / "invalid-spatial-version.zenasset", bytes,
                                   "Unknown schema and older schema with trailing spatial fields cannot silently drop regions");
        }
        Scene invalid                     = scene;
        invalid.textures[0].atlasRegion.x = UINT32_MAX;
        valid &= Check(!WriteScene(path.string(), invalid).Succeeded() && LoadBytes(path) == baseline,
                       "Invalid spatial write preserves the existing destination");
    }
    return valid;
}

bool TestSpatialRegions(const std::filesystem::path& directory)
{
    std::vector<detail::AtlasTileInput> inputs;
    for (uint32_t index = 0; index < 3; ++index)
    {
        std::shared_ptr<ImagePixels> pixels = std::make_shared<ImagePixels>();
        pixels->width                       = index == 0 ? 8 : index == 1 ? 16 : 4;
        pixels->height                      = index == 0 ? 16 : index == 1 ? 8 : 4;
        pixels->bytes.resize(size_t(pixels->width) * pixels->height * 4);
        for (size_t byte = 0; byte < pixels->bytes.size(); ++byte)
        {
            pixels->bytes[byte] = static_cast<uint8_t>((byte * 17 + index * 79) % 256);
        }
        pixels->mips.push_back(pixels->bytes);
        inputs.push_back({index, 0, pixels});
    }
    detail::AtlasPackingOptions packing;
    packing.maxPageDimension                = 64;
    packing.powerOfTwoPages                 = true;
    uint64_t                      remaining = 1ull << 20;
    const detail::AtlasPackResult packed    = detail::PackAtlasTiles(inputs, packing, remaining);
    bool                          valid = Check(packed.status.Succeeded(), "Spatial fixture uses actual bounded packer output");
    Scene                         scene;
    if (packed.status.Succeeded())
    {
        scene.name       = "spatial region package";
        scene.sourcePath = "source-is-not-present.gltf";
        for (const detail::AtlasPage& page : packed.pages)
        {
            scene.images.push_back({"spatial page", page.pixels});
        }
        scene.samplers.resize(1);
        scene.samplers[0].minFilter = scene.samplers[0].magFilter = TextureFilter::Linear;
        scene.samplers[0].wrapS = scene.samplers[0].wrapT = SamplerAddressMode::ClampToEdge;
        for (const detail::AtlasTileRegions& tile : packed.tiles)
        {
            const detail::AtlasMipRegion& region = packed.mips[tile.firstMip];
            TextureAsset                  texture;
            texture.name        = "tile " + std::to_string(tile.id);
            texture.image       = ImageId{region.page};
            texture.sampler     = SamplerId{0};
            texture.atlasRegion = {region.x, region.y, region.width, region.height};
            scene.textures.push_back(texture);
        }
        TextureAsset alias = scene.textures[0];
        alias.name         = "sRGB view";
        alias.format       = PixelFormat::RGBA8Srgb;
        alias.linearSource = TextureId{0};
        scene.textures.push_back(alias);
        scene.vertexTexCoordSets = {0};
        scene.materials.resize(1);
        scene.materials[0].m_pBaseColorTexture               = TextureId{3};
        scene.materials[0].textureTransforms[0].uvRotation   = 0.25f;
        scene.materials[0].textureTransforms[0].uvScale      = Vec2(0.5f);
        scene.materials[0].features.clearcoatTexture.texture = TextureId{1};
        valid &= Check(ValidateSceneAsset(scene), "Guarded regions and compatible views pass scene validation");
        const std::filesystem::path path = directory / "spatial.zenasset";
        for (TransportCodec codec : {TransportCodec::None, TransportCodec::Zstandard})
        {
            SceneWriteOptions options;
            options.compression.transport  = codec;
            valid                         &= Check(WriteScene(path.string(), scene, options).Succeeded(),
                                                   "Spatial records write with raw or Zstandard transport");
            const SceneReadResult read     = ReadScene(path.string());
            valid                         &= Check(read.scene && scene_test::Same(scene, *read.scene),
                                                   "Spatial regions, UV transforms, IDs and page bytes round-trip");
            const PackageResult package    = OpenPackage(path.string());
            valid                         &= Check(package.package != nullptr, "Spatial package opens");
            if (package.package)
            {
                for (const ResourceRecord& record : package.package->Resources())
                {
                    if (record.type == ResourceType::Texture)
                    {
                        valid &= Check(record.version == 4, "Spatial texture records require schema version four");
                    }
                }
            }
            if (read.scene)
            {
                for (size_t index = 0; index < read.scene->textures.size(); ++index)
                {
                    const TextureAsset& texture  = read.scene->textures[index];
                    const ImagePixels&  page     = *read.scene->images[texture.image.value].pixels;
                    const ImagePixels&  original = *inputs[index == 3 ? 0 : index].pixels;
                    for (uint32_t bound : {128u, 4u, 1u})
                    {
                        ImagePreviewOptions previewOptions;
                        previewOptions.maxDimension = bound;
                        const ImagePreviewResult preview =
                            MakeImagePreview({texture.format, page.width, page.height, std::as_bytes(std::span(page.bytes)),
                                              texture.interpretation, texture.atlasRegion},
                                             previewOptions);
                        const ImagePreviewResult reference = MakeImagePreview(
                            {texture.format, original.width, original.height, std::as_bytes(std::span(original.bytes))},
                            previewOptions);
                        valid &= Check(
                            preview.status.Succeeded() && reference.status.Succeeded() && preview.width == reference.width
                                && preview.height == reference.height && preview.pixels == reference.pixels,
                            "Selected tile preview matches standalone pixels and aspect ratio at full and bounded resolution");
                    }
                }
            }
        }
        valid &= TestSpatialCorruption(directory, scene);
        Scene invalid;
        for (uint32_t mutation = 0; mutation < 9; ++mutation)
        {
            invalid                            = scene;
            std::shared_ptr<ImagePixels> image = std::make_shared<ImagePixels>(*scene.images[0].pixels);
            invalid.images[0].pixels           = image;
            switch (mutation)
            {
                case 0: image->mips.clear(); break;
                case 1: image->mips.push_back(std::vector<uint8_t>(size_t(image->width / 2) * (image->height / 2) * 4)); break;
                case 2:
                    --image->width;
                    image->bytes.resize(size_t(image->width) * image->height * 4);
                    image->mips = {image->bytes};
                    break;
                case 3: invalid.textures[0].sampler = {}; break;
                case 4: invalid.samplers[0].wrapS = SamplerAddressMode::Repeat; break;
                case 5: invalid.samplers[0].minFilter = TextureFilter::Nearest; break;
                case 6: invalid.samplers[0].magFilter = TextureFilter::Unspecified; break;
                case 7: invalid.textures[0].arrayLayer = 1; break;
                case 8:
                    image->arrayLayers = 2;
                    image->bytes.resize(image->bytes.size() * 2);
                    image->mips = {image->bytes};
                    break;
            }
            valid &= Check(!ValidateSceneAsset(invalid), "Unsupported spatial image, mip or sampler policy is rejected");
        }
        const std::shared_ptr<const Scene> input = std::make_shared<Scene>(scene);
        SceneCookOptions                   cook;
        valid &= Check(CookScene(input, cook).scene == input, "No-op cooking retains region ownership");
        cook.settings.textures.arrayPages  = true;
        const SceneCookResult arrayCook    = CookScene(input, cook);
        valid                             &= Check(arrayCook.scene && scene_test::Same(*input, *arrayCook.scene),
                                                   "Array processing preserves spatial pages and regions");
        for (uint32_t operation = 0; operation < 4; ++operation)
        {
            cook = {};
            switch (operation)
            {
                case 0: cook.settings.textures.generateMipmaps = true; break;
                case 1: cook.settings.textures.compressBC7 = true; break;
                case 2: cook.settings.textures.compressBC5Normals = true; break;
                case 3: cook.settings.textures.compressBC4Occlusion = true; break;
            }
            const SceneCookResult rejected = CookScene(input, cook);
            valid &= Check(rejected.status.code == StatusCode::UnsupportedFeature && !rejected.scene
                               && scene_test::Same(scene, *input),
                           "Unsupported spatial recooking rejects atomically without treating the page as an ordinary texture");
        }
        for (TextureAsset& texture : scene.textures)
        {
            texture.atlasRegion = {};
        }
        valid &= Check(WriteScene(path.string(), scene).Succeeded(), "Full-image views retain the existing schema");
        const SceneReadResult legacy       = ReadScene(path.string());
        valid                             &= Check(legacy.scene && scene_test::Same(scene, *legacy.scene),
                                                   "Older texture records default to empty spatial regions");
        const PackageResult legacyPackage  = OpenPackage(path.string());
        if (legacyPackage.package)
        {
            for (const ResourceRecord& record : legacyPackage.package->Resources())
            {
                valid &= Check(record.type != ResourceType::Texture || record.version == 2,
                               "Standalone layer-zero texture records still use version two");
            }
        }
    }
    return valid;
}

bool TestImported(const std::filesystem::path& directory, const char* path)
{
    SceneImportContext context;
    context.workerCount              = 2;
    const SceneImportResult imported = ImportScene({path}, context);
    bool                    valid    = Check(bool(imported.asset), imported.error.c_str());
    if (imported.asset)
    {
        const std::filesystem::path first  = directory / "imported.zenasset";
        const std::filesystem::path second = directory / "imported-copy.zenasset";
        SceneWriteOptions           options;
        options.metadata.buildFingerprint  = "fixed-test-build";
        const Status written               = WriteScene(first.string(), *imported.asset, options);
        valid                             &= Check(written.Succeeded(), written.message.c_str());
        const SceneReadResult read         = ReadScene(first.string());
        valid                             &= Check(bool(read.scene), read.status.message.c_str());
        if (read.scene)
        {
            valid &= Check(scene_test::Same(*imported.asset, *read.scene),
                           "Imported and decoded scenes must have equal public field values");
            valid &=
                Check(WriteScene(second.string(), *read.scene, options).Succeeded() && LoadBytes(first) == LoadBytes(second),
                      "Imported canonical resources must survive exact raw round-trip");
        }
    }
    return valid;
}
bool TestCorpus(const std::filesystem::path& directory, const char* listPath)
{
    std::ifstream input(listPath, std::ios::binary);
    bool          valid = Check(bool(input), "Cannot open the UTF-8 corpus list");
    std::string   path;
    uint32_t      count = 0;
    while (std::getline(input, path))
    {
        if (!path.empty() && path.back() == '\r')
        {
            path.pop_back();
        }
        if (!path.empty())
        {
            const bool passed  = TestImported(directory, path.c_str());
            valid             &= passed;
            ++count;
            std::printf("%s %s\n", passed ? "CORPUS_OK" : "CORPUS_FAIL", path.c_str());
            std::fflush(stdout);
        }
    }
    std::printf("%u corpus scenes\n", count);
    return valid;
}
} // namespace

int main(int argc, char** argv)
{
    const int64_t               tick      = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path directory = std::filesystem::current_path() / ("scene-io-test-output-" + std::to_string(tick));
    std::error_code             error;
    std::filesystem::create_directories(directory, error);
    bool valid  = Check(!error, "Cannot create test output directory");
    valid      &= TestRoundTrip(directory);
    valid      &= TestBlockModeValidation(directory);
    valid      &= TestTextureInterpretation(directory);
    valid      &= TestEnvironmentEncoding(directory);
    valid      &= TestSpatialRegions(directory);
    if (argc == 3 && std::string_view(argv[1]) == "--corpus-list")
    {
        valid &= TestCorpus(directory, argv[2]);
    }
    else
    {
        for (int index = 1; index < argc; ++index)
        {
            valid &= TestImported(directory, argv[index]);
        }
    }
    std::printf("%u scene IO checks\n", checks);
    return valid ? 0 : 1;
}
