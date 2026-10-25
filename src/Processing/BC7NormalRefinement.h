#pragma once
#include "Processing/TextureImage.h"
#include <span>

namespace zenasset::detail
{
// Refines a complete baseline BC7Unorm level. The caller owns the unpublished
// output and discards it on failure. Temporary storage is bounded independently
// of image dimensions, charged before allocation and refunded on every exit.
Status RefineBC7NormalLevel(uint32_t                 width,
                            uint32_t                 height,
                            std::span<const uint8_t> source,
                            std::span<uint8_t>       encoded,
                            const TextureImageUse&   use,
                            uint64_t&                remaining,
                            const Cancellation*      cancellation);
} // namespace zenasset::detail
