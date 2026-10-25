#pragma once
#include "ZenAsset/Reader/Package.h"
#include <cstdio>

namespace zenasset::detail
{
// File is positioned at the payload. Caller owns output and the file lock.
Status ReadZstandardResource(FILE*                 file,
                             const ResourceRecord& record,
                             std::span<std::byte>  output,
                             uint64_t              maxScratchBytes,
                             const Cancellation*   cancellation);
} // namespace zenasset::detail
