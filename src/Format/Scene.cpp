#include "ZenAsset/Format/Image.h"
#include <algorithm>
#include <limits>

namespace zenasset
{
bool GetPixelFormatInfo(PixelFormat format, PixelFormatInfo& info)
{
    bool supported = true;
    info           = {};
    switch (format)
    {
        case PixelFormat::R8Unorm: info = {1, 1, 1, false, false}; break;
        case PixelFormat::RG8Unorm: info = {1, 1, 2, false, false}; break;
        case PixelFormat::RGBA8Unorm: info = {1, 1, 4, false, false}; break;
        case PixelFormat::RGBA8Srgb: info = {1, 1, 4, true, false}; break;
        case PixelFormat::RGBA16Float: info = {1, 1, 8, false, false}; break;
        case PixelFormat::RGBA32Float: info = {1, 1, 16, false, false}; break;
        case PixelFormat::BC4Unorm:
        case PixelFormat::BC4Snorm: info = {4, 4, 8, false, true}; break;
        case PixelFormat::BC5Unorm:
        case PixelFormat::BC5Snorm:
        case PixelFormat::BC6HUfloat:
        case PixelFormat::BC6HSfloat:
        case PixelFormat::BC7Unorm: info = {4, 4, 16, false, true}; break;
        case PixelFormat::BC7Srgb: info = {4, 4, 16, true, true}; break;
        default: supported = false; break;
    }
    return supported;
}

bool GetImageLevelByteSize(PixelFormat format, uint32_t width, uint32_t height, uint64_t& byteSize)
{
    PixelFormatInfo info;
    bool            valid = width > 0 && height > 0 && GetPixelFormatInfo(format, info);
    byteSize              = 0;
    if (valid)
    {
        const uint64_t columns = (uint64_t(width) + info.blockWidth - 1) / info.blockWidth;
        const uint64_t rows    = (uint64_t(height) + info.blockHeight - 1) / info.blockHeight;
        const uint64_t blocks  = columns * rows;
        valid                  = blocks <= std::numeric_limits<uint64_t>::max() / info.bytesPerBlock;
        if (valid)
        {
            byteSize = blocks * info.bytesPerBlock;
        }
    }
    return valid;
}

bool GetImageArrayLevelByteSize(PixelFormat format, uint32_t width, uint32_t height, uint32_t layers, uint64_t& byteSize)
{
    uint64_t   layerBytes = 0;
    const bool valid =
        layers > 0 && GetImageLevelByteSize(format, width, height, layerBytes) && layerBytes <= UINT64_MAX / layers;
    byteSize = valid ? layerBytes * layers : 0;
    return valid;
}

bool GetImageLayerMipBytes(const ImagePixels& pixels, uint32_t layer, uint32_t mip, std::span<const uint8_t>& bytes)
{
    bytes      = {};
    bool valid = pixels.width > 0 && pixels.height > 0 && layer < pixels.arrayLayers && mip < 32
              && (std::max(pixels.width, pixels.height) >> mip) != 0
              && (pixels.mips.empty() ? mip == 0 && pixels.arrayLayers == 1 : mip < pixels.mips.size());
    if (valid)
    {
        const std::vector<uint8_t>& level = pixels.mips.empty() ? pixels.bytes : pixels.mips[mip];
        uint64_t                    total = 0;
        valid = GetImageArrayLevelByteSize(pixels.format, std::max(1u, pixels.width >> mip), std::max(1u, pixels.height >> mip),
                                           pixels.arrayLayers, total)
             && total == level.size();
        if (valid)
        {
            const size_t stride = static_cast<size_t>(total / pixels.arrayLayers);
            bytes               = std::span<const uint8_t>(level).subspan(size_t(layer) * stride, stride);
        }
    }
    return valid;
}

bool IsValidImageRegion(const ImageRegion& region, uint32_t width, uint32_t height)
{
    const bool empty     = region == ImageRegion{};
    const bool contained = region.width > 0 && region.height > 0 && region.x < width && region.y < height
                        && region.width <= width - region.x && region.height <= height - region.y;
    return width > 0 && height > 0 && (empty || contained);
}

bool ArePixelFormatsViewCompatible(PixelFormat storage, PixelFormat view)
{
    PixelFormatInfo storageInfo;
    PixelFormatInfo viewInfo;
    const bool      supported = GetPixelFormatInfo(storage, storageInfo) && GetPixelFormatInfo(view, viewInfo);
    const bool      rgba8     = (storage == PixelFormat::RGBA8Unorm || storage == PixelFormat::RGBA8Srgb)
                             && (view == PixelFormat::RGBA8Unorm || view == PixelFormat::RGBA8Srgb);
    const bool      bc7       = (storage == PixelFormat::BC7Unorm || storage == PixelFormat::BC7Srgb)
                             && (view == PixelFormat::BC7Unorm || view == PixelFormat::BC7Srgb);
    return supported && (storage == view || rgba8 || bc7);
}
} // namespace zenasset
