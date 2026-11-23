#pragma once
#include "ZenAsset/Reader/Package.h"
#include <cstdio>

namespace zenasset::detail
{
ResourceData ReadResourceData(FILE*                 file,
                              const ResourceRecord& record,
                              uint64_t              maxScratchBytes,
                              const Cancellation*   cancellation,
                              TransferProgress      progress = {});
// File is positioned at the payload. Caller owns output and the file lock.
Status ReadZstandardResource(FILE*                 file,
                             const ResourceRecord& record,
                             std::span<std::byte>  output,
                             uint64_t              maxScratchBytes,
                             uint64_t&             scratchBytes,
                             const Cancellation*   cancellation,
                             TransferProgress      progress = {});
} // namespace zenasset::detail
