#include "RasterProbe.h"
#include <ZenAsset/Processor/Cook.h>
#include <ZenAsset/Processor/Import.h>
#include <ZenAsset/Processor/Write.h>
#include <ZenAsset/Format/Version.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace
{
using namespace zenasset;
using zenasset::evaluation::RasterProbe;

bool Check(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
    }
    return condition;
}

bool ValidFixture(const Scene& scene)
{
    bool valid = scene.vertices.size() == 192 && scene.indices.size() == 192 && scene.meshes.size() == 1
              && scene.meshes[0].primitives.size() == 1;
    if (valid)
    {
        const MeshPrimitiveAsset& primitive = scene.meshes[0].primitives[0];
        valid = primitive.firstIndex == 0 && primitive.indexCount == scene.indices.size() && primitive.firstVertex == 0
             && primitive.vertexCount == scene.vertices.size() && primitive.topology == MeshTopology::Triangles
             && !primitive.flatNormals;
        for (uint32_t index : scene.indices)
        {
            valid &= index < scene.vertices.size();
        }
    }
    return Check(valid, "Expected one indexed 64-triangle fixture with authored normals");
}

bool SameVertex(const Vertex& a, const Vertex& b)
{
    return a.pos == b.pos && a.normal == b.normal && a.tangent == b.tangent && a.uv0 == b.uv0 && a.uv1 == b.uv1
        && a.joint0 == b.joint0 && a.weight0 == b.weight0 && a.color == b.color;
}

bool SameCorners(const Scene& reference, const Scene& candidate)
{
    bool valid = ValidFixture(candidate) && reference.indices.size() == candidate.indices.size();
    if (valid)
    {
        for (size_t index = 0; index < reference.indices.size(); ++index)
        {
            valid &= SameVertex(reference.vertices[reference.indices[index]], candidate.vertices[candidate.indices[index]]);
        }
    }
    return Check(valid, "Resolved triangle corner values changed");
}

bool RotatedCorners(const Scene& reference, const Scene& rotated)
{
    bool valid = ValidFixture(rotated);
    if (valid)
    {
        for (size_t first = 0; first < reference.indices.size(); first += 3)
        {
            for (size_t corner = 0; corner < 3; ++corner)
            {
                valid &= SameVertex(reference.vertices[reference.indices[first + (corner + 1) % 3]],
                                    rotated.vertices[rotated.indices[first + corner]]);
            }
        }
    }
    return Check(valid, "Negative control must only rotate each triangle's corner order");
}

std::shared_ptr<const Scene> Import(const std::filesystem::path& path)
{
    SceneImportRequest request;
    request.path                          = path.string();
    const SceneImportResult      imported = ImportScene(request);
    std::shared_ptr<const Scene> scene;
    if (Check(imported.status == StatusCode::Success && imported.asset, "Import failed: " + imported.error)
        && ValidFixture(*imported.asset))
    {
        scene = imported.asset;
    }
    return scene;
}

std::shared_ptr<const Scene> Cook(const std::shared_ptr<const Scene>& source, bool optimize)
{
    SceneCookOptions options;
    options.settings.geometry.optimizeVertexFetch = optimize;
    const SceneCookResult        cooked           = CookScene(source, options);
    std::shared_ptr<const Scene> scene;
    if (Check(cooked.status.Succeeded() && cooked.scene, "Cook failed: " + cooked.status.message)
        && SameCorners(*source, *cooked.scene))
    {
        if (!optimize || Check(source->indices != cooked.scene->indices, "Fixture did not exercise actual vertex remapping"))
        {
            scene = cooked.scene;
        }
    }
    return scene;
}

struct Case
{
    const char*                  name;
    std::shared_ptr<const Scene> scene;
    bool                         optimized;
    bool                         meshoptimizer;
    bool                         zstandard;
    bool                         rotated;
};

bool WriteCase(const Case& item, const std::filesystem::path& directory, std::ofstream& report)
{
    SceneWriteOptions options;
    options.compression.meshoptimizer = item.meshoptimizer;
    options.compression.transport     = item.zstandard ? TransportCodec::Zstandard : TransportCodec::None;
    options.metadata.processing.geometry.optimizeVertexFetch = item.optimized;
    const std::filesystem::path path                         = directory / (std::string(item.name) + ".zenasset");
    const Status                written                      = WriteScene(path.string(), *item.scene, options);
    bool                        valid = Check(written.Succeeded(), "Package write failed: " + written.message);
    if (valid)
    {
        const PackageResult opened = OpenPackage(path.string());
        valid = Check(opened.status.Succeeded() && opened.package, "Package open failed: " + opened.status.message);
        if (valid)
        {
            uint64_t meshStreams           = 0;
            uint64_t compressedChunks      = 0;
            uint64_t compressedMeshStreams = 0;
            uint64_t decodedBytes          = 0;
            uint64_t storedBytes           = 0;
            for (const ResourceRecord& resource : opened.package->Resources())
            {
                if ((resource.type == ResourceType::VertexStream || resource.type == ResourceType::IndexStream)
                    && resource.encoding != 0)
                {
                    ++meshStreams;
                    compressedMeshStreams += resource.codec == TransportCodec::Zstandard ? 1 : 0;
                }
                compressedChunks += resource.codec == TransportCodec::Zstandard ? 1 : 0;
                decodedBytes     += resource.decodedBytes;
                storedBytes      += resource.storedBytes;
            }
            valid =
                Check(meshStreams == (item.meshoptimizer ? 2u : 0u), "Both mesh streams must use the requested actual encoding")
                && Check(item.zstandard ? compressedChunks != 0 : compressedChunks == 0,
                         "Transport codec was not exercised as expected");
            valid &= Check(!item.meshoptimizer || !item.zstandard || compressedMeshStreams != 0,
                           "Meshoptimizer plus Zstandard must actually compress an encoded mesh stream");
            std::error_code error;
            const uint64_t  fileBytes  = std::filesystem::file_size(path, error);
            valid                     &= Check(!error, "Cannot measure package size");
            report << item.name << '\t' << item.optimized << '\t' << meshStreams << '\t' << compressedChunks << '\t'
                   << compressedMeshStreams << '\t' << fileBytes << '\t' << storedBytes << '\t' << decodedBytes << '\n';
        }
    }
    return valid;
}

bool ReadCases(std::vector<Case>& cases, const std::filesystem::path& directory)
{
    bool valid = true;
    for (Case& item : cases)
    {
        if (valid)
        {
            const SceneReadResult read = ReadScene((directory / (std::string(item.name) + ".zenasset")).string());
            valid = Check(read.status.Succeeded() && read.scene, "Source-free package read failed: " + read.status.message);
            if (valid)
            {
                valid      = SameCorners(*item.scene, *read.scene)
                          && Check(read.metadata.processing.geometry.optimizeVertexFetch == item.optimized
                                       && read.metadata.compression.meshoptimizer == item.meshoptimizer
                                       && read.metadata.compression.transport
                                              == (item.zstandard ? TransportCodec::Zstandard : TransportCodec::None),
                                   "Decoded processing/compression metadata mismatch");
                item.scene = read.scene;
            }
        }
    }
    return valid;
}

size_t DifferentChannels(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    size_t different = std::max(a.size(), b.size());
    if (a.size() == b.size())
    {
        different = 0;
        for (size_t index = 0; index < a.size(); ++index)
        {
            different += a[index] != b[index] ? 1 : 0;
        }
    }
    return different;
}

uint32_t OracleFailures(const Scene& scene, bool lastVertex, const std::vector<uint8_t>& rgba)
{
    uint32_t failures = 0;
    for (size_t first = 0; first < scene.indices.size(); first += 3)
    {
        const Vec4& a       = scene.vertices[scene.indices[first]].pos;
        const Vec4& b       = scene.vertices[scene.indices[first + 1]].pos;
        const Vec4& c       = scene.vertices[scene.indices[first + 2]].pos;
        const float x       = ((a.x + b.x + c.x) / 3.0f * 0.5f + 0.5f) * RasterProbe::Width;
        const float y       = ((a.y + b.y + c.y) / 3.0f * 0.5f + 0.5f) * RasterProbe::Height;
        bool        matches = x >= 0 && x < RasterProbe::Width && y >= 0 && y < RasterProbe::Height;
        if (matches)
        {
            const size_t pixel    = (static_cast<size_t>(y) * RasterProbe::Width + static_cast<size_t>(x)) * 4;
            const Vec4&  expected = scene.vertices[scene.indices[first + (lastVertex ? 2 : 0)]].color;
            for (size_t channel = 0; channel < 4; ++channel)
            {
                matches &= rgba[pixel + channel] == static_cast<uint8_t>(std::lround(expected[channel] * 255.0f));
            }
        }
        failures += matches ? 0 : 1;
    }
    return failures;
}

bool SaveFrame(const std::filesystem::path& path, const std::vector<uint8_t>& rgba)
{
    std::ofstream output(path, std::ios::binary);
    output << "P6\n" << RasterProbe::Width << ' ' << RasterProbe::Height << "\n255\n";
    for (size_t pixel = 0; pixel < rgba.size(); pixel += 4)
    {
        output.write(reinterpret_cast<const char*>(rgba.data() + pixel), 3);
    }
    output.close();
    return Check(!output.fail(), "Could not save rendered frame");
}

bool RenderChecked(RasterProbe&                 probe,
                   const Scene&                 scene,
                   bool                         lastVertex,
                   std::vector<uint8_t>&        rgba,
                   const std::filesystem::path& path)
{
    const Status rendered = probe.Render(scene.vertices, scene.indices, lastVertex, rgba);
    bool         valid    = Check(rendered.Succeeded(), "Render failed: " + rendered.message);
    if (valid)
    {
        valid = Check(rgba.size() == RasterProbe::Width * RasterProbe::Height * 4, "Unexpected frame extent");
    }
    if (valid)
    {
        valid =
            Check(OracleFailures(scene, lastVertex, rgba) == 0, "Flat color differs from independent first/last corner oracle")
            && SaveFrame(path, rgba);
    }
    return valid;
}

bool RenderCases(RasterProbe&                 probe,
                 const Scene&                 source,
                 const Scene&                 rotated,
                 const std::vector<Case>&     cases,
                 const std::filesystem::path& directory)
{
    std::ofstream report(directory / "comparisons.tsv");
    report << "mode\tcase\treference_different_channels\trotated_different_channels\toracle_failures\n";
    bool valid = true;
    for (bool lastVertex : {false, true})
    {
        if (valid && (!lastVertex || probe.SupportsLastVertex()))
        {
            const std::string    mode = lastVertex ? "last" : "first";
            std::vector<uint8_t> referencePixels;
            std::vector<uint8_t> rotatedPixels;
            valid = RenderChecked(probe, source, lastVertex, referencePixels, directory / (mode + "-source.ppm"))
                 && RenderChecked(probe, rotated, lastVertex, rotatedPixels, directory / (mode + "-rotated-source.ppm"));
            if (valid)
            {
                valid = Check(DifferentChannels(referencePixels, rotatedPixels) > 1000,
                              "Rotated-corner control did not visibly change flat shading");
            }
            for (const Case& item : cases)
            {
                if (valid)
                {
                    std::vector<uint8_t> pixels;
                    valid =
                        RenderChecked(probe, *item.scene, lastVertex, pixels, directory / (mode + '-' + item.name + ".ppm"));
                    if (valid)
                    {
                        const size_t referenceDifferent = DifferentChannels(referencePixels, pixels);
                        const size_t rotatedDifferent   = DifferentChannels(rotatedPixels, pixels);
                        report << mode << '\t' << item.name << '\t' << referenceDifferent << '\t' << rotatedDifferent << '\t'
                               << OracleFailures(*item.scene, lastVertex, pixels) << '\n';
                        valid = Check((item.rotated ? rotatedDifferent : referenceDifferent) == 0,
                                      "Package or fetch remapping changed flat-shaded frame");
                    }
                }
            }
        }
    }
    report.close();
    valid &= Check(!report.fail(), "Could not save frame comparisons");
    return valid;
}

bool Run(char** argv)
{
    const std::filesystem::path directory(argv[5]);
    std::error_code             error;
    bool                        valid       = std::filesystem::create_directory(directory, error);
    valid                                   = Check(valid && !error, "Output directory must be new and its parent must exist");
    const std::filesystem::path sourcePath  = directory / "source.gltf";
    const std::filesystem::path rotatedPath = directory / "rotated.gltf";
    if (valid)
    {
        valid = std::filesystem::copy_file(argv[1], sourcePath, error) && !error;
        if (valid)
        {
            valid = std::filesystem::copy_file(argv[2], rotatedPath, error) && !error;
        }
        valid = Check(valid, "Could not copy self-contained source fixtures");
    }
    if (valid)
    {
        const std::shared_ptr<const Scene> source  = Import(sourcePath);
        const std::shared_ptr<const Scene> rotated = Import(rotatedPath);
        valid                                      = source && rotated;
        if (valid)
        {
            valid                                                = RotatedCorners(*source, *rotated);
            const std::shared_ptr<const Scene> reference         = Cook(source, false);
            const std::shared_ptr<const Scene> optimized         = Cook(source, true);
            const std::shared_ptr<const Scene> optimizedRotated  = Cook(rotated, true);
            valid                                               &= reference && optimized && optimizedRotated;
            if (valid)
            {
                std::vector<Case> cases = {{"reference-raw", reference, false, false, false, false},
                                           {"optimized-raw", optimized, true, false, false, false},
                                           {"reference-mesh-zstd", reference, false, true, true, false},
                                           {"optimized-mesh", optimized, true, true, false, false},
                                           {"optimized-mesh-zstd", optimized, true, true, true, false},
                                           {"rotated-raw", rotated, false, false, false, true},
                                           {"rotated-optimized-mesh-zstd", optimizedRotated, true, true, true, true}};
                std::ofstream     packageReport(directory / "packages.tsv");
                packageReport
                    << "case\toptimized\tencoded_mesh_streams\tzstd_chunks\tzstd_encoded_mesh_streams\tfile_bytes\tstored_chunk_bytes\tdecoded_chunk_bytes\n";
                for (const Case& item : cases)
                {
                    if (valid)
                    {
                        valid = WriteCase(item, directory, packageReport);
                    }
                }
                packageReport.close();
                valid &= Check(!packageReport.fail(), "Could not save package report");
                if (valid)
                {
                    valid = std::filesystem::remove(sourcePath, error) && !error;
                    if (valid)
                    {
                        valid = std::filesystem::remove(rotatedPath, error) && !error;
                    }
                    valid = Check(valid, "Could not remove copied sources before package reads");
                }
                if (valid)
                {
                    valid = ReadCases(cases, directory);
                }
                if (valid)
                {
                    RasterProbe  probe;
                    const Status initialized = probe.Initialize(argv[3], argv[4]);
                    valid = Check(initialized.Succeeded(), "Vulkan initialization failed: " + initialized.message);
                    if (valid)
                    {
                        valid  = RenderCases(probe, *source, *rotated, cases, directory);
                        valid &= Check(probe.ValidationErrorCount() == 0, "Vulkan validation reported errors");
                        std::ofstream deviceReport(directory / "device.txt");
                        deviceReport << "device=" << probe.DeviceName() << '\n'
                                     << "last_vertex="
                                     << (!valid                       ? "incomplete"
                                         : probe.SupportsLastVertex() ? "tested"
                                                                      : "unsupported")
                                     << '\n'
                                     << "validation_errors=" << probe.ValidationErrorCount() << '\n'
                                     << "source_free_reads=true\nactual_fetch_remapping=true\n"
                                     << "sdk_fingerprint=" << LibraryBuildFingerprint() << '\n'
                                     << "passed=" << (valid ? "true" : "false") << '\n';
                        deviceReport.close();
                        valid &= Check(!deviceReport.fail(), "Could not save device report");
                    }
                }
            }
        }
    }
    return valid;
}
} // namespace

int main(int argc, char** argv)
{
    int result = 1;
    if (argc == 6)
    {
        result = Run(argv) ? 0 : 1;
    }
    else
    {
        std::cerr << "Usage: provoking_vertex source.gltf rotated.gltf vertex.spv fragment.spv NEW_OUTPUT_DIRECTORY\n";
    }
    return result;
}
