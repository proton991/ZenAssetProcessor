#pragma once
#include <cstdint>

namespace zenasset
{
enum class TransportCodec : uint32_t
{
    None      = 0,
    Zstandard = 1
};

struct CompressionSettings
{
    TransportCodec transport{TransportCodec::None};
    // Zstandard levels 1 through 19 are supported. The encoder uses one thread
    // and a window of at most 8 MiB. Incompressible chunks remain uncompressed.
    int32_t zstandardLevel{3};
    // Lossless vertex and index sequence coding preserves every value and order.
    bool meshoptimizer{false};
};
} // namespace zenasset
