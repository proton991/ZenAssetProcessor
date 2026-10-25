#pragma once
#include "ZenAsset/Format/Scene.h"
#include "Importers/ImportErrors.h"

namespace zenasset
{
// Shared CPU image decoding. Independent calls may run on processing workers.
// Compressed source memory is borrowed only for the duration of this call.
std::vector<uint8_t> DecodeImagePixels(const uint8_t*                     data,
                                       size_t                             size,
                                       uint32_t&                          width,
                                       uint32_t&                          height,
                                       ImportErrors&                      errors,
                                       std::vector<std::vector<uint8_t>>* mips = nullptr);
} // namespace zenasset
