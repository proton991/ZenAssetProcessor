#include "Codecs/Bc6hEncoder.h"
#include <cstring>
#define BC6H_ENC_IMPLEMENTATION
#include <bc6h_enc.h>

namespace zenasset::detail
{
void EncodeBc6hBlock(const std::array<Vec4, 16>& rgba, bool signedFormat, std::array<std::byte, 16>& output)
{
    // Keep actual upstream objects alive rather than aliasing unrelated Vec4
    // and byte arrays through the library's untyped convenience entry points.
    std::array<bc6h_enc::Impl::HDRColorA, 16> input;
    for (size_t index = 0; index < input.size(); ++index)
    {
        input[index] = bc6h_enc::Impl::HDRColorA(rgba[index].x, rgba[index].y, rgba[index].z, rgba[index].w);
    }
    bc6h_enc::Impl::D3DX_BC6H block{};
    static_assert(sizeof(block) == 16);
    block.Encode(signedFormat, input.data());
    std::memcpy(output.data(), &block, sizeof(block));
}
} // namespace zenasset::detail
