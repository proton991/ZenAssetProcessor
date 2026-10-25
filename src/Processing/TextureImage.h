#pragma once
#include "ZenAsset/Format/Image.h"
#include "ZenAsset/Format/Processing.h"
#include <limits>

namespace zenasset::detail
{
enum class TextureImageRole
{
    Color,
    Data,
    Normal,
    Occlusion
};

struct TextureImageUse
{
    TextureImageRole role{TextureImageRole::Color};
    bool             srgb{false};
    bool             requiresMipmaps{true};
    bool             alphaMask{false};
    bool             exactAlpha{false};
    bool             filterNormals{true};
    bool             filterNormalMin{true};
    bool             filterNormalMips{true};
    bool             normalRepeatS{true};
    bool             normalRepeatT{true};
    // Effective sampled-alpha cutoff, including any constant material factor.
    // Finite cutoffs outside [0,1] represent constant coverage.
    float alphaCutoff{0.5f};
    bool  operator==(const TextureImageUse&) const = default;
};

struct TextureMipQuality
{
    uint32_t width{0};
    uint32_t height{0};
    double   colorRmse{0};
    double   colorPsnr{std::numeric_limits<double>::infinity()};
    double   scalarMaxError{0};
    double   normalRmsAngleDegrees{0};
    double   normalMaxAngleDegrees{0};
    // Worst RMS among bilinear sample phases, and maximum across all phases.
    double normalFilteredRmsAngleDegrees{0};
    double normalFilteredMaxAngleDegrees{0};
    // Adaptive within-level UV observations remain separate from quarter phases.
    double normalUvObservedRmsAngleDegrees{0};
    double normalUvObservedMaxAngleDegrees{0};
    // Certified leaves cover all bilinear phases within this mip, not UV+LOD jointly.
    // Partial bounds on a rejected image are not a certificate.
    double   normalUvRmsUpperBoundDegrees{0};
    double   normalUvMaxUpperBoundDegrees{0};
    uint32_t normalUvRectangleCount{0};
    bool     normalUvContinuousCertified{false};
    // Observed joint UV/LOD samples, stored on the coarser level's record.
    double normalMipRmsAngleDegrees{0};
    double normalMipMaxAngleDegrees{0};
    // Certified leaf bounds cover joint continuous UV phases and mip blending.
    // Bounds from only part of a failed qualification are not a certificate.
    double   normalMipRmsUpperBoundDegrees{0};
    double   normalMipMaxUpperBoundDegrees{0};
    uint32_t normalJointBoxCount{0};
    bool     normalJointContinuousCertified{false};
    double   alphaCoverageError{0};
    bool     exactAlpha{true};
};

struct TextureImageResult
{
    Status                             status;
    std::shared_ptr<const ImagePixels> pixels;
    std::string                        reason;
    uint64_t                           ownedBytes{0};
    uint64_t                           pixelBytes{0};
    std::vector<TextureMipQuality>     metrics;
};

// Input ownership is borrowed/shared, never charged or modified. New pixel
// storage and temporary work are charged before allocation. Temporary charges
// are restored before return; successful owned output (including metrics) stays
// deducted and is reported in ownedBytes; pixelBytes is the portion retained by
// pixels alone. After destroying metrics/reason, callers may refund the difference.
// Quality rejection shares the original
// whole image and reports a reason. Errors/cancellation return no image.
TextureImageResult ProcessTextureImage(std::shared_ptr<const ImagePixels> input,
                                       const TextureImageUse&             use,
                                       const TextureSettings&             settings,
                                       uint64_t&                          remaining,
                                       const Cancellation*                cancellation = nullptr);
} // namespace zenasset::detail
