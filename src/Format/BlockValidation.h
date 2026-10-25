#pragma once
#include "ZenAsset/Format/Image.h"
#include <span>

namespace zenasset::detail
{
// BC7's unary mode occupies the low byte; zero is reserved. BC6H has four
// reserved five-bit mode values. These are the same exclusions used by the
// pinned block decoder, without decoding pixels or taking a codec dependency.
inline bool ValidBlockPrefix(PixelFormat format, uint8_t prefix)
{
    const bool    bc7  = format == PixelFormat::BC7Unorm || format == PixelFormat::BC7Srgb;
    const bool    bc6h = format == PixelFormat::BC6HUfloat || format == PixelFormat::BC6HSfloat;
    const uint8_t mode = prefix & 31;
    return !(bc7 && prefix == 0) && !(bc6h && (mode == 19 || mode == 23 || mode == 27 || mode == 31));
}

inline bool ValidBlockPayload(PixelFormat format, std::span<const uint8_t> bytes)
{
    bool valid = true;
    if (format == PixelFormat::BC7Unorm || format == PixelFormat::BC7Srgb || format == PixelFormat::BC6HUfloat
        || format == PixelFormat::BC6HSfloat)
    {
        valid = bytes.size() % 16 == 0;
        for (size_t offset = 0; valid && offset < bytes.size(); offset += 16)
        {
            valid = ValidBlockPrefix(format, bytes[offset]);
        }
    }
    return valid;
}
} // namespace zenasset::detail
