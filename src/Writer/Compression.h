#pragma once
#include "ZenAsset/Format/Compression.h"
#include "ZenAsset/Format/Status.h"
#include <cstddef>
#include <span>
#include <vector>

namespace zenasset::detail
{
struct CompressedResource
{
    Status                 status;
    TransportCodec         codec{TransportCodec::None};
    std::vector<std::byte> bytes;
};

Status ValidateCompressionSettings(const CompressionSettings& settings);
// availableBytes excludes the already-live raw resource and directory. A None
// result intentionally has no bytes: the caller retains and writes the raw input.
CompressedResource CompressResource(std::span<const std::byte> input,
                                    const CompressionSettings& settings,
                                    uint64_t                   availableBytes,
                                    uint64_t                   maxEncoderBytes,
                                    const Cancellation*        cancellation);
} // namespace zenasset::detail
