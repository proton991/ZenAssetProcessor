#include "Processing/TextureImage.h"
#include "Diagnostics/StageMeasurements.h"
#include "Processing/BC7NormalRefinement.h"
#include "Codecs/BlockDecoding.h"
#include "Codecs/BlockEncoding.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cassert>
#include <span>

namespace zenasset::detail
{
namespace
{
class ImageBudget
{
public:
    explicit ImageBudget(uint64_t& remaining) : m_remaining(remaining) {}
    ~ImageBudget()
    {
        m_remaining += m_temporary;
    }
    bool Reserve(uint64_t bytes)
    {
        const bool valid = bytes <= m_remaining;
        if (valid)
        {
            m_remaining -= bytes;
            m_temporary += bytes;
        }
        return valid;
    }
    void Retain(uint64_t bytes)
    {
        m_temporary -= bytes;
    }
    void Release(uint64_t bytes)
    {
        m_temporary -= bytes;
        m_remaining += bytes;
    }

private:
    uint64_t& m_remaining;
    uint64_t  m_temporary{0};
};

struct ImageLevel
{
    uint32_t                 width{0};
    uint32_t                 height{0};
    std::span<const uint8_t> bytes;
    std::vector<uint8_t>     generated;
    double                   coverageError{0};
    double                   targetCoverage{-1};
};

struct ImageScratch
{
    std::array<ImageLevel, 32> levels;
    std::array<uint8_t, 64>    rgba{};
    std::array<Vec4, 16>       decoded{};
    std::array<uint64_t, 256>  histogram{};
};

Status CheckCancellation(const Cancellation* cancellation)
{
    Status status;
    if (cancellation && cancellation->IsCancelled())
    {
        status = {StatusCode::Cancelled, "Texture processing cancelled"};
    }
    return status;
}

double ToLinear(double value, bool srgb)
{
    const double result = !srgb ? value : value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    return result;
}

uint8_t ToByte(double value, bool srgb)
{
    value = std::clamp(value, 0.0, 1.0);
    if (srgb)
    {
        value = value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
    }
    return static_cast<uint8_t>(std::lround(value * 255.0));
}

bool Normalize(std::array<double, 3>& direction)
{
    const double length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
    const bool   valid  = std::isfinite(length) && length > 1e-12;
    if (valid)
    {
        for (double& value : direction)
        {
            value /= length;
        }
    }
    return valid;
}

std::array<double, 3> ReadRgb(const uint8_t* pixel, bool srgb)
{
    const std::array<double, 3> result{ToLinear(pixel[0] / 255.0, srgb), ToLinear(pixel[1] / 255.0, srgb),
                                       ToLinear(pixel[2] / 255.0, srgb)};
    return result;
}

bool ReadNormal(const std::array<double, 3>& rgb, std::array<double, 3>& normal)
{
    for (size_t channel = 0; channel < 3; ++channel)
    {
        normal[channel] = rgb[channel] * 2.0 - 1.0;
    }
    return Normalize(normal);
}

double NormalAngle(const std::array<double, 3>& reference, const std::array<double, 3>& restored, PixelFormat format)
{
    std::array<double, 3> left{}, right{};
    double                angle      = 180;
    bool                  validRight = false;
    if (format == PixelFormat::BC5Unorm)
    {
        right[0]   = restored[0] * 2.0 - 1.0;
        right[1]   = restored[1] * 2.0 - 1.0;
        right[2]   = std::sqrt(std::max(0.0, 1.0 - right[0] * right[0] - right[1] * right[1]));
        validRight = Normalize(right);
    }
    else
    {
        validRight = ReadNormal(restored, right);
    }
    if (ReadNormal(reference, left) && validRight)
    {
        const double dot = left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
        angle            = left == right ? 0 : std::acos(std::clamp(dot, -1.0, 1.0)) * (180.0 / 3.14159265358979323846);
    }
    return angle;
}

std::array<double, 3> DecodedNormalRgb(const Vec4& decoded, PixelFormat format, bool srgb)
{
    std::array<double, 3> result{decoded.x, decoded.y, decoded.z};
    if (format != PixelFormat::BC5Unorm)
    {
        const std::array<uint8_t, 3> bytes{ToByte(decoded.x, false), ToByte(decoded.y, false), ToByte(decoded.z, false)};
        result = ReadRgb(bytes.data(), srgb);
    }
    return result;
}

std::array<double, 3> BilinearNormalRgb(const std::array<std::array<double, 3>, 4>& pixels, double x, double y)
{
    std::array<double, 3> result{};
    for (size_t channel = 0; channel < 3; ++channel)
    {
        result[channel] = (1 - x) * (1 - y) * pixels[0][channel] + x * (1 - y) * pixels[1][channel]
                        + (1 - x) * y * pixels[2][channel] + x * y * pixels[3][channel];
    }
    return result;
}

Status DecodeNormalRow(std::span<const uint8_t>         encoded,
                       uint32_t                         width,
                       uint32_t                         y,
                       PixelFormat                      format,
                       bool                             srgb,
                       std::span<std::array<double, 3>> row)
{
    Status               status;
    const uint32_t       blocksWide = static_cast<uint32_t>((uint64_t(width) + 3u) / 4u);
    std::array<Vec4, 16> block{};
    for (uint32_t bx = 0; status.Succeeded() && bx < blocksWide; ++bx)
    {
        const size_t offset = (size_t(y / 4u) * blocksWide + bx) * 16u;
        status              = DecodeBlock(format, std::as_bytes(encoded.subspan(offset, 16u)), block);
        for (uint32_t x = 0; status.Succeeded() && x < 4u && bx * 4u + x < width; ++x)
        {
            row[bx * 4u + x] = DecodedNormalRgb(block[(y % 4u) * 4u + x], format, srgb);
        }
    }
    return status;
}

Status MeasureFilteredNormals(const ImageLevel&        source,
                              std::span<const uint8_t> encoded,
                              PixelFormat              format,
                              const TextureImageUse&   use,
                              uint64_t&                remaining,
                              TextureMipQuality&       quality,
                              const Cancellation*      cancellation)
{
    // Two decoded rows bound scratch memory by width. Periodic neighbors include
    // wrap seams; zero phases also cover the 1D filtering at clamp/mirror edges.
    // Filter the stored channels before reconstruction, as texture sampling does.
    ImageBudget                        budget(remaining);
    Status                             status;
    std::vector<std::array<double, 3>> rows;
    std::array<double, 16>             squared{};
    const uint64_t                     rowBytes = uint64_t(source.width) * 2u * sizeof(rows[0]);
    if (rowBytes > SIZE_MAX || !budget.Reserve(rowBytes))
    {
        status = {StatusCode::ResourceLimit, "Filtered normal quality rows exceed budget"};
    }
    else
    {
        rows.resize(size_t(source.width) * 2u);
        std::span<std::array<double, 3>> current(rows.data(), source.width);
        std::span<std::array<double, 3>> next(rows.data() + source.width, source.width);
        status = DecodeNormalRow(encoded, source.width, 0, format, use.srgb, current);
        for (uint32_t y = 0; status.Succeeded() && y < source.height; ++y)
        {
            status               = CheckCancellation(cancellation);
            const uint32_t nextY = y + 1u < source.height ? y + 1u : use.normalRepeatT ? 0u : y;
            if (status.Succeeded())
            {
                status = DecodeNormalRow(encoded, source.width, nextY, format, use.srgb, next);
            }
            for (uint32_t x = 0; status.Succeeded() && x < source.width; ++x)
            {
                const uint32_t                             nextX = x + 1u < source.width ? x + 1u : use.normalRepeatS ? 0u : x;
                const std::array<std::array<double, 3>, 4> original{
                    ReadRgb(source.bytes.data() + (size_t(y) * source.width + x) * 4u, use.srgb),
                    ReadRgb(source.bytes.data() + (size_t(y) * source.width + nextX) * 4u, use.srgb),
                    ReadRgb(source.bytes.data() + (size_t(nextY) * source.width + x) * 4u, use.srgb),
                    ReadRgb(source.bytes.data() + (size_t(nextY) * source.width + nextX) * 4u, use.srgb)};
                const std::array<std::array<double, 3>, 4> restored{current[x], current[nextX], next[x], next[nextX]};
                for (uint32_t phase = 1; phase < 16; ++phase)
                {
                    const double fx = double(phase % 4u) / 4.0;
                    const double fy = double(phase / 4u) / 4.0;
                    const double angle =
                        NormalAngle(BilinearNormalRgb(original, fx, fy), BilinearNormalRgb(restored, fx, fy), format);
                    squared[phase]                        += angle * angle;
                    quality.normalFilteredMaxAngleDegrees  = std::max(quality.normalFilteredMaxAngleDegrees, angle);
                }
            }
            std::swap(current, next);
        }
        if (status.Succeeded())
        {
            const double pixels = double(source.width) * source.height;
            for (uint32_t phase = 1; phase < 16; ++phase)
            {
                quality.normalFilteredRmsAngleDegrees =
                    std::max(quality.normalFilteredRmsAngleDegrees, std::sqrt(squared[phase] / pixels));
            }
        }
    }
    return status;
}

struct NormalSampleAxis
{
    uint32_t first{0};
    uint32_t second{0};
    double   fraction{0};
};


std::array<std::array<double, 3>, 2> SampleNormalPair(const ImageLevel&                      source,
                                                      const NormalSampleAxis&                x,
                                                      const NormalSampleAxis&                y,
                                                      std::span<const std::array<double, 3>> first,
                                                      std::span<const std::array<double, 3>> second,
                                                      bool                                   srgb)
{
    const std::array<std::array<double, 3>, 4> original{
        ReadRgb(source.bytes.data() + (size_t(y.first) * source.width + x.first) * 4, srgb),
        ReadRgb(source.bytes.data() + (size_t(y.first) * source.width + x.second) * 4, srgb),
        ReadRgb(source.bytes.data() + (size_t(y.second) * source.width + x.first) * 4, srgb),
        ReadRgb(source.bytes.data() + (size_t(y.second) * source.width + x.second) * 4, srgb)};
    const std::array<std::array<double, 3>, 4> restored{first[x.first], first[x.second], second[x.first], second[x.second]};
    const std::array<std::array<double, 3>, 2> result{BilinearNormalRgb(original, x.fraction, y.fraction),
                                                      BilinearNormalRgb(restored, x.fraction, y.fraction)};
    return result;
}

constexpr double   NormalDirectionSlack          = 1e-12;
constexpr double   NormalAngleSlack              = 1e-8;
constexpr double   DegreesPerRadian              = 180.0 / 3.14159265358979323846;
constexpr uint32_t NormalMaximumSubdivisionDepth = 12;
constexpr uint32_t NormalMaximumRegionCount      = 127;

struct NormalJointBox
{
    std::array<double, 3> first{0, 0, 0};
    std::array<double, 3> last{1, 1, 1};
    uint32_t              depth{0};
};

struct NormalJointBoxQuality
{
    std::array<double, 27> observedSquared{};
    std::array<double, 3>  variation{};
    double                 upperSquared{0};
    double                 upperMaximum{0};
};

std::array<double, 3> BlendNormalChannels(const std::array<double, 3>& first,
                                          const std::array<double, 3>& last,
                                          double                       fraction)
{
    std::array<double, 3> result{};
    for (size_t channel = 0; channel < 3; ++channel)
    {
        result[channel] = (1 - fraction) * first[channel] + fraction * last[channel];
    }
    return result;
}

double NormalDot(const std::array<double, 3>& left, const std::array<double, 3>& right)
{
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

std::array<double, 3> NormalCross(const std::array<double, 3>& left, const std::array<double, 3>& right)
{
    const std::array<double, 3> result{left[1] * right[2] - left[2] * right[1], left[2] * right[0] - left[0] * right[2],
                                       left[0] * right[1] - left[1] * right[0]};
    return result;
}

// Lower bound for the length of every vector on an affine segment. The slack
// also keeps the existing 1e-12 undefined-normal convention inside the bound.
double MinimumNormalLength(const std::array<double, 3>& first, const std::array<double, 3>& last)
{
    const std::array<double, 3> delta{last[0] - first[0], last[1] - first[1], last[2] - first[2]};
    const double                squared  = NormalDot(delta, delta);
    const double                fraction = squared > 0 ? std::clamp(-NormalDot(first, delta) / squared, 0.0, 1.0) : 0;
    const std::array<double, 3> closest  = BlendNormalChannels(first, last, fraction);
    return std::max(0.0, std::sqrt(NormalDot(closest, closest)) - NormalDirectionSlack);
}

double AffineNormalAngleBound(const std::array<double, 3>& a0,
                              const std::array<double, 3>& a1,
                              const std::array<double, 3>& b0,
                              const std::array<double, 3>& b1)
{
    double bound = 180;
    if (MinimumNormalLength(a0, a1) > 1e-12 && MinimumNormalLength(b0, b1) > 1e-12)
    {
        if (a0 == b0 && a1 == b1)
        {
            bound = 0;
        }
        else
        {
            // Cross and dot are quadratic Bernstein polynomials. Their control
            // hull bounds the cross norm above and the dot product below.
            const std::array<std::array<double, 3>, 3> cross{
                NormalCross(a0, b0), BlendNormalChannels(NormalCross(a0, b1), NormalCross(a1, b0), 0.5), NormalCross(a1, b1)};
            const double dotLower =
                std::min({NormalDot(a0, b0), (NormalDot(a0, b1) + NormalDot(a1, b0)) * 0.5, NormalDot(a1, b1)})
                - NormalDirectionSlack;
            double crossUpper = 0;
            for (const std::array<double, 3>& control : cross)
            {
                crossUpper = std::max(crossUpper, std::sqrt(NormalDot(control, control)));
            }
            crossUpper += NormalDirectionSlack;
            if (dotLower > 0)
            {
                bound = std::min(180.0, std::atan2(crossUpper, dotLower) * DegreesPerRadian + NormalAngleSlack);
            }
        }
    }
    return bound;
}

std::array<double, 3> SignedNormalChannels(const std::array<double, 3>& rgb, bool reconstructXY)
{
    std::array<double, 3> result{rgb[0] * 2 - 1, rgb[1] * 2 - 1, rgb[2] * 2 - 1};
    if (reconstructXY)
    {
        result[2] = std::sqrt(std::max(0.0, 1 - result[0] * result[0] - result[1] * result[1]));
    }
    return result;
}

double NormalXYChordError(const std::array<double, 3>& first, const std::array<double, 3>& last)
{
    const std::array<double, 3> a{first[0], first[1], 0};
    const std::array<double, 3> b{last[0], last[1], 0};
    const std::array<double, 3> delta{b[0] - a[0], b[1] - a[1], 0};
    const double                squared  = NormalDot(delta, delta);
    const double                fraction = squared > 0 ? std::clamp(-NormalDot(a, delta) / squared, 0.0, 1.0) : 0;
    const std::array<double, 3> closest  = BlendNormalChannels(a, b, fraction);
    const double                qMinimum = std::max(0.0, NormalDot(closest, closest) - NormalDirectionSlack);
    const double                qMaximum = std::max(NormalDot(a, a), NormalDot(b, b)) + NormalDirectionSlack;
    double                      error    = 0;
    if (qMinimum < 1)
    {
        const double zMinimum = std::sqrt(std::max(0.0, 1 - qMaximum));
        const double zMaximum = std::sqrt(std::max(0.0, 1 - qMinimum));
        // This range bound also covers the reconstruction cusp at q=1. Convex
        // q attains its maximum at an endpoint; no interior maximum is missed.
        error = std::max(zMaximum - std::min(first[2], last[2]), std::max(first[2], last[2]) - zMinimum);
        if (qMaximum < 1)
        {
            // Inside the unit disk, Z is concave. For this local s in [0,1],
            // |Z''| <= |delta|^2/Zmin + max|xy dot delta|^2/Zmin^3.
            // Linear interpolation error is bounded by max|Z''|/8.
            const double dot = std::max(std::abs(NormalDot(a, delta)), std::abs(NormalDot(b, delta))) + NormalDirectionSlack;
            const double curvature = (squared + NormalDirectionSlack) / zMinimum + dot * dot / (zMinimum * zMinimum * zMinimum);
            error                  = std::min(error, curvature / 8);
        }
        error += NormalDirectionSlack;
    }
    return error;
}

double NormalMipAngleBound(const std::array<double, 3>& referenceFirst,
                           const std::array<double, 3>& referenceLast,
                           const std::array<double, 3>& restoredFirst,
                           const std::array<double, 3>& restoredLast,
                           PixelFormat                  format)
{
    const bool                  xy    = format == PixelFormat::BC5Unorm;
    const std::array<double, 3> a0    = SignedNormalChannels(referenceFirst, false);
    const std::array<double, 3> a1    = SignedNormalChannels(referenceLast, false);
    const std::array<double, 3> b0    = SignedNormalChannels(restoredFirst, xy);
    const std::array<double, 3> b1    = SignedNormalChannels(restoredLast, xy);
    double                      bound = AffineNormalAngleBound(a0, a1, b0, b1);
    if (xy && bound < 180)
    {
        const double error  = NormalXYChordError(b0, b1);
        const double length = MinimumNormalLength(b0, b1);
        // Every reconstructed vector lies in a radius-error ball about its
        // chord vector; that ball subtends at most asin(error / length).
        bound = error < length ? std::min(180.0, bound + std::asin(error / length) * DegreesPerRadian + NormalAngleSlack) : 180;
    }
    return bound;
}

struct NormalUvRectangle
{
    std::array<double, 2> first{0, 0};
    std::array<double, 2> last{1, 1};
    uint32_t              depth{0};
};

struct NormalUvRectangleQuality
{
    std::array<double, 9> observedSquared{};
    double                upperSquared{0};
    double                upperMaximum{0};
};

template <size_t Dimensions> using NormalTensor = std::array<std::array<double, 3>, size_t{1} << Dimensions>;
using NormalPatch                               = NormalTensor<2>;
using NormalVolume                              = NormalTensor<3>;

void AccumulateNormalUpperSquared(double angle, double& sum)
{
    if (angle != 0)
    {
        const double squared = std::nextafter(angle * angle, std::numeric_limits<double>::infinity());
        sum                  = std::nextafter(sum + squared, std::numeric_limits<double>::infinity());
    }
}

double UpperNormalRms(double squared, double pixels)
{
    const double upper =
        squared == 0 ? 0 : std::nextafter(std::sqrt(squared / pixels), std::numeric_limits<double>::infinity());
    return std::min(180.0, upper);
}

// A bilinear patch stays in the component-wise box of its corner vectors.
// Distance from that box to zero is a conservative minimum vector length.
double MinimumNormalTensorLength(std::span<const std::array<double, 3>> patch)
{
    std::array<double, 3> closest{};
    for (size_t channel = 0; channel < 3; ++channel)
    {
        double low = patch[0][channel], high = low;
        for (const std::array<double, 3>& corner : patch)
        {
            low  = std::min(low, corner[channel]);
            high = std::max(high, corner[channel]);
        }
        closest[channel] = low > 0 ? low : high < 0 ? high : 0;
    }
    return std::max(0.0, std::sqrt(NormalDot(closest, closest)) - NormalDirectionSlack);
}

template <size_t Dimensions>
double TensorNormalAngleBound(const NormalTensor<Dimensions>& original, const NormalTensor<Dimensions>& restored)
{
    static_assert(Dimensions == 2 || Dimensions == 3);
    constexpr size_t ControlCount = Dimensions == 2 ? 9 : 27;
    double           bound        = 180;
    if (MinimumNormalTensorLength(original) > 1e-12 && MinimumNormalTensorLength(restored) > 1e-12)
    {
        if (original == restored)
        {
            bound = 0;
        }
        else
        {
            // A product of degree-one tensor patches has degree two per axis.
            // Corner pairs contribute 1/C(2, ia+ib) independently in each axis.
            std::array<std::array<double, 3>, ControlCount> cross{};
            std::array<double, ControlCount>                dot{};
            for (size_t a = 0; a < original.size(); ++a)
            {
                for (size_t b = 0; b < restored.size(); ++b)
                {
                    size_t control = 0, stride = 1;
                    double weight = 1;
                    for (size_t axis = 0; axis < Dimensions; ++axis)
                    {
                        const size_t coordinate  = ((a >> axis) & 1) + ((b >> axis) & 1);
                        control                 += coordinate * stride;
                        stride                  *= 3;
                        weight                  *= coordinate == 1 ? 0.5 : 1;
                    }
                    const std::array<double, 3> product = NormalCross(original[a], restored[b]);
                    for (size_t channel = 0; channel < 3; ++channel)
                    {
                        cross[control][channel] += product[channel] * weight;
                    }
                    dot[control] += NormalDot(original[a], restored[b]) * weight;
                }
            }
            double crossUpper = 0, dotLower = std::numeric_limits<double>::infinity();
            for (size_t control = 0; control < cross.size(); ++control)
            {
                crossUpper = std::max(crossUpper, std::sqrt(NormalDot(cross[control], cross[control])));
                dotLower   = std::min(dotLower, dot[control]);
            }
            crossUpper += NormalDirectionSlack;
            dotLower   -= NormalDirectionSlack;
            if (dotLower > 0)
            {
                bound = std::min(180.0, std::atan2(crossUpper, dotLower) * DegreesPerRadian + NormalAngleSlack);
            }
        }
    }
    return bound;
}

template <size_t Dimensions> double NormalXYTensorError(const NormalTensor<Dimensions>& reconstructed)
{
    NormalTensor<Dimensions> xy       = reconstructed;
    double                   qMaximum = 0, endpointMinimum = 1, endpointMaximum = 0;
    for (size_t corner = 0; corner < xy.size(); ++corner)
    {
        xy[corner][2]   = 0;
        qMaximum        = std::max(qMaximum, NormalDot(xy[corner], xy[corner]));
        endpointMinimum = std::min(endpointMinimum, reconstructed[corner][2]);
        endpointMaximum = std::max(endpointMaximum, reconstructed[corner][2]);
    }
    const double length    = MinimumNormalTensorLength(xy);
    const double qMinimum  = std::max(0.0, length * length - NormalDirectionSlack);
    qMaximum              += NormalDirectionSlack;
    double error           = 0;
    if (qMinimum < 1)
    {
        const double zMinimum = std::sqrt(std::max(0.0, 1 - qMaximum));
        const double zMaximum = std::sqrt(std::max(0.0, 1 - qMinimum));
        error                 = std::max(zMaximum - endpointMinimum, endpointMaximum - zMinimum);
        if (qMaximum < 1)
        {
            double edgeSquaredSum = 0;
            for (size_t axis = 0; axis < Dimensions; ++axis)
            {
                double maximumSquared = 0;
                for (size_t corner = 0; corner < xy.size(); ++corner)
                {
                    if ((corner & (size_t{1} << axis)) == 0)
                    {
                        const size_t                next = corner | (size_t{1} << axis);
                        const std::array<double, 3> delta{xy[next][0] - xy[corner][0], xy[next][1] - xy[corner][1], 0};
                        maximumSquared = std::max(maximumSquared, NormalDot(delta, delta));
                    }
                }
                edgeSquaredSum += maximumSquared;
            }
            // XY is multi-affine, so every pure second derivative vanishes.
            // The hemisphere Hessian norm is 1/Z^3. Sequential tensor linear
            // interpolation gives sum(max squared parallel-edge deltas)/(8*Zmin^3).
            const double curvature = (edgeSquaredSum + NormalDirectionSlack) / (zMinimum * zMinimum * zMinimum);
            error                  = std::min(error, curvature / 8);
        }
        error += NormalDirectionSlack;
    }
    return error;
}

template <size_t Dimensions> double NormalTensorAngleBound(const NormalTensor<Dimensions>& original,
                                                           const NormalTensor<Dimensions>& restored,
                                                           PixelFormat                     format)
{
    const bool               xy = format == PixelFormat::BC5Unorm;
    NormalTensor<Dimensions> a{}, b{};
    for (size_t corner = 0; corner < a.size(); ++corner)
    {
        a[corner] = SignedNormalChannels(original[corner], false);
        b[corner] = SignedNormalChannels(restored[corner], xy);
    }
    double bound = TensorNormalAngleBound<Dimensions>(a, b);
    if (xy && bound < 180)
    {
        const double error  = NormalXYTensorError<Dimensions>(b);
        const double length = MinimumNormalTensorLength(b);
        bound = error < length ? std::min(180.0, bound + std::asin(error / length) * DegreesPerRadian + NormalAngleSlack) : 180;
    }
    return bound;
}

NormalPatch RestrictNormalPatch(const NormalPatch& patch, const NormalUvRectangle& rectangle)
{
    NormalPatch result{};
    for (size_t corner = 0; corner < result.size(); ++corner)
    {
        const double x = corner % 2 == 0 ? rectangle.first[0] : rectangle.last[0];
        const double y = corner / 2 == 0 ? rectangle.first[1] : rectangle.last[1];
        result[corner] = BilinearNormalRgb(patch, x, y);
    }
    return result;
}

Status MeasureNormalUvRectangle(const ImageLevel&                source,
                                std::span<const uint8_t>         encoded,
                                PixelFormat                      format,
                                const TextureImageUse&           use,
                                std::span<std::array<double, 3>> rows,
                                const NormalUvRectangle&         rectangle,
                                NormalUvRectangleQuality&        measured,
                                TextureMipQuality&               quality,
                                const Cancellation*              cancellation)
{
    std::span<std::array<double, 3>> current = rows.first(source.width);
    std::span<std::array<double, 3>> next    = rows.subspan(source.width, source.width);
    Status                           status  = DecodeNormalRow(encoded, source.width, 0, format, use.srgb, current);
    for (uint32_t y = 0; status.Succeeded() && y < source.height; ++y)
    {
        status               = CheckCancellation(cancellation);
        const uint32_t nextY = y + 1u < source.height ? y + 1u : use.normalRepeatT ? 0u : y;
        if (status.Succeeded())
        {
            status = DecodeNormalRow(encoded, source.width, nextY, format, use.srgb, next);
        }
        for (uint32_t x = 0; status.Succeeded() && x < source.width; ++x)
        {
            const uint32_t    nextX = x + 1u < source.width ? x + 1u : use.normalRepeatS ? 0u : x;
            const NormalPatch original{ReadRgb(source.bytes.data() + (size_t(y) * source.width + x) * 4u, use.srgb),
                                       ReadRgb(source.bytes.data() + (size_t(y) * source.width + nextX) * 4u, use.srgb),
                                       ReadRgb(source.bytes.data() + (size_t(nextY) * source.width + x) * 4u, use.srgb),
                                       ReadRgb(source.bytes.data() + (size_t(nextY) * source.width + nextX) * 4u, use.srgb)};
            const NormalPatch restored{current[x], current[nextX], next[x], next[nextX]};
            double            observedMaximum = 0;
            for (size_t sample = 0; sample < measured.observedSquared.size(); ++sample)
            {
                const double fx = rectangle.first[0] + (rectangle.last[0] - rectangle.first[0]) * double(sample % 3) * 0.5;
                const double fy = rectangle.first[1] + (rectangle.last[1] - rectangle.first[1]) * double(sample / 3) * 0.5;
                const double angle =
                    NormalAngle(BilinearNormalRgb(original, fx, fy), BilinearNormalRgb(restored, fx, fy), format);
                measured.observedSquared[sample] += angle * angle;
                observedMaximum                   = std::max(observedMaximum, angle);
            }
            quality.normalUvObservedMaxAngleDegrees = std::max(quality.normalUvObservedMaxAngleDegrees, observedMaximum);
            const double upper =
                std::max(observedMaximum, NormalTensorAngleBound<2>(RestrictNormalPatch(original, rectangle),
                                                                    RestrictNormalPatch(restored, rectangle), format));
            AccumulateNormalUpperSquared(upper, measured.upperSquared);
            measured.upperMaximum = std::max(measured.upperMaximum, upper);
        }
        std::swap(current, next);
    }
    if (status.Succeeded())
    {
        const double pixels = double(source.width) * source.height;
        for (double sum : measured.observedSquared)
        {
            quality.normalUvObservedRmsAngleDegrees =
                std::max(quality.normalUvObservedRmsAngleDegrees, std::sqrt(sum / pixels));
        }
    }
    return status;
}

Status QualifyNormalUv(const ImageLevel&        source,
                       std::span<const uint8_t> encoded,
                       PixelFormat              format,
                       const TextureImageUse&   use,
                       const TextureSettings&   settings,
                       uint64_t&                remaining,
                       TextureMipQuality&       quality,
                       std::string&             reason,
                       const Cancellation*      cancellation)
{
    ImageBudget                                                      budget(remaining);
    Status                                                           status;
    std::vector<std::array<double, 3>>                               rows;
    std::array<NormalUvRectangle, NormalMaximumSubdivisionDepth + 1> pending{};
    const uint64_t                                                   rowBytes = uint64_t(source.width) * 2 * sizeof(rows[0]);
    const uint64_t workingBytes = rowBytes + sizeof(pending) + sizeof(NormalUvRectangleQuality);
    if (rowBytes > SIZE_MAX || !budget.Reserve(workingBytes))
    {
        status = {StatusCode::ResourceLimit, "Normal UV quality working storage exceeds budget"};
    }
    else
    {
        rows.resize(size_t(source.width) * 2);
        size_t count = 1;
        while (status.Succeeded() && reason.empty() && count != 0)
        {
            const NormalUvRectangle  rectangle = pending[--count];
            NormalUvRectangleQuality measured;
            ++quality.normalUvRectangleCount;
            status = MeasureNormalUvRectangle(source, encoded, format, use, rows, rectangle, measured, quality, cancellation);
            if (status.Succeeded())
            {
                const double upperRms = UpperNormalRms(measured.upperSquared, double(source.width) * source.height);
                if (quality.normalUvObservedRmsAngleDegrees > settings.normalRmsAngleDegrees
                    || quality.normalUvObservedMaxAngleDegrees > settings.normalMaxAngleDegrees)
                {
                    reason = format == PixelFormat::BC5Unorm ? "BC5 bilinear UV sampling exceeds normal angular limits"
                                                             : "BC7 bilinear UV sampling exceeds normal angular limits";
                }
                else if (upperRms <= settings.normalRmsAngleDegrees && measured.upperMaximum <= settings.normalMaxAngleDegrees)
                {
                    quality.normalUvRmsUpperBoundDegrees = std::max(quality.normalUvRmsUpperBoundDegrees, upperRms);
                    quality.normalUvMaxUpperBoundDegrees =
                        std::max(quality.normalUvMaxUpperBoundDegrees, measured.upperMaximum);
                }
                else if (rectangle.depth == NormalMaximumSubdivisionDepth
                         || quality.normalUvRectangleCount == NormalMaximumRegionCount)
                {
                    reason = format == PixelFormat::BC5Unorm
                               ? "BC5 bilinear UV sampling could not certify normal angular limits"
                               : "BC7 bilinear UV sampling could not certify normal angular limits";
                }
                else
                {
                    const size_t axis =
                        rectangle.last[0] - rectangle.first[0] >= rectangle.last[1] - rectangle.first[1] ? 0 : 1;
                    const double      middle = (rectangle.first[axis] + rectangle.last[axis]) * 0.5;
                    NormalUvRectangle first = rectangle, last = rectangle;
                    ++first.depth;
                    ++last.depth;
                    first.last[axis] = last.first[axis] = middle;
                    pending[count++]                    = last;
                    pending[count++]                    = first;
                }
            }
            if (status.Succeeded() && reason.empty() && count != 0
                && quality.normalUvRectangleCount == NormalMaximumRegionCount)
            {
                reason = format == PixelFormat::BC5Unorm ? "BC5 bilinear UV sampling could not certify normal angular limits"
                                                         : "BC7 bilinear UV sampling could not certify normal angular limits";
            }
        }
        quality.normalUvContinuousCertified = status.Succeeded() && reason.empty() && count == 0;
    }
    return status;
}

struct NormalPhaseAxis
{
    int64_t  base{0};
    double   offset{0};
    double   scale{1};
    uint32_t extent{1};
    bool     linear{true};
    bool     repeat{false};
};

NormalPhaseAxis MakeNormalPhaseAxis(uint32_t anchor, uint32_t fineExtent, uint32_t extent, bool linear, bool repeat)
{
    NormalPhaseAxis result;
    result.extent = extent;
    result.linear = linear;
    result.repeat = repeat;
    if (extent == fineExtent)
    {
        // This also handles the unchanged 1 -> 1 axis of a one-dimensional mip.
        result.base   = anchor;
        result.offset = linear ? 0 : 0.5;
    }
    else
    {
        assert(extent == fineExtent / 2);
        result.base   = anchor / 2;
        result.scale  = double(extent) / fineExtent;
        result.offset = (double(anchor % 2) + 0.5) * 0.5 - (linear ? 0.5 : 0);
        if ((fineExtent % 2) != 0)
        {
            result.offset -= (double(anchor) + 0.5) / (2.0 * fineExtent);
        }
    }
    return result;
}

uint32_t NormalAddress(int64_t index, uint32_t extent, bool repeat)
{
    return repeat ? static_cast<uint32_t>((index + extent) % extent)
                  : static_cast<uint32_t>(std::clamp<int64_t>(index, 0, extent - 1));
}

NormalSampleAxis SampleNormalPhase(const NormalPhaseAxis& axis, double phase)
{
    // Keep the fractional calculation near zero rather than subtracting two
    // large texel coordinates. This is u*extent - filterOffset algebraically.
    const double     coordinate = axis.offset + axis.scale * phase;
    const int64_t    relative   = static_cast<int64_t>(std::floor(coordinate));
    const int64_t    first      = axis.base + relative;
    NormalSampleAxis result;
    result.first    = NormalAddress(first, axis.extent, axis.repeat);
    result.second   = NormalAddress(first + (axis.linear ? 1 : 0), axis.extent, axis.repeat);
    result.fraction = axis.linear ? coordinate - std::floor(coordinate) : 0;
    return result;
}

NormalSampleAxis SampleNormalPiece(const NormalPhaseAxis& axis, double firstPhase, double lastPhase, double phase)
{
    const double     middle   = (firstPhase + lastPhase) * 0.5;
    const int64_t    relative = static_cast<int64_t>(std::floor(axis.offset + axis.scale * middle));
    NormalSampleAxis result;
    result.first  = NormalAddress(axis.base + relative, axis.extent, axis.repeat);
    result.second = NormalAddress(axis.base + relative + 1, axis.extent, axis.repeat);
    // A linear piece has fixed taps; corners include its continuous one-sided
    // limits. Roundoff at a calculated cut cannot move a fraction outside [0,1].
    result.fraction = std::clamp(axis.offset + axis.scale * phase - double(relative), 0.0, 1.0);
    return result;
}

struct NormalPhaseCuts
{
    std::array<double, 4> values{};
    size_t                count{2};
};

NormalPhaseCuts MakeNormalPhaseCuts(const NormalPhaseAxis& fine, const NormalPhaseAxis& coarse, double first, double last)
{
    NormalPhaseCuts result;
    result.values[0] = first;
    result.values[1] = last;
    for (const NormalPhaseAxis& axis : {fine, coarse})
    {
        const int64_t start = static_cast<int64_t>(std::floor(axis.offset + axis.scale * first));
        for (int64_t boundary = start; boundary <= start + 2; ++boundary)
        {
            const double phase = (double(boundary) - axis.offset) / axis.scale;
            if (phase > first && phase < last
                && std::find(result.values.begin(), result.values.begin() + result.count, phase)
                       == result.values.begin() + result.count)
            {
                // There is at most one fine nearest cut and one coarse cut.
                assert(result.count < result.values.size());
                result.values[result.count++] = phase;
            }
        }
    }
    std::sort(result.values.begin(), result.values.begin() + result.count);
    return result;
}

struct NormalNearestTapPair
{
    uint32_t fine{0};
    uint32_t coarse{0};
    bool     operator==(const NormalNearestTapPair&) const = default;
};

struct NormalNearestStates
{
    std::array<NormalNearestTapPair, 7> values{};
    size_t                              count{0};
};

void AddNormalNearestState(const NormalPhaseAxis& fine,
                           const NormalPhaseAxis& coarse,
                           double                 phase,
                           NormalNearestStates&   states)
{
    // Union both sides of the tiny numerical uncertainty around a calculated
    // cut. This only enlarges a bound; observations use the unperturbed phase.
    for (double guard : {-NormalDirectionSlack, 0.0, NormalDirectionSlack})
    {
        const NormalNearestTapPair state{SampleNormalPhase(fine, phase + guard).first,
                                         SampleNormalPhase(coarse, phase + guard).first};
        if (std::find(states.values.begin(), states.values.begin() + states.count, state)
            == states.values.begin() + states.count)
        {
            assert(states.count < states.values.size());
            states.values[states.count++] = state;
        }
    }
}

NormalNearestStates MakeNormalNearestStates(const NormalPhaseAxis& fine,
                                            const NormalPhaseAxis& coarse,
                                            double                 first,
                                            double                 last)
{
    const NormalPhaseCuts cuts = MakeNormalPhaseCuts(fine, coarse, first, last);
    NormalNearestStates   result;
    for (size_t cut = 0; cut < cuts.count; ++cut)
    {
        // Exact endpoints matter: odd mip ratios can select a new coarse tap
        // only at the upper phase endpoint. Never infer it from the left limit.
        AddNormalNearestState(fine, coarse, cuts.values[cut], result);
        if (cut + 1 < cuts.count)
        {
            AddNormalNearestState(fine, coarse, (cuts.values[cut] + cuts.values[cut + 1]) * 0.5, result);
        }
    }
    return result;
}

struct NormalMipPairView
{
    const ImageLevel&        fine;
    const ImageLevel&        coarse;
    std::span<const uint8_t> fineEncoded;
    std::span<const uint8_t> coarseEncoded;
    PixelFormat              format;
    const TextureImageUse&   use;
};

struct NormalMipRows
{
    std::span<std::array<double, 3>> fineFirst;
    std::span<std::array<double, 3>> fineSecond;
    std::span<std::array<double, 3>> coarseFirst;
    std::span<std::array<double, 3>> coarseSecond;
};

Status DecodeNormalMipRows(const NormalMipPairView& pair,
                           const NormalMipRows&     rows,
                           const NormalSampleAxis&  fineY,
                           const NormalSampleAxis&  coarseY)
{
    Status status = DecodeNormalRow(pair.fineEncoded, pair.fine.width, fineY.first, pair.format, pair.use.srgb, rows.fineFirst);
    if (status.Succeeded())
    {
        status = DecodeNormalRow(pair.fineEncoded, pair.fine.width, fineY.second, pair.format, pair.use.srgb, rows.fineSecond);
    }
    if (status.Succeeded())
    {
        status =
            DecodeNormalRow(pair.coarseEncoded, pair.coarse.width, coarseY.first, pair.format, pair.use.srgb, rows.coarseFirst);
    }
    if (status.Succeeded())
    {
        status = DecodeNormalRow(pair.coarseEncoded, pair.coarse.width, coarseY.second, pair.format, pair.use.srgb,
                                 rows.coarseSecond);
    }
    return status;
}

Status ObserveNormalJointBox(const NormalMipPairView& pair,
                             const NormalMipRows&     rows,
                             const NormalJointBox&    box,
                             NormalJointBoxQuality&   measured,
                             TextureMipQuality&       quality,
                             const Cancellation*      cancellation)
{
    Status status;
    for (uint32_t py = 0; status.Succeeded() && py < 3; ++py)
    {
        const double phaseY = box.first[1] + (box.last[1] - box.first[1]) * py * 0.5;
        for (uint32_t y = 0; status.Succeeded() && y < pair.fine.height; ++y)
        {
            status = CheckCancellation(cancellation);
            const NormalPhaseAxis fineMappingY =
                MakeNormalPhaseAxis(y, pair.fine.height, pair.fine.height, pair.use.filterNormalMin, pair.use.normalRepeatT);
            const NormalPhaseAxis coarseMappingY =
                MakeNormalPhaseAxis(y, pair.fine.height, pair.coarse.height, pair.use.filterNormalMin, pair.use.normalRepeatT);
            const NormalSampleAxis fineY   = SampleNormalPhase(fineMappingY, phaseY);
            const NormalSampleAxis coarseY = SampleNormalPhase(coarseMappingY, phaseY);
            if (status.Succeeded())
            {
                status = DecodeNormalMipRows(pair, rows, fineY, coarseY);
            }
            for (uint32_t x = 0; status.Succeeded() && x < pair.fine.width; ++x)
            {
                const NormalPhaseAxis fineMappingX =
                    MakeNormalPhaseAxis(x, pair.fine.width, pair.fine.width, pair.use.filterNormalMin, pair.use.normalRepeatS);
                const NormalPhaseAxis coarseMappingX = MakeNormalPhaseAxis(x, pair.fine.width, pair.coarse.width,
                                                                           pair.use.filterNormalMin, pair.use.normalRepeatS);
                for (uint32_t px = 0; px < 3; ++px)
                {
                    const double                               phaseX  = box.first[0] + (box.last[0] - box.first[0]) * px * 0.5;
                    const NormalSampleAxis                     fineX   = SampleNormalPhase(fineMappingX, phaseX);
                    const NormalSampleAxis                     coarseX = SampleNormalPhase(coarseMappingX, phaseX);
                    const std::array<std::array<double, 3>, 2> a =
                        SampleNormalPair(pair.fine, fineX, fineY, rows.fineFirst, rows.fineSecond, pair.use.srgb);
                    const std::array<std::array<double, 3>, 2> b =
                        SampleNormalPair(pair.coarse, coarseX, coarseY, rows.coarseFirst, rows.coarseSecond, pair.use.srgb);
                    for (uint32_t pt = 0; pt < 3; ++pt)
                    {
                        const double fraction = box.first[2] + (box.last[2] - box.first[2]) * pt * 0.5;
                        const double angle    = NormalAngle(BlendNormalChannels(a[0], b[0], fraction),
                                                            BlendNormalChannels(a[1], b[1], fraction), pair.format);
                        measured.observedSquared[pt * 9 + py * 3 + px] += angle * angle;
                        quality.normalMipMaxAngleDegrees                = std::max(quality.normalMipMaxAngleDegrees, angle);
                    }
                }
            }
        }
    }
    if (status.Succeeded())
    {
        const double pixels = double(pair.fine.width) * pair.fine.height;
        for (double sum : measured.observedSquared)
        {
            quality.normalMipRmsAngleDegrees = std::max(quality.normalMipRmsAngleDegrees, std::sqrt(sum / pixels));
        }
    }
    return status;
}

void MeasureNormalJointVariation(const NormalVolume& original, const NormalVolume& restored, NormalJointBoxQuality& measured)
{
    for (size_t axis = 0; axis < 3; ++axis)
    {
        for (size_t corner = 0; corner < original.size(); ++corner)
        {
            if ((corner & (size_t{1} << axis)) == 0)
            {
                const size_t          next = corner | (size_t{1} << axis);
                std::array<double, 3> a{}, b{};
                for (size_t channel = 0; channel < 3; ++channel)
                {
                    a[channel] = original[next][channel] - original[corner][channel];
                    b[channel] = restored[next][channel] - restored[corner][channel];
                }
                measured.variation[axis] = std::max({measured.variation[axis], NormalDot(a, a), NormalDot(b, b)});
            }
        }
    }
}

void MakeNormalJointPiece(const NormalMipPairView&     pair,
                          const NormalMipRows&         rows,
                          const NormalPhaseAxis&       fineX,
                          const NormalPhaseAxis&       coarseX,
                          const NormalPhaseAxis&       fineY,
                          const NormalPhaseAxis&       coarseY,
                          const std::array<double, 2>& first,
                          const std::array<double, 2>& last,
                          const NormalJointBox&        box,
                          NormalVolume&                original,
                          NormalVolume&                restored)
{
    for (size_t corner = 0; corner < original.size(); ++corner)
    {
        const double                               x        = (corner & 1) == 0 ? first[0] : last[0];
        const double                               y        = (corner & 2) == 0 ? first[1] : last[1];
        const double                               fraction = (corner & 4) == 0 ? box.first[2] : box.last[2];
        const NormalSampleAxis                     ax       = SampleNormalPiece(fineX, first[0], last[0], x);
        const NormalSampleAxis                     bx       = SampleNormalPiece(coarseX, first[0], last[0], x);
        const NormalSampleAxis                     ay       = SampleNormalPiece(fineY, first[1], last[1], y);
        const NormalSampleAxis                     by       = SampleNormalPiece(coarseY, first[1], last[1], y);
        const std::array<std::array<double, 3>, 2> a =
            SampleNormalPair(pair.fine, ax, ay, rows.fineFirst, rows.fineSecond, pair.use.srgb);
        const std::array<std::array<double, 3>, 2> b =
            SampleNormalPair(pair.coarse, bx, by, rows.coarseFirst, rows.coarseSecond, pair.use.srgb);
        original[corner] = BlendNormalChannels(a[0], b[0], fraction);
        restored[corner] = BlendNormalChannels(a[1], b[1], fraction);
    }
}

Status BoundLinearNormalJointRow(const NormalMipPairView& pair,
                                 const NormalMipRows&     rows,
                                 uint32_t                 y,
                                 const NormalJointBox&    box,
                                 std::span<double>        upper,
                                 NormalJointBoxQuality&   measured,
                                 const Cancellation*      cancellation)
{
    Status                status;
    const NormalPhaseAxis fineY   = MakeNormalPhaseAxis(y, pair.fine.height, pair.fine.height, true, pair.use.normalRepeatT);
    const NormalPhaseAxis coarseY = MakeNormalPhaseAxis(y, pair.fine.height, pair.coarse.height, true, pair.use.normalRepeatT);
    const NormalPhaseCuts yCuts   = MakeNormalPhaseCuts(fineY, coarseY, box.first[1], box.last[1]);
    for (size_t sy = 0; status.Succeeded() && sy + 1 < yCuts.count; ++sy)
    {
        status              = CheckCancellation(cancellation);
        const double firstY = yCuts.values[sy], lastY = yCuts.values[sy + 1];
        if (status.Succeeded())
        {
            status = DecodeNormalMipRows(pair, rows, SampleNormalPiece(fineY, firstY, lastY, firstY),
                                         SampleNormalPiece(coarseY, firstY, lastY, firstY));
        }
        for (uint32_t x = 0; status.Succeeded() && x < pair.fine.width; ++x)
        {
            const NormalPhaseAxis fineX =
                MakeNormalPhaseAxis(x, pair.fine.width, pair.fine.width, true, pair.use.normalRepeatS);
            const NormalPhaseAxis coarseX =
                MakeNormalPhaseAxis(x, pair.fine.width, pair.coarse.width, true, pair.use.normalRepeatS);
            const NormalPhaseCuts xCuts = MakeNormalPhaseCuts(fineX, coarseX, box.first[0], box.last[0]);
            for (size_t sx = 0; sx + 1 < xCuts.count; ++sx)
            {
                NormalVolume original{}, restored{};
                MakeNormalJointPiece(pair, rows, fineX, coarseX, fineY, coarseY, {xCuts.values[sx], firstY},
                                     {xCuts.values[sx + 1], lastY}, box, original, restored);
                upper[x] = std::max(upper[x], NormalTensorAngleBound<3>(original, restored, pair.format));
                MeasureNormalJointVariation(original, restored, measured);
            }
        }
    }
    return status;
}

void MeasureNearestNormalVariation(const std::array<double, 3>& first,
                                   const std::array<double, 3>& last,
                                   size_t                       axis,
                                   const NormalJointBox&        box,
                                   NormalJointBoxQuality&       measured)
{
    std::array<double, 3> delta{};
    for (size_t channel = 0; channel < 3; ++channel)
    {
        delta[channel] = last[channel] - first[channel];
    }
    const double width       = box.last[axis] - box.first[axis];
    measured.variation[axis] = std::max(measured.variation[axis], NormalDot(delta, delta) * width * width);
}

Status BoundNearestNormalJointRow(const NormalMipPairView& pair,
                                  const NormalMipRows&     rows,
                                  uint32_t                 y,
                                  const NormalJointBox&    box,
                                  std::span<double>        upper,
                                  NormalJointBoxQuality&   measured,
                                  const Cancellation*      cancellation)
{
    Status                status;
    const NormalPhaseAxis fineY   = MakeNormalPhaseAxis(y, pair.fine.height, pair.fine.height, false, pair.use.normalRepeatT);
    const NormalPhaseAxis coarseY = MakeNormalPhaseAxis(y, pair.fine.height, pair.coarse.height, false, pair.use.normalRepeatT);
    const NormalNearestStates yStates = MakeNormalNearestStates(fineY, coarseY, box.first[1], box.last[1]);
    for (size_t sy = 0; status.Succeeded() && sy < yStates.count; ++sy)
    {
        status                            = CheckCancellation(cancellation);
        const NormalNearestTapPair yState = yStates.values[sy];
        if (status.Succeeded())
        {
            if (sy == 0)
            {
                // Preserve the first state's decoded rows for the Y scheduling
                // heuristic; nearest sampling never consumes a second tap.
                status = DecodeNormalMipRows(pair, rows, {yState.fine, yState.fine, 0}, {yState.coarse, yState.coarse, 0});
            }
            else
            {
                status =
                    DecodeNormalRow(pair.fineEncoded, pair.fine.width, yState.fine, pair.format, pair.use.srgb, rows.fineFirst);
                if (status.Succeeded())
                {
                    status = DecodeNormalRow(pair.coarseEncoded, pair.coarse.width, yState.coarse, pair.format, pair.use.srgb,
                                             rows.coarseFirst);
                }
            }
        }
        for (uint32_t x = 0; status.Succeeded() && x < pair.fine.width; ++x)
        {
            const NormalPhaseAxis fineX =
                MakeNormalPhaseAxis(x, pair.fine.width, pair.fine.width, false, pair.use.normalRepeatS);
            const NormalPhaseAxis coarseX =
                MakeNormalPhaseAxis(x, pair.fine.width, pair.coarse.width, false, pair.use.normalRepeatS);
            const NormalNearestStates xStates = MakeNormalNearestStates(fineX, coarseX, box.first[0], box.last[0]);
            for (size_t sx = 0; sx < xStates.count; ++sx)
            {
                const NormalNearestTapPair  xState = xStates.values[sx];
                const std::array<double, 3> a =
                    ReadRgb(pair.fine.bytes.data() + (size_t(yState.fine) * pair.fine.width + xState.fine) * 4, pair.use.srgb);
                const std::array<double, 3> b = ReadRgb(
                    pair.coarse.bytes.data() + (size_t(yState.coarse) * pair.coarse.width + xState.coarse) * 4, pair.use.srgb);
                const std::array<double, 3> da = rows.fineFirst[xState.fine], db = rows.coarseFirst[xState.coarse];
                const NormalNearestTapPair  firstX = xStates.values[0], firstY = yStates.values[0];
                MeasureNearestNormalVariation(
                    ReadRgb(pair.fine.bytes.data() + (size_t(yState.fine) * pair.fine.width + firstX.fine) * 4, pair.use.srgb),
                    a, 0, box, measured);
                MeasureNearestNormalVariation(
                    ReadRgb(pair.coarse.bytes.data() + (size_t(yState.coarse) * pair.coarse.width + firstX.coarse) * 4,
                            pair.use.srgb),
                    b, 0, box, measured);
                MeasureNearestNormalVariation(rows.fineFirst[firstX.fine], da, 0, box, measured);
                MeasureNearestNormalVariation(rows.coarseFirst[firstX.coarse], db, 0, box, measured);
                MeasureNearestNormalVariation(
                    ReadRgb(pair.fine.bytes.data() + (size_t(firstY.fine) * pair.fine.width + xState.fine) * 4, pair.use.srgb),
                    a, 1, box, measured);
                MeasureNearestNormalVariation(
                    ReadRgb(pair.coarse.bytes.data() + (size_t(firstY.coarse) * pair.coarse.width + xState.coarse) * 4,
                            pair.use.srgb),
                    b, 1, box, measured);
                MeasureNearestNormalVariation(rows.fineSecond[xState.fine], da, 1, box, measured);
                MeasureNearestNormalVariation(rows.coarseSecond[xState.coarse], db, 1, box, measured);
                const std::array<double, 3> first        = BlendNormalChannels(a, b, box.first[2]);
                const std::array<double, 3> last         = BlendNormalChannels(a, b, box.last[2]);
                const std::array<double, 3> decodedFirst = BlendNormalChannels(da, db, box.first[2]);
                const std::array<double, 3> decodedLast  = BlendNormalChannels(da, db, box.last[2]);
                upper[x] = std::max(upper[x], NormalMipAngleBound(first, last, decodedFirst, decodedLast, pair.format));
                std::array<double, 3> delta{}, decodedDelta{};
                for (size_t channel = 0; channel < 3; ++channel)
                {
                    delta[channel]        = last[channel] - first[channel];
                    decodedDelta[channel] = decodedLast[channel] - decodedFirst[channel];
                }
                measured.variation[2] =
                    std::max({measured.variation[2], NormalDot(delta, delta), NormalDot(decodedDelta, decodedDelta)});
            }
        }
    }
    return status;
}

Status BoundNormalJointBox(const NormalMipPairView& pair,
                           const NormalMipRows&     rows,
                           const NormalJointBox&    box,
                           std::span<double>        rowUpper,
                           NormalJointBoxQuality&   measured,
                           const Cancellation*      cancellation)
{
    Status status;
    for (uint32_t y = 0; status.Succeeded() && y < pair.fine.height; ++y)
    {
        std::fill(rowUpper.begin(), rowUpper.end(), 0);
        status = pair.use.filterNormalMin ? BoundLinearNormalJointRow(pair, rows, y, box, rowUpper, measured, cancellation)
                                          : BoundNearestNormalJointRow(pair, rows, y, box, rowUpper, measured, cancellation);
        if (status.Succeeded())
        {
            for (double upper : rowUpper)
            {
                // Each cell contributes exactly once, after all of its pieces
                // or discrete sampler states have been bounded.
                AccumulateNormalUpperSquared(upper, measured.upperSquared);
                measured.upperMaximum = std::max(measured.upperMaximum, upper);
            }
        }
    }
    return status;
}

Status QualifyJointNormalMipPair(const ImageLevel&        fine,
                                 const ImageLevel&        coarse,
                                 std::span<const uint8_t> fineEncoded,
                                 std::span<const uint8_t> coarseEncoded,
                                 PixelFormat              format,
                                 const TextureImageUse&   use,
                                 const TextureSettings&   settings,
                                 uint64_t&                remaining,
                                 TextureMipQuality&       quality,
                                 std::string&             reason,
                                 const Cancellation*      cancellation)
{
    ZENASSET_MEASURE_SCOPE(TextureEncodeQualify);
    ImageBudget                                                   budget(remaining);
    Status                                                        status;
    std::vector<std::array<double, 3>>                            storage;
    std::vector<double>                                           rowUpper;
    std::array<NormalJointBox, NormalMaximumSubdivisionDepth + 1> pending{};
    const NormalMipPairView                                       pair{fine, coarse, fineEncoded, coarseEncoded, format, use};
    const uint64_t                                                rowCount   = 2 * (uint64_t(fine.width) + coarse.width);
    const uint64_t                                                rowBytes   = rowCount * sizeof(storage[0]);
    const uint64_t                                                upperBytes = uint64_t(fine.width) * sizeof(double);
    const uint64_t fixedBytes = sizeof(pending) + sizeof(NormalJointBoxQuality) + sizeof(NormalMipRows)
                              + 5 * sizeof(NormalVolume) + sizeof(std::array<std::array<double, 3>, 27>)
                              + sizeof(std::array<double, 27>) + 2 * sizeof(NormalPhaseCuts) + 2 * sizeof(NormalNearestStates);
    if (rowBytes > SIZE_MAX || upperBytes > SIZE_MAX || !budget.Reserve(rowBytes + upperBytes + fixedBytes))
    {
        status = {StatusCode::ResourceLimit, "Joint normal mip quality working storage exceeds budget"};
    }
    else
    {
        storage.resize(static_cast<size_t>(rowCount));
        rowUpper.resize(fine.width);
        const NormalMipRows rows{std::span(storage).first(fine.width), std::span(storage).subspan(fine.width, fine.width),
                                 std::span(storage).subspan(size_t(fine.width) * 2, coarse.width),
                                 std::span(storage).subspan(size_t(fine.width) * 2 + coarse.width, coarse.width)};
        size_t              count = 1;
        while (status.Succeeded() && reason.empty() && count != 0)
        {
            const NormalJointBox  box = pending[--count];
            NormalJointBoxQuality measured;
            ++quality.normalJointBoxCount;
            status = ObserveNormalJointBox(pair, rows, box, measured, quality, cancellation);
            if (status.Succeeded()
                && (quality.normalMipRmsAngleDegrees > settings.normalRmsAngleDegrees
                    || quality.normalMipMaxAngleDegrees > settings.normalMaxAngleDegrees))
            {
                reason = format == PixelFormat::BC5Unorm ? "BC5 joint UV/LOD sampling exceeds normal angular limits"
                                                         : "BC7 joint UV/LOD sampling exceeds normal angular limits";
            }
            if (status.Succeeded() && reason.empty())
            {
                status = BoundNormalJointBox(pair, rows, box, rowUpper, measured, cancellation);
            }
            if (status.Succeeded() && reason.empty())
            {
                const double rms = UpperNormalRms(measured.upperSquared, double(fine.width) * fine.height);
                if (rms <= settings.normalRmsAngleDegrees && measured.upperMaximum <= settings.normalMaxAngleDegrees)
                {
                    quality.normalMipRmsUpperBoundDegrees =
                        std::max({quality.normalMipRmsUpperBoundDegrees, rms, quality.normalMipRmsAngleDegrees});
                    quality.normalMipMaxUpperBoundDegrees = std::max(
                        {quality.normalMipMaxUpperBoundDegrees, measured.upperMaximum, quality.normalMipMaxAngleDegrees});
                }
                else
                {
                    size_t axis = 0;
                    for (size_t candidate = 1; candidate < measured.variation.size(); ++candidate)
                    {
                        if (measured.variation[candidate] > measured.variation[axis])
                        {
                            axis = candidate;
                        }
                    }
                    // Variation chooses work order only. No observed variation
                    // never turns an unproven bound into a certificate.
                    if (box.depth == NormalMaximumSubdivisionDepth || quality.normalJointBoxCount == NormalMaximumRegionCount
                        || measured.variation[axis] == 0)
                    {
                        reason = format == PixelFormat::BC5Unorm
                                   ? "BC5 joint UV/LOD sampling could not certify normal angular limits"
                                   : "BC7 joint UV/LOD sampling could not certify normal angular limits";
                    }
                    else
                    {
                        const double   middle = (box.first[axis] + box.last[axis]) * 0.5;
                        NormalJointBox first = box, last = box;
                        ++first.depth;
                        ++last.depth;
                        first.last[axis] = last.first[axis] = middle;
                        pending[count++]                    = last;
                        pending[count++]                    = first;
                    }
                }
            }
            if (status.Succeeded() && reason.empty() && count != 0 && quality.normalJointBoxCount == NormalMaximumRegionCount)
            {
                reason = format == PixelFormat::BC5Unorm ? "BC5 joint UV/LOD sampling could not certify normal angular limits"
                                                         : "BC7 joint UV/LOD sampling could not certify normal angular limits";
            }
        }
        quality.normalJointContinuousCertified = status.Succeeded() && reason.empty() && count == 0;
    }
    return status;
}

PixelFormat EncodedFormat(const TextureImageUse& use, const TextureSettings& settings)
{
    PixelFormat format = PixelFormat::Undefined;
    if (!use.srgb && use.role == TextureImageRole::Normal && settings.compressBC5Normals)
    {
        format = PixelFormat::BC5Unorm;
    }
    else if (!use.srgb && use.role == TextureImageRole::Occlusion && settings.compressBC4Occlusion)
    {
        format = PixelFormat::BC4Unorm;
    }
    else if (settings.compressBC7)
    {
        format = use.srgb ? PixelFormat::BC7Srgb : PixelFormat::BC7Unorm;
    }
    return format;
}

Status ValidateNormalXY(const ImageScratch& scratch, uint32_t count, std::string& reason, const Cancellation* cancellation)
{
    Status status;
    for (uint32_t level = 0; status.Succeeded() && reason.empty() && level < count; ++level)
    {
        const ImageLevel& image = scratch.levels[level];
        for (uint32_t y = 0; status.Succeeded() && reason.empty() && y < image.height; ++y)
        {
            status = CheckCancellation(cancellation);
            for (uint32_t x = 0; status.Succeeded() && reason.empty() && x < image.width; ++x)
            {
                const uint8_t*        pixel = image.bytes.data() + (size_t(y) * image.width + x) * 4;
                std::array<double, 3> normal{};
                if (!ReadNormal(ReadRgb(pixel, false), normal) || normal[2] < 0)
                {
                    reason = "BC5 normal encoding requires defined positive-Z directions in every authored mip";
                }
            }
        }
    }
    return status;
}

double Coverage(const ImageLevel& level, float cutoff, const Cancellation* cancellation, Status& status)
{
    uint64_t count = 0;
    for (uint32_t row = 0; status.Succeeded() && row < level.height; ++row)
    {
        status = CheckCancellation(cancellation);
        for (uint32_t x = 0; status.Succeeded() && x < level.width; ++x)
        {
            count += level.bytes[(size_t(row) * level.width + x) * 4 + 3] / 255.0f >= cutoff ? 1 : 0;
        }
    }
    return double(count) / (uint64_t(level.width) * level.height);
}

Status ReadLevels(const ImagePixels& input, ImageScratch& scratch, uint32_t& count, const Cancellation* cancellation)
{
    Status   status;
    uint32_t width  = input.width;
    uint32_t height = input.height;
    count           = input.mips.empty() ? 1 : static_cast<uint32_t>(input.mips.size());
    if (input.mips.size() > scratch.levels.size() || width == 0 || height == 0
        || (!input.mips.empty() && input.bytes.size() != input.mips[0].size()))
    {
        status = {StatusCode::InvalidData, "Invalid texture mip ownership or extent"};
    }
    for (uint32_t level = 0; status.Succeeded() && level < count; ++level)
    {
        const std::vector<uint8_t>& bytes    = input.mips.empty() ? input.bytes : input.mips[level];
        uint64_t                    expected = 0;
        if (!GetImageLevelByteSize(input.format, width, height, expected) || bytes.size() != expected
            || (level > 0 && scratch.levels[level - 1].width == 1 && scratch.levels[level - 1].height == 1))
        {
            status = {StatusCode::InvalidData, "Invalid texture mip byte footprint"};
        }
        else
        {
            scratch.levels[level].width  = width;
            scratch.levels[level].height = height;
            scratch.levels[level].bytes  = bytes;
            width                        = std::max(1u, width / 2);
            height                       = std::max(1u, height / 2);
        }
    }
    if (status.Succeeded() && !input.mips.empty())
    {
        const size_t stride = size_t(input.width) * 4;
        for (uint32_t row = 0; status.Succeeded() && row < input.height; ++row)
        {
            status              = CheckCancellation(cancellation);
            const size_t offset = size_t(row) * stride;
            if (status.Succeeded()
                && !std::equal(input.bytes.begin() + offset, input.bytes.begin() + offset + stride,
                               input.mips[0].begin() + offset))
            {
                status = {StatusCode::InvalidData, "Texture base bytes differ from the first authored mip"};
            }
        }
    }
    return status;
}

Status FilterLevel(const ImageLevel&      source,
                   ImageLevel&            target,
                   const TextureImageUse& use,
                   ImageBudget&           budget,
                   std::string&           reason,
                   const Cancellation*    cancellation)
{
    Status status;
    target.width   = std::max(1u, source.width / 2);
    target.height  = std::max(1u, source.height / 2);
    uint64_t bytes = 0;
    if (!GetImageLevelByteSize(PixelFormat::RGBA8Unorm, target.width, target.height, bytes) || bytes > SIZE_MAX
        || !budget.Reserve(bytes))
    {
        status = {StatusCode::ResourceLimit, "Texture mip working storage exceeds budget"};
    }
    else
    {
        target.generated.resize(static_cast<size_t>(bytes));
        target.bytes = target.generated;
    }
    for (uint32_t y = 0; status.Succeeded() && reason.empty() && y < target.height; ++y)
    {
        status              = CheckCancellation(cancellation);
        const double top    = double(y) * source.height / target.height;
        const double bottom = double(y + 1) * source.height / target.height;
        for (uint32_t x = 0; status.Succeeded() && reason.empty() && x < target.width; ++x)
        {
            const double          left  = double(x) * source.width / target.width;
            const double          right = double(x + 1) * source.width / target.width;
            std::array<double, 4> sum{};
            double                total = 0;
            for (uint32_t sy = static_cast<uint32_t>(top); sy < static_cast<uint32_t>(std::ceil(bottom)); ++sy)
            {
                for (uint32_t sx = static_cast<uint32_t>(left); sx < static_cast<uint32_t>(std::ceil(right)); ++sx)
                {
                    const double          weight = (std::min(right, double(sx + 1)) - std::max(left, double(sx)))
                                                 * (std::min(bottom, double(sy + 1)) - std::max(top, double(sy)));
                    const uint8_t*        pixel  = source.bytes.data() + (size_t(sy) * source.width + sx) * 4;
                    std::array<double, 3> rgb    = ReadRgb(pixel, use.srgb);
                    if (use.role == TextureImageRole::Normal)
                    {
                        std::array<double, 3> normal{};
                        if (!ReadNormal(rgb, normal))
                        {
                            reason = "Normal mip contains an undefined direction";
                        }
                        rgb = normal;
                    }
                    for (size_t channel = 0; channel < 3; ++channel)
                    {
                        sum[channel] += rgb[channel] * weight;
                    }
                    sum[3] += pixel[3] / 255.0 * weight;
                    total  += weight;
                }
            }
            std::array<double, 3> rgb{sum[0] / total, sum[1] / total, sum[2] / total};
            if (use.role == TextureImageRole::Normal)
            {
                if (!Normalize(rgb))
                {
                    reason = "Normal mip averages to an undefined direction";
                }
                for (double& value : rgb)
                {
                    value = value * 0.5 + 0.5;
                }
            }
            const size_t offset = (size_t(y) * target.width + x) * 4;
            for (size_t channel = 0; channel < 3; ++channel)
            {
                target.generated[offset + channel] = ToByte(rgb[channel], use.srgb);
            }
            target.generated[offset + 3] = ToByte(sum[3] / total, false);
        }
    }
    return status;
}

// Choose a monotone alpha scale from the finite set of byte thresholds. If the
// requested coverage is not representable at this mip size, reject the image.
Status PreserveCoverage(ImageLevel&            level,
                        double                 wanted,
                        const TextureImageUse& use,
                        const TextureSettings& settings,
                        ImageScratch&          scratch,
                        std::string&           reason,
                        const Cancellation*    cancellation)
{
    Status status;
    level.targetCoverage  = wanted;
    const double original = Coverage(level, use.alphaCutoff, cancellation, status);
    level.coverageError   = std::abs(original - wanted);
    if (status.Succeeded() && level.coverageError > settings.alphaCoverageMaxError && use.alphaCutoff > 0
        && use.alphaCutoff <= 1)
    {
        scratch.histogram.fill(0);
        for (uint32_t y = 0; status.Succeeded() && y < level.height; ++y)
        {
            status = CheckCancellation(cancellation);
            for (uint32_t x = 0; status.Succeeded() && x < level.width; ++x)
            {
                ++scratch.histogram[level.generated[(size_t(y) * level.width + x) * 4 + 3]];
            }
        }
        const uint64_t total     = uint64_t(level.width) * level.height;
        uint64_t       covered   = 0;
        uint32_t       threshold = 256;
        double         best      = wanted;
        for (uint32_t candidate = 255; candidate > 0; --candidate)
        {
            covered            += scratch.histogram[candidate];
            const double error  = std::abs(double(covered) / total - wanted);
            if (error < best)
            {
                best      = error;
                threshold = candidate;
            }
        }
        uint32_t cutoffByte = 0;
        while (cutoffByte < 255 && float(cutoffByte) / 255.0f < use.alphaCutoff)
        {
            ++cutoffByte;
        }
        const double scale = threshold == 256 ? 0.0 : (cutoffByte - 0.5) / (double(threshold) - 0.5);
        for (uint32_t y = 0; status.Succeeded() && y < level.height; ++y)
        {
            status = CheckCancellation(cancellation);
            for (uint32_t x = 0; status.Succeeded() && x < level.width; ++x)
            {
                const size_t offset     = (size_t(y) * level.width + x) * 4 + 3;
                level.generated[offset] = ToByte(level.generated[offset] / 255.0 * scale, false);
            }
        }
        level.coverageError = std::abs(Coverage(level, use.alphaCutoff, cancellation, status) - wanted);
    }
    if (status.Succeeded() && level.coverageError > settings.alphaCoverageMaxError)
    {
        reason = "Generated mip cannot preserve alpha-mask coverage";
    }
    return status;
}

struct QualitySums
{
    double   colorSquared{0};
    double   normalSquared{0};
    uint64_t referenceCovered{0};
    uint64_t decodedCovered{0};
};

void MeasurePixel(const uint8_t*         source,
                  const Vec4&            decoded,
                  const TextureImageUse& use,
                  PixelFormat            format,
                  TextureMipQuality&     quality,
                  QualitySums&           sums)
{
    const std::array<double, 3>  reference = ReadRgb(source, use.srgb);
    const std::array<uint8_t, 4> restoredBytes{ToByte(decoded.x, false), ToByte(decoded.y, false), ToByte(decoded.z, false),
                                               ToByte(decoded.w, false)};
    const std::array<double, 3>  restored = ReadRgb(restoredBytes.data(), use.srgb);
    for (size_t channel = 0; channel < 3; ++channel)
    {
        const double error  = std::abs(reference[channel] - restored[channel]);
        sums.colorSquared  += error * error;
        if (use.role == TextureImageRole::Data || (use.role == TextureImageRole::Occlusion && channel == 0))
        {
            const double consumedError =
                format == PixelFormat::BC4Unorm ? std::abs(double(source[0] / 255.0f) - decoded.x) : error;
            quality.scalarMaxError = std::max(quality.scalarMaxError, consumedError);
        }
    }
    if (format != PixelFormat::BC5Unorm && use.role != TextureImageRole::Occlusion)
    {
        const double alphaError  = std::abs(double(source[3]) - restoredBytes[3]) / 255.0;
        quality.scalarMaxError   = std::max(quality.scalarMaxError, alphaError);
        quality.exactAlpha       = quality.exactAlpha && source[3] == restoredBytes[3];
        sums.referenceCovered   += source[3] / 255.0f >= use.alphaCutoff ? 1 : 0;
        sums.decodedCovered     += restoredBytes[3] / 255.0f >= use.alphaCutoff ? 1 : 0;
    }
    if (use.role == TextureImageRole::Normal)
    {
        const double angle             = NormalAngle(reference, DecodedNormalRgb(decoded, format, use.srgb), format);
        sums.normalSquared            += angle * angle;
        quality.normalMaxAngleDegrees  = std::max(quality.normalMaxAngleDegrees, angle);
    }
}

void CheckQuality(const TextureImageUse&   use,
                  const TextureSettings&   settings,
                  const TextureMipQuality& quality,
                  PixelFormat              format,
                  std::string&             reason)
{
    if (use.role == TextureImageRole::Color
        && (quality.colorRmse > settings.colorMaxRmse || quality.colorPsnr < settings.colorMinPsnr))
    {
        reason = "BC7 mip exceeds linear RGB quality limits";
    }
    else if (use.role == TextureImageRole::Normal
             && (quality.normalRmsAngleDegrees > settings.normalRmsAngleDegrees
                 || quality.normalMaxAngleDegrees > settings.normalMaxAngleDegrees))
    {
        reason = format == PixelFormat::BC5Unorm ? "BC5 reconstructed mip exceeds normal angular limits"
                                                 : "BC7 mip exceeds normal angular limits";
    }
    else if (use.role == TextureImageRole::Normal
             && (quality.normalFilteredRmsAngleDegrees > settings.normalRmsAngleDegrees
                 || quality.normalFilteredMaxAngleDegrees > settings.normalMaxAngleDegrees))
    {
        reason = format == PixelFormat::BC5Unorm ? "BC5 filtered mip exceeds normal angular limits"
                                                 : "BC7 filtered mip exceeds normal angular limits";
    }
    else if (quality.scalarMaxError > settings.scalarMaxError)
    {
        reason = format == PixelFormat::BC4Unorm ? "BC4 mip exceeds consumed-R scalar error limit"
                                                 : "BC7 mip exceeds scalar channel error limit";
    }
    else if (use.exactAlpha && !quality.exactAlpha)
    {
        reason = "BC7 mip does not preserve exact alpha";
    }
    else if (use.alphaMask && quality.alphaCoverageError > settings.alphaCoverageMaxError)
    {
        reason = "BC7 mip does not preserve alpha-mask coverage";
    }
}

Status EncodeLevel(const ImageLevel&      source,
                   PixelFormat            format,
                   std::vector<uint8_t>&  target,
                   const TextureImageUse& use,
                   const TextureSettings& settings,
                   ImageScratch&          scratch,
                   TextureMipQuality&     quality,
                   std::string&           reason,
                   uint64_t&              remaining,
                   const Cancellation*    cancellation)
{
    ZENASSET_MEASURE_SCOPE(TextureEncodeQualify);
    ZENASSET_MEASURE_ADD(TextureCandidates, 1);
    Status      status;
    QualitySums sums;
    quality.width              = source.width;
    quality.height             = source.height;
    const uint32_t  blocksWide = static_cast<uint32_t>((uint64_t(source.width) + 3) / 4);
    const uint32_t  blocksHigh = static_cast<uint32_t>((uint64_t(source.height) + 3) / 4);
    PixelFormatInfo info;
    GetPixelFormatInfo(format, info);
    for (uint32_t by = 0; status.Succeeded() && reason.empty() && by < blocksHigh; ++by)
    {
        for (uint32_t bx = 0; status.Succeeded() && reason.empty() && bx < blocksWide; ++bx)
        {
            status = CheckCancellation(cancellation);
            if (status.Succeeded())
            {
                for (uint32_t y = 0; y < 4; ++y)
                {
                    for (uint32_t x = 0; x < 4; ++x)
                    {
                        const uint32_t sx    = std::min(source.width - 1, bx * 4 + x);
                        const uint32_t sy    = std::min(source.height - 1, by * 4 + y);
                        const uint8_t* pixel = source.bytes.data() + (size_t(sy) * source.width + sx) * 4;
                        std::copy_n(pixel, 4, scratch.rgba.data() + (y * 4 + x) * 4);
                        if (format == PixelFormat::BC5Unorm)
                        {
                            std::array<double, 3> normal{};
                            if (!ReadNormal(ReadRgb(pixel, false), normal) || normal[2] < 0)
                            {
                                reason = "BC5 normal encoding requires defined positive-Z directions";
                            }
                            else
                            {
                                scratch.rgba[(y * 4 + x) * 4]     = ToByte(normal[0] * 0.5 + 0.5, false);
                                scratch.rgba[(y * 4 + x) * 4 + 1] = ToByte(normal[1] * 0.5 + 0.5, false);
                            }
                        }
                    }
                }
                const size_t               offset = (size_t(by) * blocksWide + bx) * info.bytesPerBlock;
                const std::span<std::byte> block{reinterpret_cast<std::byte*>(target.data() + offset), info.bytesPerBlock};
                if (reason.empty())
                {
                    status = EncodeBlock(format, scratch.rgba, block, settings.bc7Quality);
                }
            }
        }
    }
    if (status.Succeeded() && reason.empty() && format == PixelFormat::BC7Unorm && use.role == TextureImageRole::Normal
        && use.filterNormals && settings.bc7Quality == 4)
    {
        status = RefineBC7NormalLevel(source.width, source.height, source.bytes, target, use, remaining, cancellation);
    }
    for (uint32_t by = 0; status.Succeeded() && reason.empty() && by < blocksHigh; ++by)
    {
        for (uint32_t bx = 0; status.Succeeded() && reason.empty() && bx < blocksWide; ++bx)
        {
            status = CheckCancellation(cancellation);
            if (status.Succeeded())
            {
                const size_t                     offset = (size_t(by) * blocksWide + bx) * info.bytesPerBlock;
                const std::span<const std::byte> block{reinterpret_cast<const std::byte*>(target.data() + offset),
                                                       info.bytesPerBlock};
                status = DecodeBlock(format, block, scratch.decoded);
                for (uint32_t y = 0; status.Succeeded() && reason.empty() && y < 4 && by * 4 + y < source.height; ++y)
                {
                    for (uint32_t x = 0; x < 4 && bx * 4 + x < source.width; ++x)
                    {
                        const uint8_t* original = source.bytes.data() + (size_t(by * 4 + y) * source.width + bx * 4 + x) * 4;
                        MeasurePixel(original, scratch.decoded[y * 4 + x], use, format, quality, sums);
                    }
                }
            }
        }
    }
    if (status.Succeeded() && reason.empty())
    {
        const double pixels = double(source.width) * source.height;
        quality.colorRmse   = std::sqrt(sums.colorSquared / (pixels * 3));
        quality.colorPsnr =
            quality.colorRmse == 0 ? std::numeric_limits<double>::infinity() : -20.0 * std::log10(quality.colorRmse);
        quality.normalRmsAngleDegrees = std::sqrt(sums.normalSquared / pixels);
        const double targetCoverage =
            source.targetCoverage >= 0 ? source.targetCoverage : double(sums.referenceCovered) / pixels;
        quality.alphaCoverageError = std::abs(targetCoverage - double(sums.decodedCovered) / pixels);
        if (use.role == TextureImageRole::Normal && use.filterNormals)
        {
            status = MeasureFilteredNormals(source, target, format, use, remaining, quality, cancellation);
        }
        if (status.Succeeded())
        {
            CheckQuality(use, settings, quality, format, reason);
        }
        if (status.Succeeded() && reason.empty() && use.role == TextureImageRole::Normal && use.filterNormals)
        {
            status = QualifyNormalUv(source, target, format, use, settings, remaining, quality, reason, cancellation);
        }
    }
    return status;
}

TextureImageResult MakeCandidate(std::shared_ptr<const ImagePixels> input,
                                 const TextureImageUse&             use,
                                 const TextureSettings&             settings,
                                 uint64_t&                          remaining,
                                 const Cancellation*                cancellation)
{
    TextureImageResult result;
    ImageBudget        budget(remaining);
    ImageScratch       scratch;
    uint32_t           count = 0;
    if (!budget.Reserve(sizeof(scratch)))
    {
        result.status = {StatusCode::ResourceLimit, "Texture working storage exceeds budget"};
    }
    else
    {
        result.status = ReadLevels(*input, scratch, count, cancellation);
    }
    const PixelFormat encoding = EncodedFormat(use, settings);
    const bool        generate = settings.generateMipmaps || (encoding != PixelFormat::Undefined && use.requiresMipmaps);
    if (result.status.Succeeded() && encoding == PixelFormat::BC5Unorm)
    {
        result.status = ValidateNormalXY(scratch, count, result.reason, cancellation);
    }
    if (result.status.Succeeded() && generate && use.exactAlpha
        && (scratch.levels[count - 1].width > 1 || scratch.levels[count - 1].height > 1))
    {
        result.reason = "Exact alpha requires preserving the original incomplete mip chain";
    }
    const double baseCoverage = result.status.Succeeded() && use.alphaMask
                                  ? Coverage(scratch.levels[0], use.alphaCutoff, cancellation, result.status)
                                  : 0;
    while (result.status.Succeeded() && result.reason.empty() && generate
           && (scratch.levels[count - 1].width > 1 || scratch.levels[count - 1].height > 1))
    {
        result.status = FilterLevel(scratch.levels[count - 1], scratch.levels[count], use, budget, result.reason, cancellation);
        if (result.status.Succeeded() && result.reason.empty() && use.alphaMask)
        {
            result.status =
                PreserveCoverage(scratch.levels[count], baseCoverage, use, settings, scratch, result.reason, cancellation);
        }
        ++count;
    }
    uint64_t                     persistent  = 0;
    uint64_t                     metricBytes = 0;
    std::shared_ptr<ImagePixels> output;
    if (result.status.Succeeded() && result.reason.empty())
    {
        const uint64_t metadata = sizeof(ImagePixels) + 2 * sizeof(void*) + uint64_t(count) * sizeof(std::vector<uint8_t>);
        const uint64_t metrics  = uint64_t(count) * sizeof(TextureMipQuality);
        if (!budget.Reserve(metadata + metrics))
        {
            result.status = {StatusCode::ResourceLimit, "Texture output metadata exceeds budget"};
        }
        else
        {
            persistent     = metadata + metrics;
            metricBytes    = metrics;
            output         = std::make_shared<ImagePixels>();
            output->width  = input->width;
            output->height = input->height;
            output->format = encoding != PixelFormat::Undefined ? encoding : input->format;
            output->mips.resize(count);
            result.metrics.reserve(count);
        }
    }
    for (uint32_t level = 0; result.status.Succeeded() && result.reason.empty() && output && level < count; ++level)
    {
        result.status  = CheckCancellation(cancellation);
        uint64_t bytes = 0;
        if (result.status.Succeeded()
            && (!GetImageLevelByteSize(output->format, scratch.levels[level].width, scratch.levels[level].height, bytes)
                || bytes > SIZE_MAX || (level == 0 && bytes > UINT64_MAX / 2)
                || !budget.Reserve(bytes + (level == 0 ? bytes : 0))))
        {
            result.status = {StatusCode::ResourceLimit, "Texture output pixels exceed budget"};
        }
        if (result.status.Succeeded())
        {
            persistent += bytes + (level == 0 ? bytes : 0);
            output->mips[level].resize(static_cast<size_t>(bytes));
            TextureMipQuality quality;
            quality.width              = scratch.levels[level].width;
            quality.height             = scratch.levels[level].height;
            quality.alphaCoverageError = scratch.levels[level].coverageError;
            if (encoding != PixelFormat::Undefined)
            {
                result.status = EncodeLevel(scratch.levels[level], output->format, output->mips[level], use, settings, scratch,
                                            quality, result.reason, remaining, cancellation);
                if (result.status.Succeeded() && result.reason.empty() && level > 0 && use.role == TextureImageRole::Normal
                    && use.requiresMipmaps && use.filterNormalMips)
                {
                    result.status = QualifyJointNormalMipPair(scratch.levels[level - 1], scratch.levels[level],
                                                              output->mips[level - 1], output->mips[level], output->format, use,
                                                              settings, remaining, quality, result.reason, cancellation);
                }
            }
            else
            {
                std::copy(scratch.levels[level].bytes.begin(), scratch.levels[level].bytes.end(), output->mips[level].begin());
            }
            result.metrics.push_back(quality);
            if (level == 0)
            {
                output->bytes = output->mips[0];
            }
        }
    }
    if (result.status.Succeeded() && !result.reason.empty())
    {
        output.reset();
        budget.Release(persistent - metricBytes);
        persistent = metricBytes;
        if (!budget.Reserve(result.reason.size() + 1))
        {
            result.status = {StatusCode::ResourceLimit, "Texture diagnostic exceeds budget"};
        }
        else
        {
            persistent    += result.reason.size() + 1;
            result.pixels  = std::move(input);
        }
    }
    else if (result.status.Succeeded())
    {
        result.pixels     = std::move(output);
        result.pixelBytes = persistent - metricBytes;
    }
    if (result.status.Succeeded())
    {
        result.ownedBytes = persistent;
        budget.Retain(persistent);
    }
    else
    {
        result.pixels.reset();
        std::vector<TextureMipQuality>().swap(result.metrics);
        std::string().swap(result.reason);
    }
    return result;
}
} // namespace

TextureImageResult ProcessTextureImage(std::shared_ptr<const ImagePixels> input,
                                       const TextureImageUse&             use,
                                       const TextureSettings&             settings,
                                       uint64_t&                          remaining,
                                       const Cancellation*                cancellation)
{
    TextureImageResult result;
    result.status = CheckCancellation(cancellation);
    if (result.status.Succeeded())
    {
        ProcessingSettings processing;
        processing.textures = settings;
        result.status       = ValidateProcessingSettings(processing);
    }
    if (result.status.Succeeded()
        && (!input || !std::isfinite(use.alphaCutoff)
            || (use.role != TextureImageRole::Color && use.role != TextureImageRole::Data
                && use.role != TextureImageRole::Normal && use.role != TextureImageRole::Occlusion)))
    {
        result.status = {StatusCode::InvalidArgument, "Invalid texture image or alpha cutoff"};
    }
    if (result.status.Succeeded())
    {
        if (!settings.generateMipmaps && EncodedFormat(use, settings) == PixelFormat::Undefined)
        {
            result.pixels = std::move(input);
        }
        else if (input->arrayLayers != 1)
        {
            result.pixels = std::move(input);
            result.reason = "Texture arrays retain their finalized per-layer mip payloads";
        }
        else if (input->format != PixelFormat::RGBA8Unorm && input->format != PixelFormat::RGBA8Srgb)
        {
            result.pixels = std::move(input);
            result.reason = "Texture cooking currently supports RGBA8 source images";
        }
        else if (input->width > settings.maxDimension || input->height > settings.maxDimension)
        {
            result.pixels = std::move(input);
            result.reason = "Texture dimensions exceed cooking limit; original image retained";
        }
        else
        {
            result = MakeCandidate(std::move(input), use, settings, remaining, cancellation);
        }
    }
    if (result.status.Succeeded() && !result.reason.empty() && result.ownedBytes == 0)
    {
        const uint64_t bytes = result.reason.size() + 1;
        if (bytes > remaining)
        {
            result.status = {StatusCode::ResourceLimit, "Texture diagnostic exceeds budget"};
            result.pixels.reset();
            std::string().swap(result.reason);
        }
        else
        {
            remaining         -= bytes;
            result.ownedBytes  = bytes;
        }
    }
    return result;
}
} // namespace zenasset::detail
