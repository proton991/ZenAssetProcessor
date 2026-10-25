#pragma once
#include "Importers/ImportErrors.h"

namespace zenasset
{
// Byte-preserving X/Z/alpha conversion for the OBJ UV V reversal. Poll between
// at most 128 KiB blocks so cancellation does not wait for an entire image.
void ReverseObjNormalGreen(std::vector<uint8_t>& pixels, ImportErrors& errors);
} // namespace zenasset
