#pragma once
#include "ZenAsset/Format/Image.h"
#include "ZenAsset/Format/Math.h"
#include "ZenAsset/Format/Status.h"
#include <span>

namespace zenasset::detail
{
struct HdrCubeView
{
    uint32_t size{0};
    uint32_t mipLevels{0};
    // Borrowed linear RGBA32F pixels: mip * 6 + (+X, -X, +Y, -Y, +Z, -Z).
    std::span<const std::vector<Vec4>> mipFaces;
};

struct HdrCubeSettings
{
    bool     compressBC6H{false};
    uint32_t maxDimension{16384};
    float    maxRelativeRmse{0.02f};
};

using HdrCubePixels = EnvironmentCubePixels;

struct HdrCubeFaceQuality
{
    uint32_t mipLevel{0};
    uint32_t face{0};
    uint32_t size{0};
    double   relativeRmse{0};
    double   maxAbsoluteError{0};
    double   sourceMin{0};
    double   sourceMax{0};
};

struct HdrCubeResult
{
    Status                               status;
    std::shared_ptr<const HdrCubePixels> pixels;
    std::string                          reason;
    uint64_t                             ownedBytes{0};
    uint64_t                             pixelBytes{0};
    std::vector<HdrCubeFaceQuality>      metrics;
};

// The caller retains the source owner for the call. This kernel never modifies
// or copies the source cube, and never generates/replaces its authored IBL mips.
// Disabled compression checks settings and face footprints only; it does not
// scan source pixels. The caller's scene validator still owns source validity.
// Success with null pixels means use that original cube losslessly: compression
// was disabled, or the whole cube failed eligibility/quality (reason supplied).
// Eligible BC6H UFLOAT input has RGB in [0,65504] and alpha exactly one. Malformed
// input is an error; enabled compression also rejects nonfinite input.
// Each face/mip must satisfy sqrt(sum RGB squared error / sum RGB squared input).
// Zero input energy requires exactly zero decoded RGB energy; no epsilon hides
// errors in black or very dark faces. Padded edge texels do not enter metrics.
// New output/metrics/reason and fixed block scratch are budgeted before use.
// Caller-owned input, allocator bookkeeping and fixed upstream codec stack are
// excluded. Temporary charges are restored; ownedBytes remains deducted on
// success, and pixelBytes is the part retained by pixels alone. Error/cancellation
// clears owned output and restores the original budget. After destroying metrics
// and reason, the caller may refund ownedBytes - pixelBytes.
HdrCubeResult ProcessHdrCube(const HdrCubeView&     input,
                             const HdrCubeSettings& settings,
                             uint64_t&              remaining,
                             const Cancellation*    cancellation = nullptr);
} // namespace zenasset::detail
