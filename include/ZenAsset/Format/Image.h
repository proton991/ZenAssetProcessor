#pragma once
#include <span>
#include "ZenAsset/Format/AssetId.h"
#include "ZenAsset/Format/Math.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace zenasset
{
// Library-owned values, unrelated to a graphics API's format enumeration.
// Declaring a format does not imply an encoder or renderer supports it.
enum class PixelFormat : uint32_t
{
    Undefined   = 0,
    R8Unorm     = 1,
    RG8Unorm    = 2,
    RGBA8Unorm  = 3,
    RGBA8Srgb   = 4,
    RGBA16Float = 5,
    RGBA32Float = 6,
    BC4Unorm    = 7,
    BC4Snorm    = 8,
    BC5Unorm    = 9,
    BC5Snorm    = 10,
    BC6HUfloat  = 11,
    BC6HSfloat  = 12,
    BC7Unorm    = 13,
    BC7Srgb     = 14
};

struct PixelFormatInfo
{
    uint32_t blockWidth{0};
    uint32_t blockHeight{0};
    uint32_t bytesPerBlock{0};
    bool     srgb{false};
    bool     compressed{false};
};

// Unknown formats reset info and return false. Bytes are tightly packed, with
// each compressed mip occupying at least one whole block. Overflow/zero sizes
// reset byteSize and return false; callers enforce their own allocation budget.
bool GetPixelFormatInfo(PixelFormat format, PixelFormatInfo& info);
bool GetImageLevelByteSize(PixelFormat format, uint32_t width, uint32_t height, uint64_t& byteSize);
bool GetImageArrayLevelByteSize(PixelFormat format, uint32_t width, uint32_t height, uint32_t layers, uint64_t& byteSize);
bool ArePixelFormatsViewCompatible(PixelFormat storage, PixelFormat view);

enum class TextureFilter : uint32_t
{
    Nearest     = 0,
    Linear      = 1,
    Unspecified = UINT32_MAX
};

enum class SamplerAddressMode : uint32_t
{
    Repeat            = 0,
    MirroredRepeat    = 1,
    ClampToEdge       = 2,
    ClampToBorder     = 3,
    MirrorClampToEdge = 4,
    Unspecified       = UINT32_MAX
};

struct SamplerParameters
{
    TextureFilter      minFilter{TextureFilter::Unspecified};
    TextureFilter      magFilter{TextureFilter::Unspecified};
    TextureFilter      mipFilter{TextureFilter::Linear};
    bool               useMipmaps{true};
    SamplerAddressMode wrapS{SamplerAddressMode::Unspecified};
    SamplerAddressMode wrapT{SamplerAddressMode::Unspecified};
};

struct SamplerAsset : SamplerParameters
{
    std::string name;
};

struct ImagePixels
{
    uint32_t    width{0};
    uint32_t    height{0};
    PixelFormat format{PixelFormat::RGBA8Unorm};
    // Base level, tightly packed in format. When authored mips are present,
    // mips[0] is also the base level; mips may be a partial chain.
    std::vector<uint8_t>              bytes;
    std::vector<std::vector<uint8_t>> mips;
    // Each level contains tightly packed layers in ascending layer order. Array
    // pages require explicit mips, including level zero; no implicit generation.
    uint32_t arrayLayers{1};
};

// Select one layer without copying or decoding it. Failure clears the view.
// Validates the selected level's complete storage size, not block contents.
bool GetImageLayerMipBytes(const ImagePixels& pixels, uint32_t layer, uint32_t mip, std::span<const uint8_t>& bytes);

// Owned image pixels become immutable after processing and may be shared by
// Scene copies. No pointer points into an importer or a mapped source file.
struct ImageAsset
{
    std::string                        name;
    std::shared_ptr<const ImagePixels> pixels;
};

// Immutable environment storage shared by Scene copies. Faces are indexed by
// mip * 6 + (+X, -X, +Y, -Y, +Z, -Z). Authored IBL mips encode roughness and may
// form a partial chain; ordinary image mip generation must not replace them.
// Exactly one payload array is populated: typed linear RGBA for RGBA32Float,
// or tightly packed 4x4 blocks for BC6HUfloat. No native float object bytes are
// used as the wire representation.
struct EnvironmentCubePixels
{
    PixelFormat                       format{PixelFormat::RGBA32Float};
    uint32_t                          size{0};
    uint32_t                          mipLevels{0};
    std::vector<std::vector<Vec4>>    rgba32FloatMipFaces;
    std::vector<std::vector<uint8_t>> blockMipFaces;
};

enum class TextureInterpretation : uint32_t
{
    Default = 0,
    // Linear BC5 stores normalized tangent-space XY. Reconstruct positive Z
    // before applying the material's XY normal scale and normalizing again.
    NormalXYPositiveZ = 1
};

// Integer interior rectangle in one physical image level. All zero means the
// complete image. It is separate from authored material UV transformations.
struct ImageRegion
{
    uint32_t x{0};
    uint32_t y{0};
    uint32_t width{0};
    uint32_t height{0};
    bool     operator==(const ImageRegion&) const = default;
};

// Accepts the all-zero full-image sentinel or a nonempty contained rectangle.
// Uses subtraction for extent checks so hostile coordinates cannot wrap.
bool IsValidImageRegion(const ImageRegion& region, uint32_t width, uint32_t height);

struct TextureAsset
{
    std::string name;
    ImageId     image;
    SamplerId   sampler;
    TextureId   linearSource;
    // View format is independent of storage so compatible color/data uses can
    // share one image. Adapters must map the enum, never cast to a GPU enum.
    PixelFormat           format{PixelFormat::RGBA8Unorm};
    bool                  flatNormal{false};
    TextureInterpretation interpretation{TextureInterpretation::Default};
    uint32_t              arrayLayer{0};
    // Texture resource v4: a guarded, single-level RGBA8 spatial page. Current
    // scene validation requires power-of-two tile/page extents, four-texel
    // aligned origins/gutters, explicit linear/clamp sampling and no arrays.
    // This record alone does not establish renderer support or eligibility.
    ImageRegion atlasRegion;
};
} // namespace zenasset
