#include "ZenAsset/Processor/Import.h"
#include "ZenAsset/Processor/Cook.h"
#include "ZenAsset/Processor/Write.h"
#include "SceneValueComparison.h"
#include "Importers/ObjNormal.h"
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

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
void Save(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream file(path, std::ios::binary);
    file << text;
}
void Image(const std::filesystem::path& path, const uint8_t* rgba)
{
    uint8_t tga[34]{};
    tga[2]  = 2;
    tga[12] = 2;
    tga[14] = 2;
    tga[16] = 32;
    tga[17] = 0x28;
    for (size_t index = 0; index < 4; ++index)
    {
        tga[18 + 4 * index] = rgba[4 * index + 2];
        tga[19 + 4 * index] = rgba[4 * index + 1];
        tga[20 + 4 * index] = rgba[4 * index];
        tga[21 + 4 * index] = rgba[4 * index + 3];
    }
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(tga), sizeof(tga));
}
SceneImportResult Load(const std::filesystem::path& path, const SceneImportContext& context = {})
{
    SceneImportRequest request;
    request.path               = path.string();
    SceneImportContext options = context;
    options.workerCount        = 1;
    SceneImportResult result   = ImportScene(request, options);
    return result;
}
bool Success(const SceneImportResult& result)
{
    if (result.status != StatusCode::Success)
    {
        std::fprintf(stderr, "Import: %s\n", result.error.c_str());
    }
    return result.status == StatusCode::Success && result.asset;
}
constexpr const char* triangle = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
bool                  Geometry(const std::filesystem::path& directory)
{
    bool                        valid = true;
    const std::filesystem::path path  = directory / "mixed.OBJ";
    Save(directory / "one.mtl", "newmtl red\nKd 1 0 0\nnewmtl blue\nKd 0 0 1\n");
    Save(directory / "two.mtl", "newmtl green\nKd 0 1 0\n");
    Save(path, "mtllib one.mtl two.mtl\nv 0 0 0\nv 2 0 0\nv 2 2 0\nv 1 1 0\nv 0 2 0\n"
               "vt 0 0\nvt 1 0\nvt 1 1\nvt .5 .5\nvt 0 1\nvn 0 0 2\no authored\ng first second\n"
               "usemtl red\nl 1/1 2/2 3/3\nusemtl blue\np 5 4\nusemtl green\nf -5/1/1 -4/2/1 -3/3/1 -2/4/1 -1/5/1\n"
               "g other\nf 5 4 3 2 1\n");
    const SceneImportResult imported  = Load(path);
    valid                            &= Check(Success(imported), "Mixed OBJ import");
    if (imported.asset)
    {
        const Scene& scene = *imported.asset;
        valid &=
            Check(imported.format == "obj" && imported.importerVersion == 1, "OBJ descriptor and case-insensitive extension");
        valid                           &= Check(scene.meshes.size() == 4 && scene.nodes.size() == 5 && scene.roots.size() == 1,
                                                 "Object/group runs retain hierarchy and source order");
        valid                           &= Check(scene.materials.size() == 4, "Every mtllib on one line is loaded");
        valid                           &= Check(scene.meshes[0].name == "first second" && scene.nodes[0].name == "authored",
                                                 "Authored object and multiple group names retained");
        const MeshPrimitiveAsset& lines  = scene.meshes[0].primitives[0];
        const MeshPrimitiveAsset& points  = scene.meshes[1].primitives[0];
        const MeshPrimitiveAsset& concave = scene.meshes[2].primitives[0];
        valid &= Check(lines.topology == MeshTopology::Lines && lines.indexCount == 4 && lines.material.value == 1,
                       "Polyline expands consecutive edges with red material");
        valid &= Check(points.topology == MeshTopology::Points && points.indexCount == 2 && points.material.value == 2,
                       "Point material survives group and usemtl boundaries");
        valid &= Check(concave.topology == MeshTopology::Triangles && concave.indexCount == 9 && concave.vertexCount == 5
                           && concave.material.value == 3,
                       "Concave polygon triangulates into three triangles");
        valid &= Check(scene.vertices[concave.firstVertex].normal.z == 2 && scene.vertices[concave.firstVertex].uv0.y == 1,
                       "Authored normal magnitude and V convention retained");
        valid &= Check(concave.sourceVertices == std::vector<uint32_t>({0, 1, 2, 3, 4}),
                       "Negative indices retain authored source position mapping");
        for (size_t meshIndex = 2; meshIndex < scene.meshes.size(); ++meshIndex)
        {
            const MeshPrimitiveAsset& primitive = scene.meshes[meshIndex].primitives[0];
            double                    area      = 0;
            bool                      winding   = true;
            for (uint32_t offset = 0; offset < primitive.indexCount; offset += 3)
            {
                const Vec4   a      = scene.vertices[scene.indices[primitive.firstIndex + offset]].pos;
                const Vec4   b      = scene.vertices[scene.indices[primitive.firstIndex + offset + 1]].pos;
                const Vec4   c      = scene.vertices[scene.indices[primitive.firstIndex + offset + 2]].pos;
                const double cross  = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
                winding            &= meshIndex == 2 ? cross > 0 : cross < 0;
                area               += cross / 2;
            }
            valid &= Check(winding && area == (meshIndex == 2 ? 3 : -3), "Concave area and authored winding preserved");
        }
        SceneWriteOptions write;
        write.metadata.sourceFormat         = "obj";
        write.compression.transport         = TransportCodec::Zstandard;
        write.compression.meshoptimizer     = true;
        const std::filesystem::path package = directory / "mixed.zenasset";
        const Status                written = WriteScene(package.string(), scene, write);
        const SceneReadResult       read    = ReadScene(package.string());
        valid &= Check(written.Succeeded() && read.status.Succeeded() && read.scene && scene_test::Same(scene, *read.scene),
                       "OBJ canonical scene survives compressed package round trip");
        const SceneImportResult repeated = Load(path);
        valid &= Check(Success(repeated) && scene_test::Same(scene, *repeated.asset), "OBJ import is deterministic");
    }
    Save(directory / "smooth.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\ns 4\ng one\nf 1 2 3\ng two\nf 1 3 4\ns off\nf 1 4 2\n");
    const SceneImportResult smooth  = Load(directory / "smooth.obj");
    valid                          &= Check(Success(smooth), "Smoothing group import");
    if (smooth.asset)
    {
        const Vec4 normal  = smooth.asset->vertices[0].normal;
        valid             &= Check(std::abs(normal.x - std::sqrt(.5f)) < 1e-6f && std::abs(normal.z - std::sqrt(.5f)) < 1e-6f,
                                   "Smoothing crosses group boundaries for a shared source position");
        valid             &= Check(smooth.asset->vertices[6].normal.y == 1, "Smoothing-off polygon remains flat");
    }
    return valid;
}
bool Materials(const std::filesystem::path& directory)
{
    bool valid = true;
    std::filesystem::create_directories(directory / "materials");
    const uint8_t color[]{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255};
    const uint8_t scalar[]{0, 0, 0, 255, 64, 64, 64, 255, 128, 128, 128, 255, 255, 255, 255, 255};
    const uint8_t normal[]{128, 32, 255, 255, 128, 64, 255, 255, 128, 128, 255, 255, 128, 192, 255, 255};
    Image(directory / "color.tga", color);
    Image(directory / "materials/opacity.tga", scalar);
    Image(directory / "materials/normal.tga", normal);
    Save(directory / "materials/sample.mtl",
         "newmtl surface\nKd .5 .6 .7\nKs .1 .2 .3\nKe .2 .1 0\nNs 30\nNi 1.7\nd .8\n"
         "map_Kd color.tga\nmap_d opacity.tga\nmap_Ns opacity.tga\nnorm normal.tga\nbump missing-height.tga\n");
    Save(
        directory / "material.obj",
        "mtllib materials/sample.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nvt .25 .25\nvt .75 .25\nvt .25 .75\nusemtl surface\nf 1/1 2/2 3/3\n");
    const SceneImportResult imported  = Load(directory / "material.obj");
    valid                            &= Check(Success(imported), "MTL color/scalar/normal map import");
    if (imported.asset)
    {
        const Scene&         scene    = *imported.asset;
        const MaterialAsset& material = scene.materials[1];
        const ImagePixels&   base     = *scene.images[scene.textures[material.m_pBaseColorTexture.value].image.value].pixels;
        valid           &= Check(material.baseColorFactor == Vec4(.5f, .6f, .7f, .8f) && material.alphaMode == AlphaMode::Blend
                                     && material.features.ior == 1.7f,
                                 "MTL scalar factors map to canonical PBR");
        valid           &= Check(base.bytes[0] == 255 && base.bytes[3] == 0 && base.bytes[7] == 64 && base.bytes[11] == 128
                                     && base.bytes[15] == 255,
                                 "map_d packs scalar luminance into base alpha");
        const Vec2   uv  = scene.vertices[0].uv0;
        const size_t pixel                  = (static_cast<size_t>(uv.y * 2) * 2 + static_cast<size_t>(uv.x * 2)) * 4;
        valid                              &= Check(base.bytes[pixel] == 0 && base.bytes[pixel + 2] == 255,
                                                    "Asymmetric2x2 OBJ lower-left UV samples image bottom-left blue");
        const ImagePixels& convertedNormal  = *scene.images[scene.textures[material.m_pNormalTexture.value].image.value].pixels;
        valid                              &= Check(convertedNormal.bytes[1] == 223 && convertedNormal.bytes[5] == 191,
                                                    "Explicit norm green channel reverses with V");
        const ImagePixels& roughness =
            *scene.images[scene.textures[material.m_pMetallicRoughnessTexture.value].image.value].pixels;
        valid                 &= Check(roughness.bytes[1] == 255 && roughness.bytes[13] == 64 && material.roughnessFactor == 1,
                                       "map_Ns converts exponent to PBR roughness");
        bool missingPreferred  = false;
        bool fallback          = false;
        bool bumpDiagnostic    = false;
        for (const SceneDependency& dependency : scene.dependencies)
        {
            missingPreferred |= !dependency.exists && dependency.path.ends_with("materials/color.tga");
            fallback         |= dependency.exists && dependency.path.ends_with("/color.tga");
        }
        for (const Diagnostic& diagnostic : imported.diagnostics)
        {
            bumpDiagnostic |= diagnostic.message.find("height map") != std::string::npos;
        }
        valid &=
            Check(missingPreferred && fallback, "Missing MTL-relative preferred texture and OBJ-relative fallback are tracked");
        valid &= Check(bumpDiagnostic, "Bump height map is diagnosed rather than interpreted as normal");
    }
    return valid;
}
void CancelGeometry(void* user, SceneImportStage stage)
{
    if (stage == SceneImportStage::ProcessingMeshes)
    {
        static_cast<SceneImportControl*>(*static_cast<SceneImportControl**>(user))->Cancel();
    }
}
bool Boundaries(const std::filesystem::path& directory)
{
    bool valid = true;
    Save(directory / "comments.obj",
         "# source C:\\models\\\nv 0 0 0\nv 1 0 0 # comment\\\nv 0 1 0\nf 1 2 3 # another\\\np 1\n");
    const SceneImportResult comments  = Load(directory / "comments.obj");
    valid                            &= Check(Success(comments) && comments.asset->meshes.size() == 2,
                                              "Comment backslashes cannot consume following statements");
    Save(directory / "bom.obj",
         std::string("\xef\xbb\xbf") + "v +0 0 0 1 0 0\nv 1 0 0 0 1 0\nv 0 1 0 0 0 1\nusemtl unknown\nf 1 2 \\\n3\n");
    const SceneImportResult bom  = Load(directory / "bom.obj");
    valid                       &= Check(Success(bom), "BOM, plus sign and line continuation import");
    if (bom.asset)
    {
        valid &= Check(bom.asset->vertices[0].color == Vec4(1, 0, 0, 1) && bom.asset->vertices[2].color == Vec4(0, 0, 1, 1),
                       "OBJ optional vertex colors remain attached to corners");
        valid &= Check(bom.asset->indices == std::vector<uint32_t>({0, 1, 2}), "Authored triangle order remains exact");
        valid &= Check(bom.asset->dependencies.empty(), "Root source is separate from external dependency records");
        bool diagnostic = false;
        for (const Diagnostic& item : bom.diagnostics)
        {
            diagnostic |= item.message.find("Unknown OBJ usemtl") != std::string::npos;
        }
        valid &= Check(diagnostic && bom.asset->meshes[0].primitives[0].material.value == 0,
                       "Unknown material uses documented default with diagnostic");
    }
    Save(directory / "no-uv.obj", "mtllib materials/sample.mtl\nusemtl surface\n" + std::string(triangle));
    valid &= Check(!Load(directory / "no-uv.obj").asset, "Textured corner without UVs fails explicitly");
    Save(directory / "maps.mtl",
         "newmtl mapped\nmap_Pr materials/opacity.tga\nmap_Pm materials/opacity.tga\n"
         "newmtl zero\nPr 0\nPm 0\nmap_Pr materials/opacity.tga\nmap_Pm materials/opacity.tga\n"
         "newmtl transform\nd .4\nTr .9\nmap_Kd -o .25 .5 -s 2 3 -clamp on color.tga\nnorm -bm 2 materials/normal.tga\n");
    Save(directory / "maps.obj",
         "mtllib maps.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nusemtl mapped\nf 1/1 2/2 3/3\n");
    const SceneImportResult maps  = Load(directory / "maps.obj");
    valid                        &= Check(Success(maps), "MTL PBR extension maps and transforms import");
    if (maps.asset)
    {
        const MaterialAsset& mapped     = maps.asset->materials[1];
        const MaterialAsset& zero       = maps.asset->materials[2];
        const MaterialAsset& transform  = maps.asset->materials[3];
        valid                          &= Check(mapped.roughnessFactor == 1 && mapped.metallicFactor == 1,
                                                "Unspecified scalar factors leave Pr/Pm maps unattenuated");
        valid &= Check(zero.roughnessFactor == 0 && zero.metallicFactor == 0, "Explicit zero Pr/Pm factors remain zero");
        valid &= Check(transform.baseColorFactor.w == .4f, "Authored d has precedence over Tr");
        valid &= Check(transform.normalScale == 2, "Explicit norm -bm maps to normalScale");
        valid &= Check(transform.textureTransforms[0].uvOffset == Vec2(.25f, -2.5f)
                           && transform.textureTransforms[0].uvScale == Vec2(2, 3),
                       "UV transform conjugates V reversal");
        const TextureAsset& texture = maps.asset->textures[transform.m_pBaseColorTexture.value];
        valid &= Check(maps.asset->samplers[texture.sampler.value].wrapS == SamplerAddressMode::ClampToEdge,
                       "MTL clamp sampler retained");
    }
    Save(directory / "mismatch.mtl", "newmtl mismatch\nmap_Kd -s 2 1 color.tga\nmap_d materials/opacity.tga\n");
    Save(directory / "mismatch.obj", "mtllib mismatch.mtl\nusemtl mismatch\n" + std::string(triangle));
    valid &= Check(Load(directory / "mismatch.obj").status == StatusCode::UnsupportedFeature,
                   "Different scalar/base UV transforms cannot be silently packed");
    Save(directory / "invalid.mtl", "newmtl invalid\nKd broken 0 0\n");
    Save(directory / "invalid-material.obj", "mtllib invalid.mtl\n" + std::string(triangle));
    valid &= Check(!Load(directory / "invalid-material.obj").asset, "Malformed MTL factor fails explicitly");
    Save(directory / "invalid.mtl", "newmtl invalid\nmap_Kd -bm 2 color.tga\n");
    valid &= Check(Load(directory / "invalid-material.obj").status == StatusCode::UnsupportedFeature,
                   "Non-normal maps reject inapplicable -bm");
    SceneImportContext bounded;
    bounded.maxSourceBytes  = std::filesystem::file_size(directory / "maps.obj") + 1;
    valid                  &= Check(Load(directory / "maps.obj", bounded).status == StatusCode::ResourceLimit,
                                    "OBJ source budget includes external MTL bytes");
    std::string oversized   = "v 0 0 0\np";
    for (uint32_t index = 0; index < 4097; ++index)
    {
        oversized += " 1";
    }
    Save(directory / "corners.obj", oversized + "\n");
    valid &= Check(Load(directory / "corners.obj").status == StatusCode::ResourceLimit,
                   "Per-statement corner ceiling is a resource limit");
    return valid;
}
bool CancelThirdPoll(void* user)
{
    uint32_t& polls = *static_cast<uint32_t*>(user);
    ++polls;
    return polls >= 3;
}
bool NormalCancellation()
{
    uint32_t           polls = 0;
    SceneImportControl control(SceneImportCallbacks{&polls, CancelThirdPoll, nullptr});
    ImportErrors       errors;
    errors.Reset(&control);
    std::vector<uint8_t> pixels(512 * 1024, 10);
    ReverseObjNormalGreen(pixels, errors);
    bool valid  = Check(errors.Failed() && pixels[1] == 245 && pixels[256 * 1024 + 1] == 10,
                        "OBJ normal conversion observes cancellation after bounded blocks");
    valid      &= Check(pixels[0] == 10 && pixels[2] == 10 && pixels[3] == 10, "Normal conversion preserves X/Z/alpha");
    return valid;
}
bool MaterialDefaults(const std::filesystem::path& directory)
{
    Save(directory / "defaults.mtl",
         "newmtl texture_only\nmap_Kd color.tga\nmap_Ks color.tga\nmap_Ke color.tga\n"
         "newmtl authored_zero\nKd 0 0 0\nKs 0 0 0\nKe 0 0 0\nmap_Kd color.tga\nmap_Ks color.tga\nmap_Ke color.tga\n"
         "newmtl unlit\nillum 0\nKd .2 .3 .4\n");
    Save(directory / "defaults.obj",
         "mtllib defaults.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nusemtl texture_only\nf 1/1 2/2 3/3\n");
    const SceneImportResult imported = Load(directory / "defaults.obj");
    bool                    valid = Check(Success(imported), "Texture-only material imports without invented zero multipliers");
    if (imported.asset)
    {
        const MaterialAsset& texture  = imported.asset->materials[1];
        const MaterialAsset& zero     = imported.asset->materials[2];
        valid                        &= Check(texture.baseColorFactor == Vec4(1) && texture.features.specularColor == Vec3(1)
                                                  && texture.emissiveFactor == Vec4(1, 1, 1, 0),
                                              "Omitted texture color multipliers default to one");
        valid                        &= Check(zero.baseColorFactor == Vec4(0, 0, 0, 1) && zero.features.specularColor == Vec3(0)
                                                  && zero.emissiveFactor == Vec4(0),
                                              "Explicit zero color factors retain their authored meaning");
        valid &=
            Check(!texture.unlit && !zero.unlit && imported.asset->materials[3].unlit, "Only explicit illum0 selects unlit");
    }
    return valid;
}
bool Failures(const std::filesystem::path& directory)
{
    bool        valid = true;
    const char* invalid[]{"v 0 0 0\np 2\n",
                          "v 0 0 0\nl 1 2\n",
                          "v 0 0 0\nf 1 1 1\n",
                          "v 0 0 0\nv 1 1 0\nv 0 1 0\nv 1 0 0\nf 1 2 3 4\n",
                          "v 0 0 0\nv 1 0 0\nv 1 1 1\nv 0 1 0\nf 1 2 3 4\n",
                          "curv 0 1 1 2\n",
                          "",
                          "v hello 0 0\np 1\n",
                          "v 0 nan 0\np 1\n",
                          "v 0 0 0 .5\np 1\n",
                          "v 0 0 0\nvt 0 0 1\np 1/1\n",
                          "v 0 0 0\ns nonsense\np 1\n"};
    for (const char* source : invalid)
    {
        Save(directory / "bad.obj", source);
        const SceneImportResult result = Load(directory / "bad.obj");
        valid &=
            Check(result.status != StatusCode::Success && !result.asset, "Malformed or unsupported OBJ publishes no scene");
    }
    Save(directory / "budget.obj", triangle);
    SceneImportContext context;
    context.maxObjGeometryBytes   = 256;
    valid                        &= Check(Load(directory / "budget.obj", context).status == StatusCode::ResourceLimit,
                                          "OBJ geometry budget rejects expansion");
    context                       = {};
    context.maxSourceBytes        = 4;
    valid                        &= Check(Load(directory / "budget.obj", context).status == StatusCode::ResourceLimit,
                                          "OBJ source byte budget rejects input");
    context                       = {};
    context.maxDecodedImageBytes  = 1;
    valid                        &= Check(Load(directory / "material.obj", context).status == StatusCode::ResourceLimit,
                                          "OBJ image byte budget applies to decoding");
    Save(directory / "missing.obj", "mtllib absent.mtl\n" + std::string(triangle));
    valid                       &= Check(Load(directory / "missing.obj").status == StatusCode::SourceUnavailable,
                                         "Missing authored MTL is explicit failure");
    SceneImportControl* control  = nullptr;
    SceneImportRequest  request;
    request.path    = (directory / "budget.obj").string();
    request.control = std::make_shared<SceneImportControl>(SceneImportCallbacks{&control, nullptr, CancelGeometry});
    control         = request.control.get();
    const SceneImportResult cancelled  = ImportScene(request);
    valid                             &= Check(cancelled.status == StatusCode::Cancelled && !cancelled.asset,
                                               "Cancellation during geometry stage publishes no scene");
    request.control.reset();
    request.settings.sceneIndex = 1;
    valid &= Check(ImportScene(request).status == StatusCode::InvalidRequest, "OBJ rejects nonexistent scene selection");
    return valid;
}
} // namespace

int main()
{
    const int64_t               stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / ("zenasset-obj-import-tests-" + std::to_string(stamp));
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    bool valid  = !error;
    valid      &= Geometry(directory);
    valid      &= Materials(directory);
    valid      &= Boundaries(directory);
    valid      &= NormalCancellation();
    valid      &= MaterialDefaults(directory);
    valid      &= Failures(directory);
    std::filesystem::remove_all(directory, error);
    std::printf("OBJ import checks: %u\n", checks);
    return valid ? 0 : 1;
}
