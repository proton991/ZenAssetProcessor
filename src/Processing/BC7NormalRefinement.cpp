#include "Processing/BC7NormalRefinement.h"
#include "Codecs/BlockDecoding.h"
#include "Codecs/BlockEncoding.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

namespace zenasset::detail
{
namespace
{
using NormalValue  = std::array<double, 3>;
using NormalPixels = std::array<NormalValue, 16>;

struct FilterCorner
{
    NormalValue source{};
    NormalValue decoded{};
    int32_t     target{-1};
};

struct FilterCell
{
    std::array<FilterCorner, 4> corners{};
    std::array<NormalValue, 16> reference{};
};

struct FilterContext
{
    std::array<bool, 16>       validPixels{};
    std::array<FilterCell, 25> cells{};
    std::array<uint8_t, 16>    alpha{};
    uint32_t                   count{0};
};

using FilterKey = std::array<int16_t, 749>;

struct FilterMetrics
{
    std::array<double, 16> phases{};
    double                 sum{0.0};
    double                 maximum{0.0};
    uint32_t               alphaError{0};
};

NormalValue Bilinear(const std::array<NormalValue, 4>& corners, uint32_t phase)
{
    const double fx = double(phase % 4) / 4.0;
    const double fy = double(phase / 4) / 4.0;
    NormalValue  result{};
    for (uint32_t channel = 0; channel < 3; ++channel)
    {
        const double top    = corners[0][channel] * (1.0 - fx) + corners[1][channel] * fx;
        const double bottom = corners[2][channel] * (1.0 - fx) + corners[3][channel] * fx;
        result[channel]     = top * (1.0 - fy) + bottom * fy;
    }
    return result;
}

NormalValue Unit(NormalValue value)
{
    const double squared = value[0] * value[0] + value[1] * value[1] + value[2] * value[2];
    if (squared > 0.0)
    {
        const double length = std::sqrt(squared);
        for (double& component : value)
        {
            component /= length;
        }
    }
    return value;
}

NormalValue ReadValue(const uint8_t* pixel)
{
    return {double(pixel[0]) * 2.0 - 255.0, double(pixel[1]) * 2.0 - 255.0, double(pixel[2]) * 2.0 - 255.0};
}

Status DecodeValues(std::span<const std::byte> block, NormalPixels& values, std::array<uint8_t, 16>* alpha = nullptr)
{
    std::array<Vec4, 16> decoded{};
    const Status         status = detail::DecodeBlock(PixelFormat::BC7Unorm, block, decoded);
    if (status.Succeeded())
    {
        for (uint32_t pixel = 0; pixel < 16; ++pixel)
        {
            if (alpha != nullptr)
            {
                (*alpha)[pixel] = static_cast<uint8_t>(std::round(double(decoded[pixel].w) * 255.0));
            }
            values[pixel] = {std::round(double(decoded[pixel].x) * 255.0) * 2.0 - 255.0,
                             std::round(double(decoded[pixel].y) * 255.0) * 2.0 - 255.0,
                             std::round(double(decoded[pixel].z) * 255.0) * 2.0 - 255.0};
        }
    }
    return status;
}

FilterMetrics MeasureFiltered(const FilterContext& context, const NormalPixels& values, const FilterMetrics* bounds)
{
    FilterMetrics result;
    bool          rejected = false;
    for (uint32_t cell = 0; !rejected && cell < context.count; ++cell)
    {
        std::array<NormalValue, 4> decoded{};
        for (uint32_t corner = 0; corner < 4; ++corner)
        {
            const FilterCorner& point = context.cells[cell].corners[corner];
            decoded[corner]           = point.target < 0 ? point.decoded : values[point.target];
        }
        for (uint32_t phase = 0; !rejected && phase < 16; ++phase)
        {
            const NormalValue  value      = Unit(Bilinear(decoded, phase));
            const NormalValue& reference  = context.cells[cell].reference[phase];
            const double       dot        = value[0] * reference[0] + value[1] * reference[1] + value[2] * reference[2];
            const double       angle      = value == NormalValue{} || reference == NormalValue{} ? 3.14159265358979323846
                                          : value == reference ? 0.0
                                                               : std::acos(std::clamp(dot, -1.0, 1.0));
            const double       error      = angle * angle;
            result.sum                   += error;
            result.phases[phase]         += error;
            result.maximum                = std::max(result.maximum, error);
            // Every remaining term is nonnegative. A partial sum that exceeds
            // a guard cannot recover, so expensive acos work can stop here.
            rejected =
                bounds && (result.maximum > bounds->maximum + 1e-12 || result.phases[phase] > bounds->phases[phase] + 1e-12);
        }
    }
    if (rejected)
    {
        result.sum = std::numeric_limits<double>::infinity();
    }
    return result;
}

FilterMetrics MeasureEncoded(const FilterContext&             context,
                             const std::array<std::byte, 16>& block,
                             const FilterMetrics*             bounds = nullptr)
{
    NormalPixels            values{};
    std::array<uint8_t, 16> alpha{};
    const Status            status = DecodeValues(block, values, &alpha);
    FilterMetrics           result;
    if (status.Succeeded())
    {
        uint32_t alphaError = 0;
        for (uint32_t pixel = 0; pixel < 16; ++pixel)
        {
            if (context.validPixels[pixel])
            {
                alphaError = std::max(alphaError,
                                      static_cast<uint32_t>(std::abs(int32_t(alpha[pixel]) - int32_t(context.alpha[pixel]))));
            }
        }
        if (!bounds || alphaError <= bounds->alphaError)
        {
            result = MeasureFiltered(context, values, bounds);
        }
        else
        {
            result.sum = std::numeric_limits<double>::infinity();
        }
        result.alphaError = alphaError;
    }
    else
    {
        result.sum = result.maximum = std::numeric_limits<double>::infinity();
        result.phases.fill(result.sum);
        result.alphaError = UINT32_MAX;
    }
    return result;
}

bool Dominates(const FilterMetrics& candidate, const FilterMetrics& before)
{
    bool accepted = candidate.alphaError <= before.alphaError && candidate.maximum <= before.maximum + 1e-12;
    for (uint32_t phase = 0; accepted && phase < 16; ++phase)
    {
        accepted = candidate.phases[phase] <= before.phases[phase] + 1e-12;
    }
    return accepted;
}

struct NeighborBlock
{
    size_t       index{SIZE_MAX};
    NormalPixels pixels{};
};

struct CachedBlock
{
    bool                      occupied{false};
    FilterKey                 key{};
    std::array<std::byte, 16> encoded{};
};

struct RefinementScratch
{
    FilterContext context;
    FilterKey     key{};
    // A 4x4 core plus its bilinear halo touches at most nine encoded blocks.
    std::array<NeighborBlock, 9> neighbors;
    // Exact-key direct mapping: collisions affect search cost, never decisions.
    std::array<CachedBlock, 64> cache;
    BC7NormalSearchScratch      search;
    BC7NormalCandidates         candidates{};
    std::array<uint8_t, 64>     pixels{};
};

uint32_t Coordinate(int64_t value, uint32_t extent, bool repeat)
{
    const uint32_t result = repeat ? static_cast<uint32_t>((value + extent) % extent)
                                   : static_cast<uint32_t>(std::clamp<int64_t>(value, 0, extent - 1));
    return result;
}

Status ReadNeighbor(uint32_t                      x,
                    uint32_t                      y,
                    uint32_t                      blocksWide,
                    std::span<const uint8_t>      encoded,
                    std::array<NeighborBlock, 9>& neighbors,
                    NormalValue&                  value)
{
    Status       status;
    const size_t block = size_t(y / 4) * blocksWide + x / 4;
    size_t       slot  = 0;
    while (slot < neighbors.size() && neighbors[slot].index != SIZE_MAX && neighbors[slot].index != block)
    {
        ++slot;
    }
    if (slot == neighbors.size())
    {
        status = {StatusCode::InvalidData, "BC7 normal neighborhood exceeds nine blocks"};
    }
    else
    {
        if (neighbors[slot].index == SIZE_MAX)
        {
            status                = DecodeValues(std::as_bytes(encoded.subspan(block * 16, 16)), neighbors[slot].pixels);
            neighbors[slot].index = block;
        }
        if (status.Succeeded())
        {
            value = neighbors[slot].pixels[(y % 4) * 4 + x % 4];
        }
    }
    return status;
}

Status MakeContext(std::span<const uint8_t> source,
                   std::span<const uint8_t> encoded,
                   uint32_t                 width,
                   uint32_t                 height,
                   uint32_t                 bx,
                   uint32_t                 by,
                   const TextureImageUse&   use,
                   RefinementScratch&       scratch)
{
    Status         status;
    FilterContext& context = scratch.context;
    FilterKey&     key     = scratch.key;
    context.count          = 0;
    key.fill(0);
    for (NeighborBlock& block : scratch.neighbors)
    {
        block.index = SIZE_MAX;
    }
    uint32_t               cursor = 1;
    std::array<size_t, 25> anchors{};
    const uint32_t         extentX    = std::min(4u, width - bx * 4);
    const uint32_t         extentY    = std::min(4u, height - by * 4);
    const uint32_t         blocksWide = static_cast<uint32_t>((uint64_t(width) + 3) / 4);
    for (int32_t y = -1; status.Succeeded() && y < int32_t(extentY); ++y)
    {
        for (int32_t x = -1; status.Succeeded() && x < int32_t(extentX); ++x)
        {
            const uint32_t ax        = Coordinate(int64_t(bx * 4) + x, width, use.normalRepeatS);
            const uint32_t ay        = Coordinate(int64_t(by * 4) + y, height, use.normalRepeatT);
            const size_t   anchor    = size_t(ay) * width + ax;
            bool           duplicate = false;
            for (uint32_t index = 0; index < context.count; ++index)
            {
                duplicate = duplicate || anchors[index] == anchor;
            }
            if (!duplicate)
            {
                anchors[context.count]          = anchor;
                FilterCell&                cell = context.cells[context.count++];
                std::array<NormalValue, 4> reference{};
                for (uint32_t corner = 0; status.Succeeded() && corner < 4; ++corner)
                {
                    const uint32_t cx    = Coordinate(int64_t(ax) + corner % 2, width, use.normalRepeatS);
                    const uint32_t cy    = Coordinate(int64_t(ay) + corner / 2, height, use.normalRepeatT);
                    const size_t   index = size_t(cy) * width + cx;
                    FilterCorner&  point = cell.corners[corner];
                    point.source         = ReadValue(source.data() + index * 4);
                    status               = ReadNeighbor(cx, cy, blocksWide, encoded, scratch.neighbors, point.decoded);
                    point.target         = cx >= bx * 4 && cx < bx * 4 + extentX && cy >= by * 4 && cy < by * 4 + extentY
                                             ? int32_t((cy - by * 4) * 4 + cx - bx * 4)
                                             : -1;
                    reference[corner]    = point.source;
                    key[cursor++]        = static_cast<int16_t>(point.target);
                    for (uint32_t channel = 0; channel < 3; ++channel)
                    {
                        key[cursor++] = static_cast<int16_t>(point.source[channel]);
                        key[cursor++] = static_cast<int16_t>(point.decoded[channel]);
                    }
                }
                for (uint32_t phase = 0; phase < 16; ++phase)
                {
                    cell.reference[phase] = Unit(Bilinear(reference, phase));
                }
            }
        }
    }
    key[0]              = static_cast<int16_t>(context.count);
    const size_t offset = (size_t(by) * blocksWide + bx) * 16;
    for (uint32_t y = 0; y < 4; ++y)
    {
        for (uint32_t x = 0; x < 4; ++x)
        {
            const size_t   pixel = size_t(std::min(by * 4 + y, height - 1)) * width + std::min(bx * 4 + x, width - 1);
            const uint32_t local = y * 4 + x;
            std::copy_n(source.data() + pixel * 4, 4, scratch.pixels.data() + local * 4);
            context.alpha[local]       = source[pixel * 4 + 3];
            context.validPixels[local] = x < extentX && y < extentY;
            key[701 + local]           = context.alpha[local];
            key[717 + local]           = context.validPixels[local] ? 1 : 0;
            key[733 + local]           = encoded[offset + local];
        }
    }
    return status;
}

uint32_t CacheIndex(const FilterKey& key)
{
    uint32_t hash = 2166136261u;
    for (int16_t value : key)
    {
        hash = (hash ^ static_cast<uint16_t>(value)) * 16777619u;
    }
    return hash % 64;
}

Status RefineBlock(RefinementScratch& scratch, std::span<uint8_t> encoded, const Cancellation* cancellation)
{
    Status                    status;
    CachedBlock&              cached = scratch.cache[CacheIndex(scratch.key)];
    std::array<std::byte, 16> bestBlock{};
    std::memcpy(bestBlock.data(), encoded.data(), 16);
    const FilterMetrics original = MeasureEncoded(scratch.context, bestBlock);
    double              best     = original.sum;
    if (cached.occupied && cached.key == scratch.key)
    {
        bestBlock = cached.encoded;
        best      = MeasureEncoded(scratch.context, bestBlock).sum;
    }
    else
    {
        status = EncodeBC7NormalCandidates(scratch.pixels, scratch.candidates, scratch.search, cancellation);
        for (size_t index = 0; status.Succeeded() && index < scratch.candidates.size(); ++index)
        {
            const std::array<std::byte, 16>& candidate = scratch.candidates[index];
            bool                             duplicate = candidate == bestBlock;
            for (size_t previous = 0; !duplicate && previous < index; ++previous)
            {
                duplicate = candidate == scratch.candidates[previous];
            }
            if (!duplicate)
            {
                if (cancellation && cancellation->IsCancelled())
                {
                    status = {StatusCode::Cancelled, "BC7 normal filtering cancelled"};
                }
                else
                {
                    const FilterMetrics metrics = MeasureEncoded(scratch.context, candidate, &original);
                    if (metrics.sum < best && Dominates(metrics, original))
                    {
                        best      = metrics.sum;
                        bestBlock = candidate;
                    }
                }
            }
        }
        if (status.Succeeded())
        {
            cached.occupied = true;
            cached.key      = scratch.key;
            cached.encoded  = bestBlock;
        }
    }
    if (status.Succeeded() && best < original.sum)
    {
        std::memcpy(encoded.data(), bestBlock.data(), 16);
    }
    return status;
}
} // namespace

Status RefineBC7NormalLevel(uint32_t                 width,
                            uint32_t                 height,
                            std::span<const uint8_t> source,
                            std::span<uint8_t>       encoded,
                            const TextureImageUse&   use,
                            uint64_t&                remaining,
                            const Cancellation*      cancellation)
{
    Status   status;
    uint64_t sourceSize  = 0;
    uint64_t encodedSize = 0;
    if (!GetImageLevelByteSize(PixelFormat::RGBA8Unorm, width, height, sourceSize)
        || !GetImageLevelByteSize(PixelFormat::BC7Unorm, width, height, encodedSize) || source.size() != sourceSize
        || encoded.size() != encodedSize || use.srgb)
    {
        status = {StatusCode::InvalidArgument, "Invalid BC7 normal refinement level"};
    }
    else if (cancellation && cancellation->IsCancelled())
    {
        status = {StatusCode::Cancelled, "BC7 normal filtering cancelled"};
    }
    else if (remaining < sizeof(RefinementScratch))
    {
        status = {StatusCode::ResourceLimit, "BC7 normal refinement scratch exceeds budget"};
    }
    else
    {
        remaining -= sizeof(RefinementScratch);
        {
            const std::unique_ptr<RefinementScratch> scratch    = std::make_unique<RefinementScratch>();
            const uint32_t                           blocksWide = static_cast<uint32_t>((uint64_t(width) + 3) / 4);
            const uint32_t                           blocksHigh = static_cast<uint32_t>((uint64_t(height) + 3) / 4);
            for (uint32_t by = 0; status.Succeeded() && by < blocksHigh; ++by)
            {
                for (uint32_t bx = 0; status.Succeeded() && bx < blocksWide; ++bx)
                {
                    if (cancellation && cancellation->IsCancelled())
                    {
                        status = {StatusCode::Cancelled, "BC7 normal filtering cancelled"};
                    }
                    else
                    {
                        status = MakeContext(source, encoded, width, height, bx, by, use, *scratch);
                        if (status.Succeeded())
                        {
                            status =
                                RefineBlock(*scratch, encoded.subspan((size_t(by) * blocksWide + bx) * 16, 16), cancellation);
                        }
                    }
                }
            }
        }
        remaining += sizeof(RefinementScratch);
    }
    return status;
}
} // namespace zenasset::detail
