#include <ZenAsset/Processor/Import.h>
#include <ZenAsset/Processor/Write.h>
#include <ZenAsset/Reader/Scene.h>
#include <ZenAsset/Format/Version.h>
#include <charconv>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <vector>

namespace
{
struct ProgressProbe
{
    const zenasset::SceneImportControl* control{nullptr};
    zenasset::SceneImportFileProgress   last;
    uint64_t                            callbacks{0};
    bool                                coherent{true};
    bool                                sawKnownPending{false};
    bool                                sawIntermediate{false};
};

void ObserveFileProgress(void* user, zenasset::SceneImportFileProgress progress)
{
    ProgressProbe&                          probe  = *static_cast<ProgressProbe*>(user);
    const zenasset::SceneImportFileProgress polled = probe.control->GetFileProgress();
    probe.coherent        &= progress.processedFiles <= progress.totalFiles && polled.processedFiles == progress.processedFiles
                          && polled.totalFiles == progress.totalFiles && polled.totalKnown == progress.totalKnown;
    probe.sawKnownPending |= progress.totalKnown && progress.processedFiles < progress.totalFiles;
    probe.sawIntermediate |=
        progress.totalKnown && progress.processedFiles != 0 && progress.processedFiles < progress.totalFiles;
    probe.last = progress;
    ++probe.callbacks;
}

bool CheckFileProgress(const ProgressProbe& probe, uint64_t expectedFiles)
{
    const zenasset::SceneImportFileProgress final = probe.control->GetFileProgress();
    const bool valid                              = probe.callbacks != 0 && probe.coherent && probe.sawKnownPending
                                                 && (expectedFiles == 1 || probe.sawIntermediate) && final.totalKnown && final.totalFiles == expectedFiles
                                                 && final.processedFiles == expectedFiles && probe.last.totalKnown
                                                 && probe.last.totalFiles == final.totalFiles && probe.last.processedFiles == final.processedFiles;
    if (!valid)
    {
        std::cerr << "Installed progress API mismatch: callbacks=" << probe.callbacks << " processed=" << final.processedFiles
                  << " total=" << final.totalFiles << " known=" << final.totalKnown << " expected=" << expectedFiles << '\n';
    }
    return valid;
}

bool SupportsCurrentSourceFormats()
{
    bool valid = true;
    for (const char* path : {"scene.gltf", "scene.glb", "scene.fbx", "scene.obj"})
    {
        valid &= zenasset::FindSceneImportFormat(path) != nullptr;
    }
    return valid;
}

bool CheckInstalledCompression(const zenasset::Scene& scene, const char* output)
{
    zenasset::SceneWriteOptions options;
    options.compression.transport  = zenasset::TransportCodec::Zstandard;
    const zenasset::Status written = zenasset::WriteScene(output, scene, options);
    bool                   valid   = written.Succeeded();
    if (valid)
    {
        const zenasset::SceneReadResult decoded    = zenasset::ReadScene(output);
        const zenasset::PackageResult   package    = zenasset::OpenPackage(output);
        bool                            compressed = false;
        if (package.status.Succeeded())
        {
            for (const zenasset::ResourceRecord& resource : package.package->Resources())
            {
                compressed |= resource.codec == zenasset::TransportCodec::Zstandard;
            }
        }
        valid = decoded.status.Succeeded() && decoded.scene && compressed
             && decoded.scene->vertices.size() == scene.vertices.size() && decoded.scene->indices == scene.indices
             && decoded.metadata.compression.transport == zenasset::TransportCodec::Zstandard;
    }
    return valid;
}
} // namespace

int main(int argc, char** argv)
{
    int result = 1;
    if (argc != 4)
    {
        std::cerr << "Expected fixture path, package output path, and physical input-file count\n";
    }
    else
    {
        uint64_t                     expectedFiles = 0;
        const char*                  countEnd      = argv[3] + std::strlen(argv[3]);
        const std::from_chars_result parsed        = std::from_chars(argv[3], countEnd, expectedFiles);
        ProgressProbe                probe;
        zenasset::SceneImportRequest request;
        request.path    = argv[1];
        request.control = std::make_shared<zenasset::SceneImportControl>(
            zenasset::SceneImportCallbacks{&probe, nullptr, nullptr, &ObserveFileProgress});
        probe.control                          = request.control.get();
        const bool                  validCount = parsed.ec == std::errc{} && parsed.ptr == countEnd && expectedFiles != 0;
        zenasset::SceneImportResult imported;
        if (validCount)
        {
            imported = zenasset::ImportScene(request);
        }
        if (!validCount)
        {
            std::cerr << "Expected a positive physical input-file count\n";
        }
        else if (imported.status == zenasset::StatusCode::Success && imported.asset)
        {
            const zenasset::Scene& scene      = *imported.asset;
            bool                   greenPixel = false;
            for (const zenasset::ImageAsset& image : scene.images)
            {
                if (image.pixels && image.pixels->width == 1 && image.pixels->height == 1 && image.pixels->bytes.size() == 4)
                {
                    const std::vector<uint8_t>& bytes  = image.pixels->bytes;
                    greenPixel                        |= bytes[0] == 0 && bytes[1] == 255 && bytes[2] == 0 && bytes[3] == 255;
                }
            }
            if (scene.vertices.size() == 3 && scene.indices.size() == 3 && scene.nodes.size() == 1 && scene.meshes.size() == 1
                && greenPixel && SupportsCurrentSourceFormats() && CheckFileProgress(probe, expectedFiles))
            {
                if (CheckInstalledCompression(scene, argv[2]))
                {
                    std::cout << "Installed SDK imported " << expectedFiles << " physical file(s), observed " << probe.callbacks
                              << " coherent progress callbacks, and round-tripped a compressed package\n"
                              << "Library build fingerprint: " << zenasset::LibraryBuildFingerprint() << '\n';
                    result = 0;
                }
                else
                {
                    std::cerr << "Installed codec archives could not round-trip the imported scene\n";
                }
            }
            else
            {
                std::cerr << "Installed Processor produced unexpected geometry or image data\n";
            }
        }
        else
        {
            std::cerr << "Import failed: " << imported.error << '\n';
        }
    }
    return result;
}
