#include "Importers/ImageDecoder.h"
#include <webp/decode.h>
#include <basisu_transcoder.h>
#include <stb_image.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>

namespace zenasset
{
namespace
{
bool ReservePixels(uint32_t width, uint32_t height, uint32_t copies, ImportErrors& errors)
{
    const uint64_t count = uint64_t(width) * height;
    bool           valid = width > 0 && height > 0 && count <= SIZE_MAX / 4 / copies;
    if (!valid)
    {
        errors.Fail("Invalid image dimensions", SceneImportStatus::ResourceLimit);
    }
    else
    {
        valid = errors.ReserveImageBytes(count * 4 * copies);
    }
    return valid;
}
void DecodeBasis(const uint8_t*                     data,
                 size_t                             size,
                 uint32_t&                          width,
                 uint32_t&                          height,
                 ImportErrors&                      errors,
                 std::vector<uint8_t>&              pixels,
                 std::vector<std::vector<uint8_t>>* mips)
{
    static std::once_flag initialized;
    std::call_once(initialized, basist::basisu_transcoder_init);
    basist::ktx2_transcoder transcoder;
    if (size > UINT32_MAX || !transcoder.init(data, static_cast<uint32_t>(size)))
    {
        errors.Fail("Failed to initialize KTX2 Basis Universal transcoder");
    }
    else
    {
        width  = transcoder.get_width();
        height = transcoder.get_height();
        if (uint64_t(width) * height > UINT32_MAX / 4 || transcoder.get_levels() > 32 || transcoder.get_levels() == 0)
        {
            errors.Fail("Invalid KTX2 image dimensions", SceneImportStatus::ResourceLimit);
        }
        else if (ReservePixels(width, height, mips != nullptr ? 2u : 1u, errors))
        {
            for (uint32_t level = 1; mips != nullptr && level < transcoder.get_levels() && !errors.Failed(); ++level)
            {
                ReservePixels(std::max(1u, width >> level), std::max(1u, height >> level), 1, errors);
            }
            if (!errors.Failed() && !transcoder.start_transcoding())
            {
                errors.Fail("Failed to initialize KTX2 Basis Universal transcoder");
            }
            if (!errors.Failed())
            {
                pixels.resize(size_t(width) * height * 4);
                if (!transcoder.transcode_image_level(0, 0, 0, pixels.data(), width * height,
                                                      basist::transcoder_texture_format::cTFRGBA32))
                {
                    errors.Fail("Failed to transcode KTX2 Basis Universal image");
                    pixels.clear();
                }
                else if (mips != nullptr)
                {
                    mips->push_back(pixels);
                    for (uint32_t level = 1; level < transcoder.get_levels() && !errors.Failed(); ++level)
                    {
                        const uint32_t       levelWidth  = std::max(1u, width >> level);
                        const uint32_t       levelHeight = std::max(1u, height >> level);
                        std::vector<uint8_t> mip(size_t(levelWidth) * levelHeight * 4);
                        if (!transcoder.transcode_image_level(level, 0, 0, mip.data(), levelWidth * levelHeight,
                                                              basist::transcoder_texture_format::cTFRGBA32))
                        {
                            errors.Fail("Failed to transcode KTX2 Basis Universal mip level");
                        }
                        else
                        {
                            mips->push_back(std::move(mip));
                        }
                    }
                }
            }
        }
    }
}
} // namespace
std::vector<uint8_t> DecodeImagePixels(const uint8_t*                     data,
                                       size_t                             size,
                                       uint32_t&                          width,
                                       uint32_t&                          height,
                                       ImportErrors&                      errors,
                                       std::vector<std::vector<uint8_t>>* mipBytes)
{
    std::vector<uint8_t> pixels;
    static const uint8_t ktx2Identifier[]{0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
    if (size >= sizeof(ktx2Identifier) && std::memcmp(data, ktx2Identifier, sizeof(ktx2Identifier)) == 0)
    {
        DecodeBasis(data, size, width, height, errors, pixels, mipBytes);
    }
    else
    {
        int imageWidth  = 0;
        int imageHeight = 0;
        if (WebPGetInfo(data, size, &imageWidth, &imageHeight))
        {
            width  = static_cast<uint32_t>(imageWidth);
            height = static_cast<uint32_t>(imageHeight);
            if (imageWidth > std::numeric_limits<int>::max() / 4)
            {
                errors.Fail("WebP image stride exceeds the decoder limit", SceneImportStatus::ResourceLimit);
            }
            else if (ReservePixels(width, height, 1, errors))
            {
                pixels.resize(size_t(width) * height * 4);
                if (WebPDecodeRGBAInto(data, size, pixels.data(), pixels.size(), imageWidth * 4) == nullptr)
                {
                    errors.Fail("Failed to decode WebP image");
                    pixels.clear();
                }
            }
        }
        else if (size <= static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            stbi_set_flip_vertically_on_load_thread(0);
            int channels = 0;
            if (!stbi_info_from_memory(data, static_cast<int>(size), &imageWidth, &imageHeight, &channels))
            {
                const char* reason = stbi_failure_reason();
                errors.Fail(std::string("Unsupported or corrupt image (") + (reason != nullptr ? reason : "unknown format")
                            + ")");
            }
            else if (ReservePixels(static_cast<uint32_t>(imageWidth), static_cast<uint32_t>(imageHeight), 2, errors))
            {
                const int expectedWidth  = imageWidth;
                const int expectedHeight = imageHeight;
                stbi_uc*  decoded =
                    stbi_load_from_memory(data, static_cast<int>(size), &imageWidth, &imageHeight, &channels, STBI_rgb_alpha);
                if (decoded != nullptr && imageWidth == expectedWidth && imageHeight == expectedHeight)
                {
                    width  = static_cast<uint32_t>(imageWidth);
                    height = static_cast<uint32_t>(imageHeight);
                    pixels.assign(decoded, decoded + size_t(width) * height * 4);
                }
                else
                {
                    const char* reason = stbi_failure_reason();
                    errors.Fail(std::string("Unsupported or corrupt image (")
                                + (reason != nullptr ? reason : "invalid dimensions") + ")");
                }
                stbi_image_free(decoded);
            }
        }
        else
        {
            errors.Fail("Image exceeds the decoder size limit", SceneImportStatus::ResourceLimit);
        }
    }
    return pixels;
}
} // namespace zenasset
