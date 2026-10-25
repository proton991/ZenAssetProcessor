#pragma once
#include "ZenAsset/Format/Image.h"
#include "ZenAsset/Format/Math.h"
#include "ZenAsset/Format/Status.h"
#include <array>
#include <cstddef>
#include <span>

namespace zenasset::detail
{
// Decodes one block without allocating. RGB remains in the stored transfer
// function; callers apply color transfer and normal/channel interpretation.
// BC4 emits (R,0,0,1), BC5 (R,G,0,1), BC6H (R,G,B,1). Failure clears output.
Status DecodeBlock(PixelFormat format, std::span<const std::byte> encoded, std::array<Vec4, 16>& output);
} // namespace zenasset::detail
