#include "ZenAsset/Reader/Scene.h"
#include "ZenAsset/Reader/Image.h"
#include "ZenAsset/Format/Validation.h"
#include "Format/PackageLayout.h"
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>
#if defined(_WIN32)
#    include <process.h>
#else
#    include <unistd.h>
#endif

namespace
{
constexpr size_t MaxInputBytes = 256u << 10;

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "Reader fuzz invariant failed: %s\n", message);
        std::abort();
    }
}

class InputFile
{
public:
    InputFile()
    {
        std::error_code error;
        m_path = std::filesystem::temp_directory_path(error);
        Require(!error, "cannot locate temporary directory");
#if defined(_WIN32)
        const int processId = _getpid();
#else
        const int processId = getpid();
#endif
        const int64_t tick           = std::chrono::steady_clock::now().time_since_epoch().count();
        m_path                      /= "zenasset-fuzz-" + std::to_string(processId) + "-" + std::to_string(tick) + ".zenasset";
        const std::u8string encoded  = m_path.u8string();
        m_utf8.assign(reinterpret_cast<const char*>(encoded.data()), encoded.size());
    }
    ~InputFile()
    {
        std::error_code error;
        std::filesystem::remove(m_path, error);
    }
    const std::string& Save(const uint8_t* data, size_t size)
    {
        std::ofstream output(m_path, std::ios::binary | std::ios::trunc);
        if (size != 0)
        {
            output.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        }
        output.close();
        Require(bool(output), "cannot write temporary input");
        return m_utf8;
    }

private:
    std::filesystem::path m_path;
    std::string           m_utf8;
};

void ExerciseImagePreviews(const zenasset::ImagePixels&    pixels,
                           zenasset::PixelFormat           format,
                           zenasset::TextureInterpretation interpretation)
{
    zenasset::ImagePreviewOptions options;
    options.maxDimension   = 8;
    options.maxOutputBytes = 256;
    const bool supported =
        format == zenasset::PixelFormat::RGBA8Unorm || format == zenasset::PixelFormat::RGBA8Srgb
        || format == zenasset::PixelFormat::BC7Unorm || format == zenasset::PixelFormat::BC7Srgb
        || format == zenasset::PixelFormat::BC4Unorm
        || (format == zenasset::PixelFormat::BC5Unorm && interpretation == zenasset::TextureInterpretation::NormalXYPositiveZ);
    const size_t count = pixels.mips.empty() ? 1 : pixels.mips.size();
    for (size_t level = 0; level < count; ++level)
    {
        const std::vector<uint8_t>&        bytes = pixels.mips.empty() ? pixels.bytes : pixels.mips[level];
        const zenasset::ImageMipView       view{format,
                                                std::max(1u, pixels.width >> level),
                                                std::max(1u, pixels.height >> level),
                                                {reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()},
                                                interpretation};
        const zenasset::ImagePreviewResult preview = zenasset::MakeImagePreview(view, options);
        Require(!supported || preview.status.Succeeded(), "valid supported image view failed to preview");
        Require(preview.status.Succeeded() || preview.pixels.empty(), "failed preview exposed pixels");
        if (preview.status.Succeeded())
        {
            Require(preview.width > 0 && preview.width <= options.maxDimension && preview.height > 0
                        && preview.height <= options.maxDimension
                        && preview.pixels.size() == uint64_t(preview.width) * preview.height * 4
                        && preview.pixels.size() <= options.maxOutputBytes,
                    "successful preview exceeded output bounds");
        }
        zenasset::Cancellation cancellation;
        cancellation.Cancel();
        const zenasset::ImagePreviewResult cancelled = zenasset::MakeImagePreview(view, options, &cancellation);
        // Unsupported interpretations are rejected before cancellation is polled.
        const zenasset::StatusCode expected = supported ? zenasset::StatusCode::Cancelled : preview.status.code;
        Require(cancelled.status.code == expected && cancelled.pixels.empty(),
                "cancelled preview exposed pixels or failed to cancel");
    }
}

void ExercisePreviews(const zenasset::Scene& scene)
{
    for (const zenasset::ImageAsset& image : scene.images)
    {
        ExerciseImagePreviews(*image.pixels, image.pixels->format, zenasset::TextureInterpretation::Default);
    }
    for (const zenasset::TextureAsset& texture : scene.textures)
    {
        ExerciseImagePreviews(*scene.images[texture.image.value].pixels, texture.format, texture.interpretation);
    }
}

void Exercise(InputFile& file, const uint8_t* data, size_t size)
{
    zenasset::SceneReadLimits limits;
    limits.package.maxFileBytes          = MaxInputBytes;
    limits.package.maxDecodedBytes       = 2u << 20;
    limits.package.maxChunkBytes         = MaxInputBytes;
    limits.package.maxResources          = 512;
    limits.package.maxDirectoryBytes     = 64u << 10;
    limits.maxAllocationBytes            = 4u << 20;
    limits.maxArrayElements              = 65536;
    limits.maxStringBytes                = 64u << 10;
    limits.maxSourceDocumentBytes        = MaxInputBytes;
    limits.maxRuntimeGeometryBytes       = 4u << 20;
    const std::string&            path   = file.Save(data, size);
    const zenasset::PackageResult opened = zenasset::OpenPackage(path, limits.package);
    Require(opened.status.Succeeded() == bool(opened.package), "package result ownership disagrees with status");
    if (opened.package)
    {
        const zenasset::Package& package = *opened.package;
        package.ValidatePayloads();
        for (const zenasset::ResourceRecord& record : package.Resources())
        {
            const zenasset::ResourceData resource = package.Read(record.id);
            Require(resource.status.Succeeded() || resource.bytes.empty(), "failed resource read exposed payload");
            Require(!resource.status.Succeeded() || resource.bytes.size() == record.decodedBytes,
                    "successful resource read has an unexpected size");
        }
        const zenasset::SceneMetadataResult metadata = zenasset::ReadSceneMetadata(package, limits);
        (void)metadata;
        const zenasset::SceneReadResult decoded = zenasset::ReadScene(package, limits);
        Require(decoded.status.Succeeded() == bool(decoded.scene), "scene result ownership disagrees with status");
        if (decoded.scene)
        {
            std::string reason;
            Require(zenasset::ValidateSceneAsset(*decoded.scene, limits.maxRuntimeGeometryBytes, &reason),
                    "successful scene read exposed an invalid graph");
            ExercisePreviews(*decoded.scene);
        }
        zenasset::Cancellation cancellation;
        cancellation.Cancel();
        Require(zenasset::ReadScene(package, limits, &cancellation).status.code == zenasset::StatusCode::Cancelled,
                "cancelled scene read was not cancelled");
    }
}

// Check the original bytes first. Also repair integrity fields, when their
// bounded ranges are representable, so mutations can reach semantic decoding
// without having to rediscover three independent checksums.
void RepairIntegrity(std::vector<uint8_t>& bytes)
{
    using namespace zenasset::detail;
    if (bytes.size() >= HeaderBytes)
    {
        const uint64_t directoryOffset = Read64(bytes.data() + 24);
        const uint64_t count           = Read64(bytes.data() + 32);
        if (directoryOffset == HeaderBytes && count <= 512 && count <= (bytes.size() - HeaderBytes) / DirectoryEntryBytes)
        {
            for (size_t index = 0; index < count; ++index)
            {
                uint8_t*       entry  = bytes.data() + HeaderBytes + index * DirectoryEntryBytes;
                const uint64_t offset = Read64(entry + 24);
                const uint64_t size   = Read64(entry + 32);
                if (offset <= bytes.size() && size <= bytes.size() - offset)
                {
                    Write64(entry + 48, HashBytes(bytes.data() + offset, static_cast<size_t>(size)));
                }
            }
            Write64(bytes.data() + 48, HashBytes(bytes.data() + HeaderBytes, count * DirectoryEntryBytes));
        }
        Write64(bytes.data() + 56, HashBytes(bytes.data(), 56));
    }
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size <= MaxInputBytes)
    {
        static InputFile file;
        Exercise(file, data, size);
        std::vector<uint8_t> repaired(data, data + size);
        RepairIntegrity(repaired);
        Exercise(file, repaired.data(), repaired.size());
    }
    return 0;
}
