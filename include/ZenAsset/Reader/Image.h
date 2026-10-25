#pragma once
#include "ZenAsset/Format/Image.h"
#include "ZenAsset/Format/Status.h"
#include <cstddef>
#include <span>
#include <vector>

namespace zenasset
{
struct ImageMipView
{
    PixelFormat                format{PixelFormat::Undefined};
    uint32_t                   width{0};
    uint32_t                   height{0};
    std::span<const std::byte> bytes;
    TextureInterpretation      interpretation{TextureInterpretation::Default};
    // Crop within the supplied physical mip without copying its pixel storage.
    ImageRegion region;
};

struct ImagePreviewOptions
{
    uint32_t maxDimension{128};
    uint64_t maxOutputBytes{64ull << 20};
    bool     opaqueAlpha{false};
};

struct ImagePreviewResult
{
    Status               status;
    uint32_t             width{0};
    uint32_t             height{0};
    std::vector<uint8_t> pixels;
};

// RGBA8, BC7, BC4 UNORM and explicitly marked BC5 UNORM normals. Caller owns
// one tightly packed mip for the duration of the call. BC4 displays R as opaque
// grayscale. BC5 requires NormalXYPositiveZ and displays the reconstructed,
// normalized unit-scale XYZ normal in [0,1], with opaque alpha. Other formats
// require Default interpretation; HDR/SNORM previews are not supported.
// RGBA8/BC7 output retains the stored transfer function, with alpha averaged
// independently unless opaqueAlpha is requested. This is a display preview,
// not color-correct mip generation. At most 4x4 taps per output pixel are used.
// Allocations are limited to the output; BC decoding uses one fixed block.
// Only sampled blocks are decoded/validated. This does not validate unsampled
// block contents or create a full-resolution decoded copy. Failure clears output.
// A nonzero region selects the logical preview extent; byte-size validation and
// block strides still use the complete physical mip. Every tap stays in-region.
ImagePreviewResult MakeImagePreview(const ImageMipView&        mip,
                                    const ImagePreviewOptions& options      = {},
                                    const Cancellation*        cancellation = nullptr);
} // namespace zenasset
