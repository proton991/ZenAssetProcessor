#include "Codecs/BlockEncoding.h"
#include "Codecs/Bc6hEncoder.h"
#include <bc7enc.h>
#include <rgbcx.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <limits>

namespace zenasset::detail
{
namespace
{
std::once_flag g_blockEncoderInitialization;

void InitializeBlockEncoders()
{
    bc7enc_compress_block_init();
    rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal);
}

uint64_t ScoreBC4Endpoints(const uint8_t* pixels, uint8_t first, uint8_t second, std::array<uint8_t, 8>& block)
{
    // The decoder retains fractional palette values. Scaling byte units by 35
    // represents both 1/5 and 1/7 interpolation exactly, without 8-bit rounding.
    std::array<int32_t, 8> palette{int32_t(first) * 35, int32_t(second) * 35};
    const int32_t          steps = first > second ? 7 : 5;
    for (int32_t index = 1; index < steps; ++index)
    {
        palette[index + 1] = ((steps - index) * first + index * second) * (35 / steps);
    }
    if (steps == 5)
    {
        palette[6] = 0;
        palette[7] = 255 * 35;
    }
    uint64_t error     = 0;
    uint64_t selectors = 0;
    for (uint32_t pixel = 0; pixel < 16; ++pixel)
    {
        uint32_t bestError = UINT32_MAX;
        uint32_t selector  = 0;
        for (uint32_t candidate = 0; candidate < palette.size(); ++candidate)
        {
            const int32_t  difference     = int32_t(pixels[pixel * 4]) * 35 - palette[candidate];
            const uint32_t candidateError = static_cast<uint32_t>(difference * difference);
            if (candidateError < bestError)
            {
                bestError = candidateError;
                selector  = candidate;
            }
        }
        error     += bestError;
        selectors |= uint64_t(selector) << (pixel * 3);
    }
    block[0] = first;
    block[1] = second;
    for (uint32_t index = 0; index < 6; ++index)
    {
        block[index + 2] = static_cast<uint8_t>(selectors >> (index * 8));
    }
    return error;
}

void RefineBC4(const uint8_t* pixels, std::byte* output)
{
    std::array<uint8_t, 8> best{};
    uint64_t               bestError =
        ScoreBC4Endpoints(pixels, std::to_integer<uint8_t>(output[0]), std::to_integer<uint8_t>(output[1]), best);
    int32_t minimum = 255;
    int32_t maximum = 0;
    for (uint32_t pixel = 0; pixel < 16; ++pixel)
    {
        minimum = std::min(minimum, int32_t(pixels[pixel * 4]));
        maximum = std::max(maximum, int32_t(pixels[pixel * 4]));
    }
    // Retain upstream's bounded endpoint neighborhood and both interpolation modes.
    // Its chosen endpoints are also scored, so the fractional SSE cannot increase.
    const int32_t radius = static_cast<int32_t>(rgbcx::BC4_DEFAULT_SEARCH_RAD);
    for (uint32_t mode = 0; mode < 2 && bestError != 0; ++mode)
    {
        for (int32_t low = -radius; low <= radius && bestError != 0; ++low)
        {
            for (int32_t high = -radius; high <= radius && bestError != 0; ++high)
            {
                uint8_t first  = static_cast<uint8_t>(std::clamp(maximum + high, 0, 255));
                uint8_t second = static_cast<uint8_t>(std::clamp(minimum + low, 0, 255));
                if ((mode == 0 && first < second) || (mode == 1 && first > second))
                {
                    std::swap(first, second);
                }
                std::array<uint8_t, 8> candidate{};
                const uint64_t         error = ScoreBC4Endpoints(pixels, first, second, candidate);
                if (error < bestError)
                {
                    bestError = error;
                    best      = candidate;
                }
            }
        }
    }
    std::memcpy(output, best.data(), best.size());
}
using NormalDirections = std::array<std::array<double, 3>, 16>;

uint32_t ReadBits(const std::array<std::byte, 16>& block, uint32_t& offset, uint32_t count)
{
    uint32_t value = 0;
    for (uint32_t bit = 0; bit < count; ++bit)
    {
        value |= ((std::to_integer<uint32_t>(block[offset / 8]) >> (offset % 8)) & 1u) << bit;
        ++offset;
    }
    return value;
}

void WriteBits(std::array<std::byte, 16>& block, uint32_t& offset, uint32_t count, uint32_t value)
{
    const uint32_t byte   = offset / 8;
    const uint32_t shift  = offset % 8;
    value                &= (1u << count) - 1u;
    block[byte]          |= std::byte((value << shift) & 255u);
    if (shift + count > 8)
    {
        block[byte + 1] |= std::byte(value >> (8 - shift));
    }
    offset += count;
}

using Endpoints = std::array<std::array<uint32_t, 4>, 2>;

double ScoreMode6(const NormalDirections&   source,
                  const Endpoints&          endpoints,
                  std::array<uint32_t, 16>& selectors,
                  double                    cutoff = std::numeric_limits<double>::infinity())
{
    constexpr std::array<uint32_t, 16> weights{0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
    // Contiguous components let the independent distance calculations run in
    // parallel. The selector reduction and pixel sum retain their original order.
    std::array<std::array<double, 16>, 3> palette{};
    for (uint32_t selector = 0; selector < 16; ++selector)
    {
        double length = 0.0;
        for (uint32_t channel = 0; channel < 3; ++channel)
        {
            const uint32_t value =
                ((64 - weights[selector]) * endpoints[0][channel] + weights[selector] * endpoints[1][channel] + 32) >> 6;
            palette[channel][selector]  = double(value) * 2.0 / 255.0 - 1.0;
            length                     += palette[channel][selector] * palette[channel][selector];
        }
        for (uint32_t channel = 0; channel < 3; ++channel)
        {
            palette[channel][selector] /= std::sqrt(length);
        }
    }
    double total = 0.0;
    // A partial sum cannot decrease. Rejected trials need neither the remaining
    // selectors nor a packed block; successful trials still evaluate every texel.
    for (uint32_t pixel = 0; total < cutoff && pixel < 16; ++pixel)
    {
        const std::array<double, 3>& normal = source[pixel];
        double                       best   = std::numeric_limits<double>::infinity();
        std::array<double, 16>       errors{};
        for (uint32_t selector = 0; selector < 16; ++selector)
        {
            const double dx  = normal[0] - palette[0][selector];
            const double dy  = normal[1] - palette[1][selector];
            const double dz  = normal[2] - palette[2][selector];
            errors[selector] = dx * dx + dy * dy + dz * dz;
        }
        for (uint32_t selector = 0; selector < 16; ++selector)
        {
            const double error = errors[selector];
            if (error < best)
            {
                best             = error;
                selectors[pixel] = selector;
            }
        }
        total += best;
    }
    return total;
}

double CachedScoreMode6(const NormalDirections&   source,
                        const Endpoints&          endpoints,
                        std::array<uint32_t, 16>& selectors,
                        BC7NormalSearchScratch&   scratch,
                        double                    cutoff = std::numeric_limits<double>::infinity())
{
    // Only RGB affects angular scores. The upper bit distinguishes a populated
    // entry, including all-zero RGB endpoints, from a freshly reset cache.
    uint64_t key = uint64_t(1) << 48;
    for (uint32_t end = 0; end < 2; ++end)
    {
        for (uint32_t channel = 0; channel < 3; ++channel)
        {
            key |= uint64_t(endpoints[end][channel]) << ((end * 3 + channel) * 8);
        }
    }
    BC7NormalSearchScratch::Score& entry = scratch.scores[(key * 0x9e3779b97f4a7c15ull) >> 57];
    // A rejected partial sum is only a lower bound. Reuse it to reject a trial
    // with an equal or lower cutoff; a larger cutoff requires scoring again.
    if (entry.key != key || (!entry.complete && entry.score < cutoff))
    {
        entry.key       = key;
        entry.score     = ScoreMode6(source, endpoints, selectors, cutoff);
        entry.complete  = entry.score < cutoff;
        entry.selectors = 0;
        if (entry.complete)
        {
            for (uint32_t pixel = 0; pixel < 16; ++pixel)
            {
                entry.selectors |= uint64_t(selectors[pixel]) << (pixel * 4);
            }
        }
    }
    else if (entry.complete && entry.score < cutoff)
    {
        for (uint32_t pixel = 0; pixel < 16; ++pixel)
        {
            selectors[pixel] = uint32_t((entry.selectors >> (pixel * 4)) & 15u);
        }
    }
    return entry.score;
}

void PackMode6(Endpoints endpoints, std::array<uint32_t, 16> selectors, std::array<std::byte, 16>& block)
{
    if (selectors[0] >= 8)
    {
        std::swap(endpoints[0], endpoints[1]);
        for (uint32_t& selector : selectors)
        {
            selector = 15 - selector;
        }
    }
    block.fill(std::byte{0});
    uint32_t offset = 0;
    WriteBits(block, offset, 7, 64);
    for (uint32_t channel = 0; channel < 4; ++channel)
    {
        for (uint32_t end = 0; end < 2; ++end)
        {
            WriteBits(block, offset, 7, endpoints[end][channel] >> 1);
        }
    }
    WriteBits(block, offset, 1, endpoints[0][0] & 1u);
    WriteBits(block, offset, 1, endpoints[1][0] & 1u);
    for (uint32_t pixel = 0; pixel < 16; ++pixel)
    {
        WriteBits(block, offset, pixel == 0 ? 3 : 4, selectors[pixel]);
    }
}

Endpoints ReadMode6Endpoints(const std::array<std::byte, 16>& block)
{
    uint32_t  offset = 7;
    Endpoints endpoints{};
    for (uint32_t channel = 0; channel < 4; ++channel)
    {
        for (uint32_t end = 0; end < 2; ++end)
        {
            endpoints[end][channel] = ReadBits(block, offset, 7) << 1;
        }
    }
    for (uint32_t end = 0; end < 2; ++end)
    {
        const uint32_t pbit = ReadBits(block, offset, 1);
        for (uint32_t channel = 0; channel < 4; ++channel)
        {
            endpoints[end][channel] |= pbit;
        }
    }
    return endpoints;
}

Endpoints ExtendMode6Endpoints(const Endpoints& original, double extendLow, double extendHigh)
{
    Endpoints endpoints = original;
    for (uint32_t channel = 0; channel < 3; ++channel)
    {
        const double difference = double(original[1][channel]) - double(original[0][channel]);
        for (uint32_t end = 0; end < 2; ++end)
        {
            const double  extended  = double(original[end][channel]) + difference * (end == 0 ? -extendLow : extendHigh);
            const int32_t quantized = static_cast<int32_t>(std::round((extended - (original[end][channel] & 1u)) / 2.0)) * 2;
            endpoints[end][channel] = static_cast<uint32_t>(std::clamp(quantized, 0, 254)) | (original[end][channel] & 1u);
        }
    }
    return endpoints;
}

Status OptimizeMode6(const NormalDirections&    source,
                     Endpoints                  endpoints,
                     BC7NormalSearchScratch&    scratch,
                     std::array<std::byte, 16>& block,
                     const Cancellation*        cancellation)
{
    Status                   status;
    std::array<uint32_t, 16> selectors{};
    double                   best = CachedScoreMode6(source, endpoints, selectors, scratch);
    PackMode6(endpoints, selectors, block);
    bool changed = true;
    for (uint32_t pass = 0; status.Succeeded() && changed && best > 0 && pass < 8; ++pass)
    {
        changed = false;
        if (cancellation && cancellation->IsCancelled())
        {
            status = {StatusCode::Cancelled, "BC7 normal search cancelled"};
        }
        for (uint32_t component = 0; status.Succeeded() && component < 8; ++component)
        {
            const uint32_t end     = component / 4;
            const uint32_t channel = component % 4;
            // Both directions toggle the same shared p-bit. A second toggle
            // either repeats a rejected trial or returns to a worse endpoint.
            for (int32_t direction = -1; direction <= (channel == 3 ? -1 : 1); direction += 2)
            {
                Endpoints candidate = endpoints;
                if (channel == 3)
                {
                    for (uint32_t value = 0; value < 4; ++value)
                    {
                        candidate[end][value] ^= 1u;
                    }
                }
                else
                {
                    const int32_t value     = int32_t(candidate[end][channel]) + direction * 2;
                    candidate[end][channel] = static_cast<uint32_t>(std::clamp(value, 0, 254)) | (candidate[end][channel] & 1u);
                }
                const double score =
                    candidate == endpoints ? best : CachedScoreMode6(source, candidate, selectors, scratch, best);
                if (score < best)
                {
                    changed   = true;
                    best      = score;
                    endpoints = candidate;
                    PackMode6(endpoints, selectors, block);
                }
            }
        }
    }
    return status;
}
} // namespace

Status EncodeBlock(PixelFormat format, const std::array<uint8_t, 64>& rgba, std::span<std::byte> output, uint32_t bc7Quality)
{
    Status     status;
    const bool supported  = format == PixelFormat::BC7Unorm || format == PixelFormat::BC7Srgb || format == PixelFormat::BC4Unorm
                         || format == PixelFormat::BC5Unorm;
    const size_t required = format == PixelFormat::BC4Unorm ? 8 : 16;
    std::fill(output.begin(), output.end(), std::byte{0});
    if (!supported)
    {
        status = {StatusCode::UnsupportedFeature, "Unsupported LDR block encoding"};
    }
    else if (output.size() != required || bc7Quality > BC7ENC_MAX_UBER_LEVEL)
    {
        status = {StatusCode::InvalidArgument, "Invalid block storage size or BC7 quality"};
    }
    else
    {
        std::call_once(g_blockEncoderInitialization, InitializeBlockEncoders);
        alignas(16) std::array<std::byte, 16> block{};
        if (format == PixelFormat::BC4Unorm)
        {
            rgbcx::encode_bc4_hq(block.data(), rgba.data(), 4);
            RefineBC4(rgba.data(), block.data());
        }
        else if (format == PixelFormat::BC5Unorm)
        {
            rgbcx::encode_bc5_hq(block.data(), rgba.data(), 0, 1, 4);
            RefineBC4(rgba.data(), block.data());
            RefineBC4(rgba.data() + 1, block.data() + 8);
        }
        else
        {
            bc7enc_compress_block_params parameters{};
            bc7enc_compress_block_params_init(&parameters);
            bc7enc_compress_block_params_init_linear_weights(&parameters);
            parameters.m_uber_level = bc7Quality;
            // Upstream's boolean reports alpha presence, not success/failure.
            (void)bc7enc_compress_block(block.data(), rgba.data(), &parameters);
        }
        std::memcpy(output.data(), block.data(), required);
    }
    return status;
}

Status EncodeHdrBlock(PixelFormat format, const std::array<Vec4, 16>& rgba, std::span<std::byte> output)
{
    Status status;
    std::fill(output.begin(), output.end(), std::byte{0});
    bool        valid        = output.size() == 16;
    const bool  signedFormat = format == PixelFormat::BC6HSfloat;
    const float minimum      = signedFormat ? -65504.0f : 0.0f;
    for (const Vec4& pixel : rgba)
    {
        valid = valid && std::isfinite(pixel.x) && std::isfinite(pixel.y) && std::isfinite(pixel.z) && pixel.x >= minimum
             && pixel.x <= 65504.0f && pixel.y >= minimum && pixel.y <= 65504.0f && pixel.z >= minimum && pixel.z <= 65504.0f
             && pixel.w == 1.0f;
    }
    if (format != PixelFormat::BC6HUfloat && !signedFormat)
    {
        status = {StatusCode::UnsupportedFeature, "Unsupported HDR block encoding"};
    }
    else if (!valid)
    {
        status = {StatusCode::InvalidArgument, "BC6H requires finite representable RGB, unit alpha and sixteen output bytes"};
    }
    else
    {
        alignas(16) std::array<std::byte, 16> block{};
        EncodeBc6hBlock(rgba, signedFormat, block);
        std::memcpy(output.data(), block.data(), block.size());
    }
    return status;
}

Status EncodeBC7NormalCandidates(const std::array<uint8_t, 64>& rgba,
                                 BC7NormalCandidates&           candidates,
                                 BC7NormalSearchScratch&        scratch,
                                 const Cancellation*            cancellation)
{
    Status status;
    candidates = {};
    if (cancellation && cancellation->IsCancelled())
    {
        status = {StatusCode::Cancelled, "BC7 normal search cancelled"};
    }
    if (status.Succeeded())
    {
        for (BC7NormalSearchScratch::Score& entry : scratch.scores)
        {
            entry.key = 0;
        }
        std::call_once(g_blockEncoderInitialization, InitializeBlockEncoders);
        bc7enc_compress_block_params parameters{};
        bc7enc_compress_block_params_init(&parameters);
        bc7enc_compress_block_params_init_linear_weights(&parameters);
        parameters.m_uber_level = 4;
        parameters.m_mode_mask  = 1u << 6;
        alignas(16) std::array<std::byte, 16> initial{};
        (void)bc7enc_compress_block(initial.data(), rgba.data(), &parameters);
        NormalDirections directions{};
        for (uint32_t pixel = 0; pixel < 16; ++pixel)
        {
            double squared = 0;
            for (uint32_t channel = 0; channel < 3; ++channel)
            {
                directions[pixel][channel]  = double(rgba[pixel * 4 + channel]) * 2.0 / 255.0 - 1.0;
                squared                    += directions[pixel][channel] * directions[pixel][channel];
            }
            // Odd signed byte coordinates cannot form the zero vector.
            const double length = std::sqrt(squared);
            for (double& component : directions[pixel])
            {
                component /= length;
            }
        }
        constexpr std::array<double, 4> extensions{0.0, 0.25, 0.5, 1.0};
        uint32_t                        index = 0;
        std::array<Endpoints, 16>       seeds{};
        const Endpoints                 original = ReadMode6Endpoints(initial);
        // Quantization can collapse different extensions onto the same seed.
        // Reuse its exact result without changing the sixteen-candidate order.
        for (uint32_t low = 0; status.Succeeded() && low < 4; ++low)
        {
            for (uint32_t high = 0; status.Succeeded() && high < 4; ++high)
            {
                seeds[index]      = ExtendMode6Endpoints(original, extensions[low], extensions[high]);
                uint32_t previous = 0;
                while (previous < index && seeds[previous] != seeds[index])
                {
                    ++previous;
                }
                if (previous < index)
                {
                    candidates[index] = candidates[previous];
                }
                else
                {
                    status = OptimizeMode6(directions, seeds[index], scratch, candidates[index], cancellation);
                }
                ++index;
            }
        }
    }
    if (!status.Succeeded())
    {
        candidates = {};
    }
    return status;
}
} // namespace zenasset::detail
