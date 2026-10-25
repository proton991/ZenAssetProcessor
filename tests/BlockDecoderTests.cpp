#include "Codecs/BlockDecoding.h"
#include <cstdio>

int main()
{
    using namespace zenasset;
    std::array<std::byte, 16> block{};
    std::array<Vec4, 16>      pixels{};
    block[0]   = std::byte{255};
    bool valid = detail::DecodeBlock(PixelFormat::BC4Unorm, {block.data(), 8}, pixels).Succeeded();
    for (const Vec4& pixel : pixels)
    {
        valid = valid && pixel.x == 1.0f && pixel.y == 0.0f && pixel.z == 0.0f && pixel.w == 1.0f;
    }
    block[0] = std::byte{129};
    block[8] = std::byte{127};
    valid    = valid && detail::DecodeBlock(PixelFormat::BC5Snorm, block, pixels).Succeeded();
    for (const Vec4& pixel : pixels)
    {
        valid = valid && pixel.x == -1.0f && pixel.y == 1.0f && pixel.z == 0.0f && pixel.w == 1.0f;
    }
    block.fill({});
    valid    = valid && detail::DecodeBlock(PixelFormat::BC6HUfloat, block, pixels).Succeeded();
    valid    = valid && detail::DecodeBlock(PixelFormat::BC7Unorm, block, pixels).code == StatusCode::InvalidData;
    block[0] = std::byte{64};
    valid    = valid && detail::DecodeBlock(PixelFormat::BC7Unorm, block, pixels).Succeeded();
    valid = valid && detail::DecodeBlock(PixelFormat::BC7Unorm, {block.data(), 15}, pixels).code == StatusCode::InvalidArgument;
    for (const Vec4& pixel : pixels)
    {
        valid = valid && pixel.x == 0.0f && pixel.y == 0.0f && pixel.z == 0.0f && pixel.w == 0.0f;
    }
    std::printf("Decoder-only block fixtures: %s\n", valid ? "PASS" : "FAIL");
    return valid ? 0 : 1;
}
