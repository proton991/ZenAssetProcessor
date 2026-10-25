#include "ZenAsset/Reader/Image.h"
#include <algorithm>
#include <array>
#include <cstdio>

namespace
{
using namespace zenasset;

// BC7 mode 6, equal endpoints (200, 40, 20, 128), zero indices.
constexpr std::array<uint8_t, 16> kColorBlock{0x40, 0x32, 0x99, 0x42, 0x51, 0x28, 0x80, 0x40, 0, 0, 0, 0, 0, 0, 0, 0};

bool Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::printf("FAIL: %s\n", message);
    }
    return condition;
}

bool HasColor(const ImagePreviewResult& preview, const std::array<uint8_t, 4>& color)
{
    bool valid = preview.status.Succeeded() && preview.pixels.size() == size_t(preview.width) * preview.height * 4;
    for (size_t offset = 0; valid && offset < preview.pixels.size(); offset += 4)
    {
        valid = std::equal(color.begin(), color.end(), preview.pixels.begin() + offset);
    }
    return valid;
}

bool IsFailure(const ImagePreviewResult& preview, StatusCode code)
{
    return preview.status.code == code && preview.width == 0 && preview.height == 0 && preview.pixels.empty();
}

bool PollCancellation(void* data)
{
    uint32_t& calls = *static_cast<uint32_t*>(data);
    ++calls;
    return calls >= 5;
}

bool CheckRgba()
{
    const std::array<uint8_t, 16> pixels{0, 20, 40, 0, 100, 40, 80, 64, 200, 60, 120, 128, 100, 80, 160, 128};
    ImageMipView                  mip{PixelFormat::RGBA8Srgb, 2, 2, std::as_bytes(std::span(pixels))};
    ImagePreviewOptions           options;
    options.maxDimension             = 1;
    options.maxOutputBytes           = 4;
    const ImagePreviewResult reduced = MakeImagePreview(mip, options);
    bool                     valid   = Check(reduced.width == 1 && reduced.height == 1 && HasColor(reduced, {100, 50, 100, 80}),
                                             "RGBA stored-transfer and independent alpha average");
    options.opaqueAlpha              = true;
    valid = Check(HasColor(MakeImagePreview(mip, options), {100, 50, 100, 255}), "opaque preview alpha") && valid;
    options.maxDimension              = 20;
    options.maxOutputBytes            = pixels.size();
    const ImagePreviewResult original = MakeImagePreview(mip, options);
    valid = Check(original.width == 2 && original.height == 2 && original.pixels[4] == 100, "preview never upscales") && valid;
    options.opaqueAlpha = false;
    mip                 = {PixelFormat::RGBA8Unorm, 1, 1, std::as_bytes(std::span(pixels)).first(4)};
    valid               = Check(HasColor(MakeImagePreview(mip, options), {0, 20, 40, 0}), "one texel preserves alpha") && valid;
    return valid;
}

bool CheckBc7()
{
    std::vector<uint8_t> blocks;
    for (uint32_t block = 0; block < 4; ++block)
    {
        blocks.insert(blocks.end(), kColorBlock.begin(), kColorBlock.end());
    }
    ImageMipView        mip{PixelFormat::BC7Srgb, 5, 7, std::as_bytes(std::span(blocks))};
    ImagePreviewOptions options;
    options.maxDimension         = 3;
    options.maxOutputBytes       = 24;
    const ImagePreviewResult odd = MakeImagePreview(mip, options);
    bool                     valid =
        Check(odd.width == 2 && odd.height == 3 && HasColor(odd, {200, 40, 20, 128}), "odd BC7 extent and aspect rounding");
    for (const PixelFormat format : {PixelFormat::BC7Unorm, PixelFormat::BC7Srgb})
    {
        for (uint32_t dimension = 1; dimension <= 4; ++dimension)
        {
            mip                           = {format, dimension, 1, std::as_bytes(std::span(kColorBlock))};
            options.maxDimension          = 128;
            options.maxOutputBytes        = 16;
            const ImagePreviewResult tiny = MakeImagePreview(mip, options);
            valid = Check(tiny.width == dimension && tiny.height == 1 && HasColor(tiny, {200, 40, 20, 128}),
                          "tiny BC7 mip is one complete block")
                 && valid;
        }
    }
    return valid;
}

bool CheckBoundedSampling()
{
    // Only the 4x4 sampled blocks are valid. All other blocks have reserved mode
    // prefixes, so a full image decode would fail instead of producing a preview.
    constexpr uint32_t   dimension    = 512;
    constexpr uint32_t   blocksPerRow = dimension / 4;
    std::vector<uint8_t> blocks(size_t(blocksPerRow) * blocksPerRow * 16);
    for (uint32_t y = 64; y < dimension; y += 128)
    {
        for (uint32_t x = 64; x < dimension; x += 128)
        {
            const size_t offset = (size_t(y / 4) * blocksPerRow + x / 4) * 16;
            std::copy(kColorBlock.begin(), kColorBlock.end(), blocks.begin() + offset);
        }
    }
    const ImageMipView  mip{PixelFormat::BC7Unorm, dimension, dimension, std::as_bytes(std::span(blocks))};
    ImagePreviewOptions options;
    options.maxDimension   = 1;
    options.maxOutputBytes = 4;
    bool valid =
        Check(HasColor(MakeImagePreview(mip, options), {200, 40, 20, 128}), "bounded preview decodes only sampled blocks");
    options.maxDimension   = dimension;
    options.maxOutputBytes = size_t(dimension) * dimension * 4;
    valid                  = Check(IsFailure(MakeImagePreview(mip, options), StatusCode::InvalidData),
                                   "sampled reserved BC7 mode fails and clears output")
                          && valid;
    return valid;
}

bool CheckScalarAndNormal()
{
    // Equal endpoints and index zero need no encoder in this Reader-only test.
    const std::array<uint8_t, 8>  scalar{64, 64, 0, 0, 0, 0, 0, 0};
    const std::array<uint8_t, 16> normal{128, 128, 0, 0, 0, 0, 0, 0, 128, 128, 0, 0, 0, 0, 0, 0};
    bool                          valid = true;
    for (const PixelFormat format : {PixelFormat::BC4Unorm, PixelFormat::BC5Unorm})
    {
        const std::span<const uint8_t> block =
            format == PixelFormat::BC4Unorm ? std::span<const uint8_t>(scalar) : std::span<const uint8_t>(normal);
        const TextureInterpretation interpretation =
            format == PixelFormat::BC4Unorm ? TextureInterpretation::Default : TextureInterpretation::NormalXYPositiveZ;
        const std::array<uint8_t, 4> color = format == PixelFormat::BC4Unorm ? std::array<uint8_t, 4>{64, 64, 64, 255}
                                                                             : std::array<uint8_t, 4>{128, 128, 255, 255};
        std::vector<uint8_t>         blocks;
        for (uint32_t index = 0; index < 4; ++index)
        {
            blocks.insert(blocks.end(), block.begin(), block.end());
        }
        ImageMipView        mip{format, 5, 7, std::as_bytes(std::span(blocks)), interpretation};
        ImagePreviewOptions options;
        options.maxDimension          = 3;
        options.maxOutputBytes        = 24;
        const ImagePreviewResult odd  = MakeImagePreview(mip, options);
        valid                        &= Check(odd.width == 2 && odd.height == 3 && HasColor(odd, color),
                                              "odd scalar/normal preview uses correct block stride and bounded output");
        options.maxOutputBytes        = 23;
        valid                        &= Check(IsFailure(MakeImagePreview(mip, options), StatusCode::ResourceLimit),
                                              "scalar/normal output budget enforced before allocation");
        options.maxOutputBytes        = 24;
        uint32_t     calls            = 0;
        Cancellation polled(PollCancellation, &calls);
        valid &= Check(IsFailure(MakeImagePreview(mip, options, &polled), StatusCode::Cancelled) && calls == 5,
                       "scalar/normal sampling cancellation clears all output");
        for (uint32_t width = 1; width <= 4; ++width)
        {
            mip                  = {format, width, 1, std::as_bytes(block), interpretation};
            options.maxDimension = 128;
            valid &= Check(HasColor(MakeImagePreview(mip, options), color), "tiny scalar/normal mip uses one whole block");
        }
        mip.bytes  = mip.bytes.first(block.size() - 1);
        valid     &= Check(IsFailure(MakeImagePreview(mip, options), StatusCode::InvalidArgument),
                           "truncated scalar/normal block rejected");
        mip.bytes  = std::as_bytes(std::span(blocks)).first(block.size() + 1);
        valid     &= Check(IsFailure(MakeImagePreview(mip, options), StatusCode::InvalidArgument),
                           "scalar/normal trailing bytes rejected");
    }
    std::array<uint8_t, 16>  scalarPair{64, 64, 0, 0, 0, 0, 0, 0, 192, 192, 0, 0, 0, 0, 0, 0};
    const ImagePreviewResult pair    = MakeImagePreview({PixelFormat::BC4Unorm, 5, 1, std::as_bytes(std::span(scalarPair))});
    valid                           &= Check(pair.status.Succeeded() && pair.pixels[0] == 64 && pair.pixels[16] == 192,
                                             "BC4 eight-byte block stride reaches the second block");
    std::array<uint8_t, 16> outside  = normal;
    outside[0] = outside[1] = outside[8] = outside[9] = 255;
    valid &= Check(HasColor(MakeImagePreview({PixelFormat::BC5Unorm, 1, 1, std::as_bytes(std::span(outside)),
                                              TextureInterpretation::NormalXYPositiveZ}),
                            {218, 218, 128, 255}),
                   "out-of-disk normal XY clamps reconstructed Z and normalizes the direction");
    ImageMipView mismatched{PixelFormat::BC4Unorm, 1, 1, std::as_bytes(std::span(scalar)),
                            TextureInterpretation::NormalXYPositiveZ};
    valid      &= Check(IsFailure(MakeImagePreview(mismatched), StatusCode::InvalidArgument),
                        "normal interpretation is rejected on scalar storage");
    mismatched  = {PixelFormat::BC5Unorm, 1, 1, std::as_bytes(std::span(normal)), static_cast<TextureInterpretation>(999)};
    valid &= Check(IsFailure(MakeImagePreview(mismatched), StatusCode::InvalidArgument), "unknown interpretation rejected");
    return valid;
}

bool CheckRegions()
{
    // A crop crossing both block axes must keep the physical page row stride.
    std::vector<uint8_t> blocks;
    for (uint8_t endpoint : {64, 128, 192, 255})
    {
        const std::array<uint8_t, 8> block{endpoint, endpoint, 0, 0, 0, 0, 0, 0};
        blocks.insert(blocks.end(), block.begin(), block.end());
    }
    ImageMipView               mip{PixelFormat::BC4Unorm,          8,           8, std::as_bytes(std::span(blocks)),
                                   TextureInterpretation::Default, {3, 3, 2, 2}};
    const ImagePreviewResult   crop = MakeImagePreview(mip);
    const std::vector<uint8_t> expected{64, 64, 64, 255, 128, 128, 128, 255, 192, 192, 192, 255, 255, 255, 255, 255};
    bool                valid = Check(crop.status.Succeeded() && crop.width == 2 && crop.height == 2 && crop.pixels == expected,
                                      "Region crop samples all four physical blocks without including neighboring texels");
    ImagePreviewOptions options;
    options.maxDimension    = 1;
    options.maxOutputBytes  = 4;
    valid                  &= Check(HasColor(MakeImagePreview(mip, options), {160, 160, 160, 255}),
                                    "Region downsampling uses logical dimensions and stays inside the selected crop");
    options.maxOutputBytes  = 3;
    valid                  &= Check(IsFailure(MakeImagePreview(mip, options), StatusCode::ResourceLimit),
                                    "Cropped preview output budget is enforced");
    for (const ImageRegion& invalid :
         {ImageRegion{0, 0, 0, 2}, ImageRegion{0, 0, 2, 0}, ImageRegion{1, 0, 0, 0}, ImageRegion{0, 1, 0, 0},
          ImageRegion{7, 0, 2, 1}, ImageRegion{0, 7, 1, 2}, ImageRegion{UINT32_MAX, 0, 2, 2}, ImageRegion{0, UINT32_MAX, 2, 2},
          ImageRegion{1, 1, UINT32_MAX, 2}, ImageRegion{1, 1, 2, UINT32_MAX}})
    {
        mip.region  = invalid;
        valid      &= Check(IsFailure(MakeImagePreview(mip), StatusCode::InvalidArgument),
                            "Empty, incomplete, overflowing and out-of-page regions fail without partial previews");
    }
    mip.region  = {3, 3, 2, 2};
    mip.bytes   = mip.bytes.first(8);
    valid      &= Check(IsFailure(MakeImagePreview(mip), StatusCode::InvalidArgument),
                        "Region input validation requires the complete physical mip storage");
    mip.bytes   = std::as_bytes(std::span(blocks));
    Cancellation cancelled;
    cancelled.Cancel();
    valid &=
        Check(IsFailure(MakeImagePreview(mip, {}, &cancelled), StatusCode::Cancelled), "Cropped preview honors cancellation");
    return valid;
}

bool CheckFailures()
{
    ImageMipView        mip{PixelFormat::BC7Unorm, 4, 4, std::as_bytes(std::span(kColorBlock))};
    ImagePreviewOptions options;
    options.maxOutputBytes = 63;
    bool valid             = Check(IsFailure(MakeImagePreview(mip, options), StatusCode::ResourceLimit), "output byte budget");
    options.maxOutputBytes = 64;
    valid                  = Check(MakeImagePreview(mip, options).status.Succeeded(), "exact output budget") && valid;
    options.maxDimension   = 0;
    valid = Check(IsFailure(MakeImagePreview(mip, options), StatusCode::InvalidArgument), "zero preview bound") && valid;
    options.maxDimension = 128;
    mip.bytes            = mip.bytes.first(15);
    valid = Check(IsFailure(MakeImagePreview(mip, options), StatusCode::InvalidArgument), "truncated block") && valid;
    std::array<std::byte, 17> trailing{};
    mip.bytes  = trailing;
    valid      = Check(IsFailure(MakeImagePreview(mip, options), StatusCode::InvalidArgument), "trailing mip bytes") && valid;
    mip.bytes  = std::as_bytes(std::span(kColorBlock));
    mip.width  = 0;
    valid      = Check(IsFailure(MakeImagePreview(mip, options), StatusCode::InvalidArgument), "zero mip extent") && valid;
    mip.width  = UINT32_MAX;
    mip.height = UINT32_MAX;
    valid =
        Check(IsFailure(MakeImagePreview(mip, options), StatusCode::InvalidArgument), "enormous invalid footprint") && valid;
    mip   = {PixelFormat::BC5Unorm, 4, 4, std::as_bytes(std::span(kColorBlock))};
    valid = Check(IsFailure(MakeImagePreview(mip, options), StatusCode::UnsupportedFeature), "normal format not interpreted")
         && valid;
    mip.format = PixelFormat::BC7Unorm;
    Cancellation cancelled;
    cancelled.Cancel();
    valid =
        Check(IsFailure(MakeImagePreview(mip, options, &cancelled), StatusCode::Cancelled), "initial cancellation") && valid;
    uint32_t     calls = 0;
    Cancellation polled(PollCancellation, &calls);
    valid = Check(IsFailure(MakeImagePreview(mip, options, &polled), StatusCode::Cancelled) && calls == 5,
                  "cancellation during sampling clears output")
         && valid;
    return valid;
}
} // namespace

int main()
{
    bool valid = CheckRgba();
    valid      = CheckBc7() && valid;
    valid      = CheckBoundedSampling() && valid;
    valid      = CheckScalarAndNormal() && valid;
    valid      = CheckFailures() && valid;
    valid      = CheckRegions() && valid;
    std::printf("Bounded image preview fixtures: %s\n", valid ? "PASS" : "FAIL");
    return valid ? 0 : 1;
}
