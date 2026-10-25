#include "ZenAsset/Reader/Image.h"
#include "Codecs/BlockDecoding.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace zenasset
{
namespace
{
struct PreviewBlock
{
    uint32_t             x{UINT32_MAX};
    uint32_t             y{UINT32_MAX};
    std::array<Vec4, 16> pixels{};
};

Status PreviewTexel(const ImageMipView&     mip,
                    uint32_t                x,
                    uint32_t                y,
                    PreviewBlock&           cache,
                    std::array<uint8_t, 4>& color,
                    const Cancellation*     cancellation)
{
    Status     status;
    const bool compressed = mip.format != PixelFormat::RGBA8Unorm && mip.format != PixelFormat::RGBA8Srgb;
    if (cancellation && cancellation->IsCancelled())
    {
        status = {StatusCode::Cancelled, "Image preview cancelled"};
    }
    else if (!compressed)
    {
        const size_t offset = (size_t(y) * mip.width + x) * 4;
        for (size_t channel = 0; channel < 4; ++channel)
        {
            color[channel] = std::to_integer<uint8_t>(mip.bytes[offset + channel]);
        }
    }
    else
    {
        const uint32_t blockX = x / 4;
        const uint32_t blockY = y / 4;
        if (cache.x != blockX || cache.y != blockY)
        {
            const uint64_t blocksPerRow = (uint64_t(mip.width) + 3) / 4;
            const size_t   blockBytes   = mip.format == PixelFormat::BC4Unorm ? 8 : 16;
            const uint64_t offset       = (uint64_t(blockY) * blocksPerRow + blockX) * blockBytes;
            status  = detail::DecodeBlock(mip.format, mip.bytes.subspan(static_cast<size_t>(offset), blockBytes), cache.pixels);
            cache.x = blockX;
            cache.y = blockY;
        }
        if (status.Succeeded())
        {
            Vec4 pixel = cache.pixels[(y % 4) * 4 + x % 4];
            if (mip.format == PixelFormat::BC4Unorm)
            {
                pixel = {pixel.x, pixel.x, pixel.x, 1};
            }
            else if (mip.format == PixelFormat::BC5Unorm)
            {
                const double nx     = double(pixel.x) * 2 - 1;
                const double ny     = double(pixel.y) * 2 - 1;
                const double nz     = std::sqrt(std::max(0.0, 1 - nx * nx - ny * ny));
                const double length = std::sqrt(nx * nx + ny * ny + nz * nz);
                pixel               = {static_cast<float>(nx / length * 0.5 + 0.5), static_cast<float>(ny / length * 0.5 + 0.5),
                                       static_cast<float>(nz / length * 0.5 + 0.5), 1};
            }
            const std::array<float, 4> values{pixel.x, pixel.y, pixel.z, pixel.w};
            for (size_t channel = 0; channel < 4; ++channel)
            {
                color[channel] = static_cast<uint8_t>(std::lround(std::clamp(values[channel], 0.0f, 1.0f) * 255.0f));
            }
        }
    }
    return status;
}

uint32_t PreviewCoordinate(uint32_t output, uint32_t tap, uint32_t taps, uint32_t inputSize, uint32_t outputSize)
{
    const double   coordinate = (double(output) + (double(tap) + 0.5) / taps) * inputSize / outputSize;
    const uint32_t result     = static_cast<uint32_t>(std::min(double(inputSize - 1), coordinate));
    return result;
}
} // namespace

ImagePreviewResult MakeImagePreview(const ImageMipView&        mip,
                                    const ImagePreviewOptions& options,
                                    const Cancellation*        cancellation)
{
    ImagePreviewResult result;
    uint64_t           inputBytes    = 0;
    uint64_t           outputBytes   = 0;
    uint32_t           taps          = 1;
    const uint32_t     logicalWidth  = mip.region == ImageRegion{} ? mip.width : mip.region.width;
    const uint32_t     logicalHeight = mip.region == ImageRegion{} ? mip.height : mip.region.height;
    const bool         supported     = mip.format == PixelFormat::RGBA8Unorm || mip.format == PixelFormat::RGBA8Srgb
                                    || mip.format == PixelFormat::BC7Unorm || mip.format == PixelFormat::BC7Srgb
                                    || mip.format == PixelFormat::BC4Unorm || mip.format == PixelFormat::BC5Unorm;
    if (mip.interpretation != TextureInterpretation::Default && mip.interpretation != TextureInterpretation::NormalXYPositiveZ)
    {
        result.status = {StatusCode::InvalidArgument, "Unknown image preview interpretation"};
    }
    else if (!supported)
    {
        result.status = {StatusCode::UnsupportedFeature, "Image preview supports RGBA8, BC7, BC4 and marked BC5 normals"};
    }
    else if (mip.format == PixelFormat::BC5Unorm && mip.interpretation != TextureInterpretation::NormalXYPositiveZ)
    {
        result.status = {StatusCode::UnsupportedFeature, "BC5 preview requires an explicit XY normal interpretation"};
    }
    else if (mip.format != PixelFormat::BC5Unorm && mip.interpretation != TextureInterpretation::Default)
    {
        result.status = {StatusCode::InvalidArgument, "Image preview interpretation does not match its format"};
    }
    else if (!GetImageLevelByteSize(mip.format, mip.width, mip.height, inputBytes) || inputBytes != mip.bytes.size()
             || options.maxDimension == 0 || !IsValidImageRegion(mip.region, mip.width, mip.height))
    {
        result.status = {StatusCode::InvalidArgument, "Invalid image mip extent, storage size or preview dimension"};
    }
    else if (cancellation && cancellation->IsCancelled())
    {
        result.status = {StatusCode::Cancelled, "Image preview cancelled"};
    }
    else
    {
        const uint32_t longest = std::max(logicalWidth, logicalHeight);
        const uint32_t bound   = std::min(longest, options.maxDimension);
        result.width           = std::max(1u, static_cast<uint32_t>((uint64_t(logicalWidth) * bound + longest / 2) / longest));
        result.height          = std::max(1u, static_cast<uint32_t>((uint64_t(logicalHeight) * bound + longest / 2) / longest));
        taps                   = static_cast<uint32_t>(std::min(uint64_t(4), (uint64_t(longest) + bound - 1) / bound));
        if (!GetImageLevelByteSize(PixelFormat::RGBA8Unorm, result.width, result.height, outputBytes)
            || outputBytes > options.maxOutputBytes || outputBytes > std::numeric_limits<size_t>::max())
        {
            result.status = {StatusCode::ResourceLimit, "Image preview exceeds output byte budget"};
        }
    }
    if (result.status.Succeeded())
    {
        result.pixels.resize(static_cast<size_t>(outputBytes));
        PreviewBlock cache;
        for (uint32_t y = 0; result.status.Succeeded() && y < result.height; ++y)
        {
            for (uint32_t x = 0; result.status.Succeeded() && x < result.width; ++x)
            {
                std::array<uint32_t, 4> sum{};
                for (uint32_t tapY = 0; result.status.Succeeded() && tapY < taps; ++tapY)
                {
                    for (uint32_t tapX = 0; result.status.Succeeded() && tapX < taps; ++tapX)
                    {
                        std::array<uint8_t, 4> color{};
                        result.status =
                            PreviewTexel(mip, mip.region.x + PreviewCoordinate(x, tapX, taps, logicalWidth, result.width),
                                         mip.region.y + PreviewCoordinate(y, tapY, taps, logicalHeight, result.height), cache,
                                         color, cancellation);
                        for (size_t channel = 0; channel < 4; ++channel)
                        {
                            sum[channel] += color[channel];
                        }
                    }
                }
                const size_t offset = (size_t(y) * result.width + x) * 4;
                for (size_t channel = 0; channel < 4; ++channel)
                {
                    result.pixels[offset + channel] = static_cast<uint8_t>((sum[channel] + taps * taps / 2) / (taps * taps));
                }
                if (options.opaqueAlpha)
                {
                    result.pixels[offset + 3] = 255;
                }
            }
        }
    }
    if (!result.status.Succeeded())
    {
        result.width  = 0;
        result.height = 0;
        std::vector<uint8_t>().swap(result.pixels);
    }
    return result;
}
} // namespace zenasset
