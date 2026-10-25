#pragma once
#include "ZenAsset/Format/Math.h"
#include <array>
#include <cstddef>

namespace zenasset::detail
{
void EncodeBc6hBlock(const std::array<Vec4, 16>& rgba, bool signedFormat, std::array<std::byte, 16>& output);
}
