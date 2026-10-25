#include "ZenAsset/Format/Scene.h"
#include "ZenAsset/Format/Processing.h"
#include "ZenAsset/Format/Utf8.h"
#include "ZenAsset/Format/Version.h"
#include "ZenAsset/Format/Validation.h"
#include <cstdio>
#include <limits>
#include <type_traits>

namespace
{
void Check(bool condition, const char* message, int& failures)
{
    if (!condition)
    {
        std::fprintf(stderr, "%s\n", message);
        ++failures;
    }
}

void CheckProcessingSettings(int& failures)
{
    using namespace zenasset;
    const ProcessingSettings defaults;
    const uint64_t           hash = ProcessingSettingsHash(defaults);
    Check(ValidateProcessingSettings(defaults).Succeeded(), "Default processing settings must validate", failures);
    Check(hash == ProcessingSettingsHash(ProcessingSettings{}), "Settings identity must be deterministic", failures);
    ProcessingSettings changed;
    changed.geometry.optimizeVertexFetch = true;
    Check(hash != ProcessingSettingsHash(changed), "Fetch optimization must change settings identity", failures);
    changed                                   = defaults;
    changed.geometry.positionQuantizationBits = 16;
    Check(hash != ProcessingSettingsHash(changed), "Position precision must change settings identity", failures);
    for (float GeometrySettings::* field :
         {&GeometrySettings::positionMaxRelativeError, &GeometrySettings::positionMaxAbsoluteError})
    {
        changed                  = defaults;
        changed.geometry.*field *= 2;
        Check(hash != ProcessingSettingsHash(changed), "Every position error setting must change settings identity", failures);
    }
    Check(!HasProcessingOperations(defaults), "Default settings preserve the input owner", failures);
    for (uint32_t TextureSettings::* field : {&TextureSettings::arrayMaxDimension, &TextureSettings::arrayMaxLayers,
                                              &TextureSettings::arrayMaxPages, &TextureSettings::arrayMaxImages})
    {
        changed                 = defaults;
        changed.textures.*field = 0;
        Check(!ValidateProcessingSettings(changed).Succeeded(), "Array limits must be valid even while disabled", failures);
    }
    changed                            = defaults;
    changed.textures.arrayMaxDimension = 16385;
    Check(!ValidateProcessingSettings(changed).Succeeded(), "Array dimension upper bound is enforced", failures);
    changed                         = defaults;
    changed.textures.arrayMaxLayers = 2049;
    Check(!ValidateProcessingSettings(changed).Succeeded(), "Array layer upper bound is enforced", failures);
    changed.textures.arrayMaxLayers = 1;
    Check(!ValidateProcessingSettings(changed).Succeeded(), "An array page needs at least two layers", failures);
    for (bool TextureSettings::* field :
         {&TextureSettings::generateMipmaps, &TextureSettings::compressBC7, &TextureSettings::compressBC5Normals,
          &TextureSettings::compressBC4Occlusion, &TextureSettings::compressBC6HEnvironment, &TextureSettings::arrayPages,
          &TextureSettings::spatialPages})
    {
        changed                 = defaults;
        changed.textures.*field = true;
        Check(HasProcessingOperations(changed), "Every texture activation must request processing", failures);
        Check(hash != ProcessingSettingsHash(changed), "Every texture activation must change settings identity", failures);
    }
    for (uint32_t TextureSettings::* field :
         {&TextureSettings::bc7Quality, &TextureSettings::maxDimension, &TextureSettings::arrayMaxDimension,
          &TextureSettings::arrayMaxLayers, &TextureSettings::arrayMaxPages, &TextureSettings::arrayMaxImages,
          &TextureSettings::spatialMaxDimension, &TextureSettings::spatialMaxPages, &TextureSettings::spatialMaxImages})
    {
        changed = defaults;
        --(changed.textures.*field);
        Check(hash != ProcessingSettingsHash(changed), "Every texture integer setting must change settings identity", failures);
    }
    for (float TextureSettings::* field :
         {&TextureSettings::colorMinPsnr, &TextureSettings::colorMaxRmse, &TextureSettings::scalarMaxError,
          &TextureSettings::normalRmsAngleDegrees, &TextureSettings::normalMaxAngleDegrees,
          &TextureSettings::alphaCoverageMaxError, &TextureSettings::hdrMaxRelativeRmse})
    {
        changed                  = defaults;
        changed.textures.*field *= 2;
        Check(hash != ProcessingSettingsHash(changed), "Every texture error setting must change settings identity", failures);
        for (float invalid : {-1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
        {
            changed.textures.*field = invalid;
            Check(!ValidateProcessingSettings(changed).Succeeded(), "Invalid texture quality bounds must fail", failures);
        }
    }
    for (uint32_t TextureSettings::* field :
         {&TextureSettings::spatialMaxDimension, &TextureSettings::spatialMaxPages, &TextureSettings::spatialMaxImages})
    {
        changed                 = defaults;
        changed.textures.*field = 0;
        Check(!ValidateProcessingSettings(changed).Succeeded(), "Zero spatial limits fail even while disabled", failures);
    }
    for (uint32_t dimension : {0u, 8u, 31u, 16385u})
    {
        changed                              = defaults;
        changed.textures.spatialPages        = true;
        changed.textures.spatialMaxDimension = dimension;
        Check(!ValidateProcessingSettings(changed).Succeeded(), "Enabled spatial pages require bounded POT dimensions",
              failures);
    }
    changed                     = defaults;
    changed.textures.arrayPages = changed.textures.spatialPages = true;
    Check(!ValidateProcessingSettings(changed).Succeeded(), "Array and spatial modes are mutually exclusive", failures);
    changed                     = defaults;
    changed.textures.bc7Quality = 5;
    Check(!ValidateProcessingSettings(changed).Succeeded(), "Unknown BC7 quality must fail", failures);
    changed                       = defaults;
    changed.textures.maxDimension = 0;
    Check(!ValidateProcessingSettings(changed).Succeeded(), "Zero maximum texture dimension must fail", failures);
    changed                                = defaults;
    changed.textures.normalRmsAngleDegrees = 6;
    Check(!ValidateProcessingSettings(changed).Succeeded(), "Normal RMS tolerance must not exceed maximum tolerance", failures);
    for (float TextureSettings::* field : {&TextureSettings::colorMaxRmse, &TextureSettings::scalarMaxError,
                                           &TextureSettings::alphaCoverageMaxError, &TextureSettings::hdrMaxRelativeRmse})
    {
        changed                 = defaults;
        changed.textures.*field = 1.01f;
        Check(!ValidateProcessingSettings(changed).Succeeded(), "Normalized texture error tolerance must not exceed one",
              failures);
    }
    changed                                = defaults;
    changed.textures.normalMaxAngleDegrees = 181;
    Check(!ValidateProcessingSettings(changed).Succeeded(), "Normal angular tolerance must not exceed 180 degrees", failures);
}
} // namespace

int main()
{
    static_assert(std::is_same_v<std::underlying_type_t<zenasset::PixelFormat>, uint32_t>);
    static_assert(std::is_same_v<std::underlying_type_t<zenasset::AnimatedProperty>, uint32_t>);
    static_assert(!std::is_same_v<zenasset::NodeId, zenasset::MeshId>);
    static_assert(sizeof(zenasset::NodeId) == sizeof(uint32_t));
    static_assert(std::is_trivially_copyable_v<zenasset::Mat4>);
    static_assert(zenasset::Mat4()[3][3] == 1.0f);

    int failures = 0;
    CheckProcessingSettings(failures);
    Check(zenasset::IsValidUtf8(std::string_view("ASCII\0text", 10)), "ASCII and embedded null must be valid UTF-8", failures);
    Check(zenasset::IsValidUtf8("\xc2\xa2\xe2\x82\xac\xf0\x90\x8d\x88"), "Valid multibyte scalars must decode", failures);
    for (std::string_view text : {"\x80", "\xc0\x80", "\xe0\x80\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82"})
    {
        Check(!zenasset::IsValidUtf8(text), "Malformed UTF-8 must be rejected", failures);
    }
    Check(zenasset::LibraryBuildFingerprint().size() == 64, "Build identity must contain a SHA-256 fingerprint", failures);
    uint64_t bytes = 0;
    Check(zenasset::GetImageLevelByteSize(zenasset::PixelFormat::BC7Unorm, 1, 1, bytes) && bytes == 16,
          "A tiny BC7 mip must occupy a whole block", failures);
    Check(zenasset::GetImageLevelByteSize(zenasset::PixelFormat::BC5Snorm, 5, 7, bytes) && bytes == 64,
          "Non-aligned BC5 dimensions must round up both block axes", failures);
    Check(zenasset::GetImageLevelByteSize(zenasset::PixelFormat::RGBA8Srgb, 5, 7, bytes) && bytes == 140,
          "RGBA8 must remain tightly packed", failures);
    Check(!zenasset::GetImageLevelByteSize(zenasset::PixelFormat::RGBA32Float, UINT32_MAX, UINT32_MAX, bytes) && bytes == 0,
          "Oversized image products must reject arithmetic overflow", failures);
    Check(!zenasset::GetImageLevelByteSize(zenasset::PixelFormat::BC7Unorm, 0, 1, bytes) && bytes == 0,
          "Zero dimensions must be rejected", failures);
    Check(!zenasset::GetImageLevelByteSize(static_cast<zenasset::PixelFormat>(UINT32_MAX), 1, 1, bytes) && bytes == 0,
          "Unknown image formats must be rejected", failures);
    Check(zenasset::ArePixelFormatsViewCompatible(zenasset::PixelFormat::BC7Unorm, zenasset::PixelFormat::BC7Srgb),
          "BC7 storage must permit a compatible sRGB view", failures);
    Check(!zenasset::ArePixelFormatsViewCompatible(zenasset::PixelFormat::BC5Unorm, zenasset::PixelFormat::BC5Snorm),
          "Signed and unsigned normal encodings require distinct semantics", failures);
    Check(!zenasset::ArePixelFormatsViewCompatible(zenasset::PixelFormat::Undefined, zenasset::PixelFormat::Undefined),
          "Matching undefined formats do not define valid storage", failures);

    const zenasset::Mat4 matrix(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16);
    Check(matrix[0][3] == 4 && matrix[3][0] == 13, "Matrix constructor must use column-major ordering", failures);
    zenasset::Bounds bounds;
    bounds.SetMin(zenasset::Vec3(-2, 4, 1));
    bounds.SetMin(zenasset::Vec3(2, -4, 3));
    bounds.SetMax(zenasset::Vec3(-2, 4, 1));
    bounds.SetMax(zenasset::Vec3(2, -4, 3));
    Check(bounds.GetMin() == zenasset::Vec3(-2, -4, 1) && bounds.GetMax() == zenasset::Vec3(2, 4, 3),
          "Bounds setters must accumulate extrema", failures);
    Check(!zenasset::NodeId().IsValid() && zenasset::NodeId{0}.IsValid(), "Default IDs must represent absence", failures);

    zenasset::Scene                        scene;
    std::shared_ptr<zenasset::ImagePixels> pixels = std::make_shared<zenasset::ImagePixels>();
    pixels->width                                 = 1;
    pixels->height                                = 1;
    pixels->bytes                                 = {1, 2, 3, 4};
    scene.images.push_back({"owned image", pixels});
    pixels.reset();
    zenasset::Scene copy = scene;
    scene.images.clear();
    Check(copy.images[0].pixels->bytes[2] == 3, "A copied scene must retain its immutable image owner", failures);
    std::shared_ptr<zenasset::ImagePixels> blocks = std::make_shared<zenasset::ImagePixels>();
    blocks->width                                 = 5;
    blocks->height                                = 7;
    blocks->format                                = zenasset::PixelFormat::BC7Unorm;
    blocks->bytes.assign(64, 0x40);
    blocks->mips = {blocks->bytes, std::vector<uint8_t>(16, 0x40), std::vector<uint8_t>(16, 0x40)};
    zenasset::Scene compressed;
    compressed.images.push_back({"BC7 image", blocks});
    zenasset::TextureAsset texture;
    texture.image  = zenasset::ImageId{0};
    texture.format = zenasset::PixelFormat::BC7Srgb;
    compressed.textures.push_back(texture);
    Check(zenasset::ValidateSceneAsset(compressed), "Block footprints and BC7 sRGB view validate", failures);
    blocks->bytes[16] = blocks->mips[0][16] = 0;
    Check(!zenasset::ValidateSceneAsset(compressed), "A reserved mode in a nonfirst BC7 block is rejected", failures);
    blocks->bytes[16] = blocks->mips[0][16] = 0x40;
    blocks->mips.back()[0]                  = 0;
    Check(!zenasset::ValidateSceneAsset(compressed), "A reserved mode in a tiny BC7 mip is rejected", failures);
    blocks->mips.back()[0] = 0x40;
    blocks->mips.back().pop_back();
    Check(!zenasset::ValidateSceneAsset(compressed), "A truncated tiny BC7 mip is rejected", failures);
    blocks->mips.back().push_back(0x40);
    blocks->mips[0][0] ^= 1;
    Check(!zenasset::ValidateSceneAsset(compressed), "Duplicate base mip must match its base payload", failures);
    blocks->mips[0]               = blocks->bytes;
    compressed.textures[0].format = zenasset::PixelFormat::RGBA8Srgb;
    Check(!zenasset::ValidateSceneAsset(compressed), "Block storage cannot use an uncompressed view", failures);
    compressed.textures[0].format = zenasset::PixelFormat::BC7Srgb;
    blocks->mips.resize(1);
    Check(zenasset::ValidateSceneAsset(compressed), "Partial authored BC7 chains remain valid", failures);
    std::shared_ptr<zenasset::ImagePixels> zero = std::make_shared<zenasset::ImagePixels>();
    zero->width = zero->height = 1;
    zero->bytes.resize(4, 0);
    compressed.images[0].pixels   = zero;
    compressed.textures[0].format = zenasset::PixelFormat::RGBA8Unorm;
    Check(zenasset::ValidateSceneAsset(compressed), "Zero RGBA bytes remain valid uncompressed pixels", failures);
    return failures == 0 ? 0 : 1;
}
