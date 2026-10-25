#pragma once
#include "ZenAsset/Format/Image.h"
#include "ZenAsset/Format/Math.h"
#include "ZenAsset/Format/Status.h"
#include <array>
#include <cstddef>
#include <span>

namespace zenasset::detail
{
// Exactly sixteen tightly packed RGBA8 pixels. BC4 consumes R; BC5 consumes RG.
// No color transfer or image-edge policy is performed here. Output must be one
// complete block. Failure clears it. Shared encoder tables initialize once.
// BC4/BC5 endpoint selection is refined against fractional UNORM interpolation,
// rather than an 8-bit-rounded palette. Image/normal quality gates remain separate.
Status EncodeBlock(PixelFormat                    format,
                   const std::array<uint8_t, 64>& rgba,
                   std::span<std::byte>           output,
                   uint32_t                       bc7Quality = 4);
// BC6H accepts finite RGB within the half-float range, with alpha exactly one.
Status EncodeHdrBlock(PixelFormat format, const std::array<Vec4, 16>& rgba, std::span<std::byte> output);

// Direction-optimized mode-6 alternatives for a linear RGB normal block. These
// are candidates only: image sampling and alpha guards must select the result.
// The bounded search uses quality 4 and clears all candidates on cancellation.
using BC7NormalCandidates = std::array<std::array<std::byte, 16>, 16>;
// Caller-owned so image refinement includes this cache in its scratch budget.
// Entries belong to one source block; every candidate-search call resets them.
struct BC7NormalSearchScratch
{
    struct Score
    {
        uint64_t key{0};
        double   score{0};
        uint64_t selectors{0};
        bool     complete{false};
    };
    std::array<Score, 128> scores{};
};
Status EncodeBC7NormalCandidates(const std::array<uint8_t, 64>& rgba,
                                 BC7NormalCandidates&           candidates,
                                 BC7NormalSearchScratch&        scratch,
                                 const Cancellation*            cancellation);
} // namespace zenasset::detail
