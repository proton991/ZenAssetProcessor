#include "Processing/HdrCube.h"
#include "Diagnostics/StageMeasurements.h"
#include "Codecs/BlockDecoding.h"
#include "Codecs/BlockEncoding.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace zenasset::detail
{
namespace
{
struct HdrCubeScratch
{
    std::array<Vec4, 16> source;
    std::array<Vec4, 16> decoded;
};

Status Poll(const Cancellation* cancellation)
{
    Status status;
    if (cancellation != nullptr && cancellation->IsCancelled())
    {
        status = {StatusCode::Cancelled, "HDR cube processing cancelled"};
    }
    return status;
}

Status Reserve(uint64_t bytes, uint64_t& available)
{
    Status status;
    if (bytes > available)
    {
        status = {StatusCode::ResourceLimit, "HDR cube processing exceeds its allocation budget"};
    }
    else
    {
        available -= bytes;
    }
    return status;
}

Status ValidateView(const HdrCubeView& input, uint64_t& encodedBytes)
{
    Status   status;
    uint32_t maximumMips = 0;
    for (uint32_t extent = input.size; extent != 0; extent >>= 1)
    {
        ++maximumMips;
    }
    if (input.size == 0 || input.mipLevels == 0 || input.mipLevels > maximumMips
        || uint64_t(input.mipLevels) * 6 != input.mipFaces.size())
    {
        status = {StatusCode::InvalidData, "Invalid HDR cube extent, mip count or face count"};
    }
    encodedBytes = 0;
    for (size_t face = 0; status.Succeeded() && face < input.mipFaces.size(); ++face)
    {
        const uint32_t extent = std::max(1u, input.size >> (face / 6));
        const uint64_t pixels = uint64_t(extent) * extent;
        uint64_t       bytes  = 0;
        if (pixels > SIZE_MAX / sizeof(Vec4) || input.mipFaces[face].size() != pixels
            || !GetImageLevelByteSize(PixelFormat::BC6HUfloat, extent, extent, bytes) || bytes > SIZE_MAX
            || bytes > UINT64_MAX - encodedBytes)
        {
            status = {StatusCode::InvalidData, "Invalid HDR cube face footprint"};
        }
        else
        {
            encodedBytes += bytes;
        }
    }
    return status;
}

Status InspectPixels(const HdrCubeView&     input,
                     const HdrCubeSettings& settings,
                     const Cancellation*    cancellation,
                     const char*&           reason)
{
    Status status;
    if (input.size > settings.maxDimension)
    {
        reason = "HDR cube dimensions exceed cooking limit; original cube retained";
    }
    for (size_t face = 0; status.Succeeded() && face < input.mipFaces.size(); ++face)
    {
        const uint32_t extent = std::max(1u, input.size >> (face / 6));
        for (uint32_t y = 0; status.Succeeded() && y < extent; ++y)
        {
            status = Poll(cancellation);
            for (uint32_t x = 0; status.Succeeded() && x < extent; ++x)
            {
                const Vec4& pixel = input.mipFaces[face][size_t(y) * extent + x];
                if (!std::isfinite(pixel.x) || !std::isfinite(pixel.y) || !std::isfinite(pixel.z) || !std::isfinite(pixel.w))
                {
                    status = {StatusCode::InvalidData, "HDR cube contains nonfinite pixels"};
                }
                else if (reason == nullptr && pixel.w != 1.0f)
                {
                    reason = "BC6H requires unit alpha; original cube retained";
                }
                else if (reason == nullptr
                         && (pixel.x < 0 || pixel.y < 0 || pixel.z < 0 || pixel.x > 65504.0f || pixel.y > 65504.0f
                             || pixel.z > 65504.0f))
                {
                    reason = "HDR cube exceeds unsigned half-float RGB range; original cube retained";
                }
            }
        }
    }
    return status;
}

Status EncodeFace(std::span<const Vec4> input,
                  uint32_t              extent,
                  std::vector<uint8_t>& output,
                  HdrCubeScratch&       scratch,
                  HdrCubeFaceQuality&   quality,
                  const Cancellation*   cancellation)
{
    ZENASSET_MEASURE_SCOPE(HdrEncodeQualify);
    ZENASSET_MEASURE_ADD(HdrCandidates, 1);
    Status         status;
    const uint32_t blocks       = extent / 4 + (extent % 4 != 0);
    double         squaredError = 0;
    double         sourceEnergy = 0;
    quality.sourceMin           = std::numeric_limits<double>::infinity();
    for (uint32_t by = 0; status.Succeeded() && by < blocks; ++by)
    {
        for (uint32_t bx = 0; status.Succeeded() && bx < blocks; ++bx)
        {
            status = Poll(cancellation);
            if (status.Succeeded())
            {
                for (uint32_t y = 0; y < 4; ++y)
                {
                    for (uint32_t x = 0; x < 4; ++x)
                    {
                        const uint32_t py         = std::min(extent - 1, by * 4 + y);
                        const uint32_t px         = std::min(extent - 1, bx * 4 + x);
                        scratch.source[y * 4 + x] = input[size_t(py) * extent + px];
                    }
                }
                const size_t               offset = (size_t(by) * blocks + bx) * 16;
                const std::span<std::byte> block(reinterpret_cast<std::byte*>(output.data() + offset), 16);
                status = EncodeHdrBlock(PixelFormat::BC6HUfloat, scratch.source, block);
                if (status.Succeeded())
                {
                    status = DecodeBlock(PixelFormat::BC6HUfloat, block, scratch.decoded);
                }
                for (uint32_t y = 0; status.Succeeded() && y < 4 && by * 4 + y < extent; ++y)
                {
                    for (uint32_t x = 0; x < 4 && bx * 4 + x < extent; ++x)
                    {
                        const Vec4& source  = scratch.source[y * 4 + x];
                        const Vec4& decoded = scratch.decoded[y * 4 + x];
                        for (uint32_t channel = 0; channel < 3; ++channel)
                        {
                            const double reference    = source[channel];
                            const double difference   = double(decoded[channel]) - reference;
                            squaredError             += difference * difference;
                            sourceEnergy             += reference * reference;
                            quality.maxAbsoluteError  = std::max(quality.maxAbsoluteError, std::abs(difference));
                            quality.sourceMin         = std::min(quality.sourceMin, reference);
                            quality.sourceMax         = std::max(quality.sourceMax, reference);
                        }
                    }
                }
            }
        }
    }
    if (status.Succeeded())
    {
        quality.relativeRmse = sourceEnergy > 0  ? std::sqrt(squaredError / sourceEnergy)
                             : squaredError == 0 ? 0
                                                 : std::numeric_limits<double>::infinity();
    }
    return status;
}
} // namespace

HdrCubeResult ProcessHdrCube(const HdrCubeView&     input,
                             const HdrCubeSettings& settings,
                             uint64_t&              remaining,
                             const Cancellation*    cancellation)
{
    HdrCubeResult result;
    result.status = Poll(cancellation);
    if (result.status.Succeeded()
        && (settings.maxDimension == 0 || !std::isfinite(settings.maxRelativeRmse) || settings.maxRelativeRmse < 0
            || settings.maxRelativeRmse > 1))
    {
        result.status = {StatusCode::InvalidArgument, "Invalid HDR cube processing settings"};
    }
    uint64_t encodedBytes = 0;
    if (result.status.Succeeded())
    {
        result.status = ValidateView(input, encodedBytes);
    }
    // Keep the caller's allowance unchanged until the complete result commits.
    uint64_t                       available   = remaining;
    uint64_t                       pixelBytes  = 0;
    uint64_t                       metricBytes = 0;
    const char*                    reason      = nullptr;
    std::shared_ptr<HdrCubePixels> output;
    if (result.status.Succeeded() && settings.compressBC6H)
    {
        result.status = InspectPixels(input, settings, cancellation, reason);
    }
    if (result.status.Succeeded() && settings.compressBC6H && reason == nullptr)
    {
        const uint64_t records =
            sizeof(HdrCubePixels) + 2 * sizeof(void*) + input.mipFaces.size() * sizeof(std::vector<uint8_t>);
        metricBytes = input.mipFaces.size() * sizeof(HdrCubeFaceQuality);
        if (encodedBytes > UINT64_MAX - records)
        {
            result.status = {StatusCode::ResourceLimit, "HDR cube output exceeds addressable storage"};
        }
        else
        {
            pixelBytes    = records + encodedBytes;
            result.status = Reserve(pixelBytes, available);
        }
        if (result.status.Succeeded())
        {
            result.status = Reserve(metricBytes, available);
        }
        if (result.status.Succeeded())
        {
            result.status = Reserve(sizeof(HdrCubeScratch), available);
        }
        if (result.status.Succeeded())
        {
            output            = std::make_shared<HdrCubePixels>();
            output->format    = PixelFormat::BC6HUfloat;
            output->size      = input.size;
            output->mipLevels = input.mipLevels;
            output->blockMipFaces.resize(input.mipFaces.size());
            result.metrics.reserve(input.mipFaces.size());
            HdrCubeScratch scratch;
            for (size_t face = 0; result.status.Succeeded() && reason == nullptr && face < input.mipFaces.size(); ++face)
            {
                const uint32_t extent = std::max(1u, input.size >> (face / 6));
                uint64_t       bytes  = 0;
                GetImageLevelByteSize(PixelFormat::BC6HUfloat, extent, extent, bytes);
                output->blockMipFaces[face].resize(static_cast<size_t>(bytes));
                HdrCubeFaceQuality quality;
                quality.mipLevel = static_cast<uint32_t>(face / 6);
                quality.face     = static_cast<uint32_t>(face % 6);
                quality.size     = extent;
                result.status =
                    EncodeFace(input.mipFaces[face], extent, output->blockMipFaces[face], scratch, quality, cancellation);
                if (result.status.Succeeded())
                {
                    result.metrics.push_back(quality);
                    if (quality.relativeRmse > settings.maxRelativeRmse)
                    {
                        reason = "BC6H face/mip relative RMSE exceeds quality limit; original cube retained";
                    }
                }
            }
            available += sizeof(HdrCubeScratch);
        }
    }
    if (result.status.Succeeded() && reason != nullptr)
    {
        output.reset();
        available                  += pixelBytes;
        pixelBytes                  = 0;
        const uint64_t reasonBytes  = std::strlen(reason) + 1;
        result.status               = Reserve(reasonBytes, available);
        if (result.status.Succeeded())
        {
            result.reason     = reason;
            result.ownedBytes = metricBytes + reasonBytes;
        }
    }
    else if (result.status.Succeeded())
    {
        result.pixels     = std::move(output);
        result.pixelBytes = pixelBytes;
        result.ownedBytes = pixelBytes + metricBytes;
    }
    if (result.status.Succeeded())
    {
        result.status = Poll(cancellation);
    }
    if (result.status.Succeeded())
    {
        remaining -= result.ownedBytes;
    }
    else
    {
        result.pixels.reset();
        std::vector<HdrCubeFaceQuality>().swap(result.metrics);
        std::string().swap(result.reason);
        result.ownedBytes = 0;
        result.pixelBytes = 0;
    }
    return result;
}
} // namespace zenasset::detail
