#include "Codecs/BlockDecoding.h"
#include "Format/BlockValidation.h"
#include <cmath>
#include <cstring>
#define BCDEC_STATIC
#define BCDEC_BC4BC5_PRECISE
#define BCDEC_IMPLEMENTATION
#include <bcdec.h>

namespace zenasset::detail
{
Status DecodeBlock(PixelFormat format, std::span<const std::byte> encoded, std::array<Vec4, 16>& output)
{
    Status status;
    output.fill({});
    PixelFormatInfo info;
    if (!GetPixelFormatInfo(format, info) || !info.compressed)
    {
        status = {StatusCode::UnsupportedFeature, "Unsupported compressed block format"};
    }
    else if (encoded.size() != info.bytesPerBlock)
    {
        status = {StatusCode::InvalidArgument, "Compressed block has an incorrect byte count"};
    }
    else
    {
        alignas(16) std::array<uint64_t, 2> block{};
        std::memcpy(block.data(), encoded.data(), encoded.size());
        const uint8_t prefix = std::to_integer<uint8_t>(encoded[0]);
        const bool    bc7    = format == PixelFormat::BC7Unorm || format == PixelFormat::BC7Srgb;
        const bool    bc6h   = format == PixelFormat::BC6HUfloat || format == PixelFormat::BC6HSfloat;
        if (!ValidBlockPrefix(format, prefix))
        {
            status = {StatusCode::InvalidData, "Compressed block uses a reserved mode"};
        }
        else
        {
            std::array<float, 48>   values{};
            std::array<uint8_t, 64> bytes{};
            uint32_t                channels = 0;
            if (bc7)
            {
                bcdec_bc7(block.data(), bytes.data(), 16);
            }
            else if (bc6h)
            {
                channels = 3;
                bcdec_bc6h_float(block.data(), values.data(), 12, format == PixelFormat::BC6HSfloat);
            }
            else if (format == PixelFormat::BC5Unorm || format == PixelFormat::BC5Snorm)
            {
                channels = 2;
                bcdec_bc5_float(block.data(), values.data(), 8, format == PixelFormat::BC5Snorm);
            }
            else
            {
                channels = 1;
                bcdec_bc4_float(block.data(), values.data(), 4, format == PixelFormat::BC4Snorm);
            }
            for (size_t index = 0; index < output.size(); ++index)
            {
                if (bc7)
                {
                    output[index] = {bytes[index * 4] / 255.0f, bytes[index * 4 + 1] / 255.0f, bytes[index * 4 + 2] / 255.0f,
                                     bytes[index * 4 + 3] / 255.0f};
                }
                else
                {
                    output[index] = {values[index * channels], channels > 1 ? values[index * channels + 1] : 0.0f,
                                     channels > 2 ? values[index * channels + 2] : 0.0f, 1.0f};
                }
                if (!std::isfinite(output[index].x) || !std::isfinite(output[index].y) || !std::isfinite(output[index].z))
                {
                    status = {StatusCode::InvalidData, "Compressed block produced nonfinite pixels"};
                }
            }
        }
    }
    if (!status.Succeeded())
    {
        output.fill({});
    }
    return status;
}
} // namespace zenasset::detail
