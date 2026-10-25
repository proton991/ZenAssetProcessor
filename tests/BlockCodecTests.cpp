#include "Codecs/BlockDecoding.h"
#include "Codecs/BlockEncoding.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <thread>

namespace
{
using namespace zenasset;
using namespace zenasset::detail;

struct EncodeJob
{
    std::array<uint8_t, 64>   input{};
    std::array<std::byte, 16> output{};
    Status                    status;
};

void EncodeJobBlock(EncodeJob* job)
{
    job->status = EncodeBlock(PixelFormat::BC7Unorm, job->input, job->output);
}

bool CheckConcurrentInitialization()
{
    std::array<EncodeJob, 4>   jobs{};
    std::array<std::thread, 4> threads;
    for (size_t job = 0; job < jobs.size(); ++job)
    {
        for (size_t index = 0; index < 16; ++index)
        {
            jobs[job].input[index * 4]     = static_cast<uint8_t>(index * 16);
            jobs[job].input[index * 4 + 1] = 128;
            jobs[job].input[index * 4 + 2] = 200;
            jobs[job].input[index * 4 + 3] = static_cast<uint8_t>(255 - index * 8);
        }
        threads[job] = std::thread(EncodeJobBlock, &jobs[job]);
    }
    bool valid = true;
    for (size_t job = 0; job < jobs.size(); ++job)
    {
        threads[job].join();
        valid = valid && jobs[job].status.Succeeded() && jobs[job].output == jobs[0].output;
    }
    return valid;
}

bool CheckLdr()
{
    bool                             valid = true;
    const std::array<PixelFormat, 4> formats{PixelFormat::BC7Unorm, PixelFormat::BC7Srgb, PixelFormat::BC4Unorm,
                                             PixelFormat::BC5Unorm};
    for (PixelFormat format : formats)
    {
        std::array<uint8_t, 64> pixels{};
        for (size_t index = 0; index < 16; ++index)
        {
            pixels[index * 4]     = 64;
            pixels[index * 4 + 1] = 192;
            pixels[index * 4 + 2] = 32;
            pixels[index * 4 + 3] = 128;
        }
        std::array<std::byte, 16> block{};
        const size_t              size    = format == PixelFormat::BC4Unorm ? 8 : 16;
        const Status              encoded = EncodeBlock(format, pixels, {block.data(), size});
        std::array<Vec4, 16>      decoded{};
        const Status              read = DecodeBlock(format, {block.data(), size}, decoded);
        valid                          = valid && encoded.Succeeded() && read.Succeeded();
        for (const Vec4& pixel : decoded)
        {
            valid = valid && std::abs(pixel.x - 64.0f / 255.0f) <= 2.0f / 255.0f;
            if (format != PixelFormat::BC4Unorm)
            {
                valid = valid && std::abs(pixel.y - 192.0f / 255.0f) <= 2.0f / 255.0f;
            }
            if (format == PixelFormat::BC7Unorm || format == PixelFormat::BC7Srgb)
            {
                valid = valid && std::abs(pixel.z - 32.0f / 255.0f) <= 2.0f / 255.0f
                     && std::abs(pixel.w - 128.0f / 255.0f) <= 2.0f / 255.0f;
            }
        }
    }
    return valid;
}

bool CheckNarrowBC45Ramps()
{
    bool valid = true;
    // Every byte-range position, both edges, and independent X/Y directions.
    // These inputs have exact BC4 palettes; an 8-bit scoring tie can instead
    // select endpoints whose fractional decoded values miss by almost one byte.
    for (uint32_t base = 0; valid && base <= 252; ++base)
    {
        std::array<uint8_t, 64> pixels{};
        for (uint32_t pixel = 0; pixel < 16; ++pixel)
        {
            pixels[pixel * 4]     = static_cast<uint8_t>(base + pixel % 4);
            pixels[pixel * 4 + 1] = static_cast<uint8_t>(base + pixel / 4);
            pixels[pixel * 4 + 2] = 255;
            pixels[pixel * 4 + 3] = static_cast<uint8_t>(pixel * 17);
        }
        for (PixelFormat format : {PixelFormat::BC4Unorm, PixelFormat::BC5Unorm})
        {
            std::array<std::byte, 16> block{};
            std::array<Vec4, 16>      decoded{};
            const size_t              size = format == PixelFormat::BC4Unorm ? 8 : 16;
            valid                          = valid && EncodeBlock(format, pixels, {block.data(), size}).Succeeded()
                                          && DecodeBlock(format, {block.data(), size}, decoded).Succeeded();
            for (uint32_t pixel = 0; pixel < 16; ++pixel)
            {
                valid = valid && std::abs(decoded[pixel].x - pixels[pixel * 4] / 255.0f) < 0.0000001f;
                if (format == PixelFormat::BC5Unorm)
                {
                    valid = valid && std::abs(decoded[pixel].y - pixels[pixel * 4 + 1] / 255.0f) < 0.0000001f;
                }
            }
        }
    }
    return valid;
}

bool CheckHdr()
{
    bool valid = true;
    for (PixelFormat format : {PixelFormat::BC6HUfloat, PixelFormat::BC6HSfloat})
    {
        std::array<Vec4, 16> pixels{};
        const float          red = format == PixelFormat::BC6HSfloat ? -4.0f : 4.0f;
        pixels.fill({red, 1.0f, 16.0f, 1.0f});
        std::array<std::byte, 16> block{};
        const Status              encoded = EncodeHdrBlock(format, pixels, block);
        std::array<Vec4, 16>      decoded{};
        const Status              read = DecodeBlock(format, block, decoded);
        valid                          = valid && encoded.Succeeded() && read.Succeeded();
        for (const Vec4& pixel : decoded)
        {
            valid = valid && std::abs(pixel.x - red) < 0.08f && std::abs(pixel.y - 1.0f) < 0.02f
                 && std::abs(pixel.z - 16.0f) < 0.32f && pixel.w == 1.0f;
        }
        pixels[0].x = std::numeric_limits<float>::infinity();
        valid       = valid && EncodeHdrBlock(format, pixels, block).code == StatusCode::InvalidArgument;
    }
    return valid;
}

// The unpatched scalar BC6H endpoint search included uninitialized palette
// padding in its RGB score. Reuse differently filled caller-stack memory so
// repeated calls exercise that defect without reading expired storage here.
#if defined(_MSC_VER)
#    define ZENASSET_TEST_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#    define ZENASSET_TEST_NOINLINE __attribute__((noinline))
#else
#    define ZENASSET_TEST_NOINLINE
#endif
ZENASSET_TEST_NOINLINE void FillHdrCallStack(uint32_t pattern)
{
    volatile uint32_t scratch[16384];
    for (size_t index = 0; index < 16384; ++index)
    {
        scratch[index] = pattern;
    }
}

template <size_t Padding> ZENASSET_TEST_NOINLINE Status EncodeHdrWithStack(PixelFormat                 format,
                                                                           const std::array<Vec4, 16>& pixels,
                                                                           uint32_t                    pattern,
                                                                           std::array<std::byte, 16>&  block)
{
    volatile uint32_t padding[Padding];
    for (size_t index = 0; index < Padding; ++index)
    {
        padding[index] = pattern;
    }
    FillHdrCallStack(pattern);
    Status status = EncodeHdrBlock(format, pixels, block);
    if (padding[Padding - 1] != pattern)
    {
        status = {StatusCode::InvalidData, "HDR test caller storage changed"};
    }
    return status;
}
#undef ZENASSET_TEST_NOINLINE

bool HdrQuality(PixelFormat format, const std::array<Vec4, 16>& source, const std::array<std::byte, 16>& block)
{
    std::array<Vec4, 16> decoded{};
    bool                 valid        = DecodeBlock(format, block, decoded).Succeeded();
    double               squaredError = 0.0;
    double               energy       = 0.0;
    for (size_t pixel = 0; pixel < source.size(); ++pixel)
    {
        valid &= decoded[pixel].w == 1.0f;
        for (size_t channel = 0; channel < 3; ++channel)
        {
            valid                   &= std::isfinite(decoded[pixel][channel]);
            const double difference  = double(decoded[pixel][channel]) - source[pixel][channel];
            squaredError            += difference * difference;
            energy                  += double(source[pixel][channel]) * source[pixel][channel];
        }
    }
    valid &= squaredError <= energy * 0.02 * 0.02;
    return valid;
}

bool CheckHdrStackDeterminism()
{
    bool                              valid        = true;
    uint32_t                          differences  = 0;
    uint32_t                          encodedCount = 0;
    constexpr std::array<uint32_t, 4> patterns{0u, 0x01010101u, 0x7f7f7f7fu, 0xffffffffu};
    for (PixelFormat format : {PixelFormat::BC6HUfloat, PixelFormat::BC6HSfloat})
    {
        for (uint32_t fixture = 0; fixture < 7; ++fixture)
        {
            std::array<Vec4, 16> pixels{};
            for (size_t pixel = 0; pixel < pixels.size(); ++pixel)
            {
                const float red   = fixture < 6 ? 4.0f + float(fixture) : 4.0f + float(pixel % 4) * 0.125f;
                const float green = fixture < 6 ? 2.0f : 2.0f + float(pixel / 4) * 0.0625f;
                const float blue  = fixture < 6 ? 16.0f : 16.0f + float(pixel % 4) * 0.25f;
                pixels[pixel]     = Vec4(format == PixelFormat::BC6HSfloat ? -red : red, green, blue, 1.0f);
            }
            std::array<std::byte, 16> reference{};
            valid &= EncodeHdrWithStack<1>(format, pixels, 0, reference).Succeeded() && HdrQuality(format, pixels, reference);
            for (uint32_t repeat = 0; repeat < 2; ++repeat)
            {
                for (uint32_t pattern : patterns)
                {
                    std::array<std::byte, 16> first{}, second{};
                    const Status              firstStatus  = EncodeHdrWithStack<257>(format, pixels, pattern, first);
                    const Status              secondStatus = EncodeHdrWithStack<1021>(format, pixels, pattern, second);
                    valid        &= firstStatus.Succeeded() && secondStatus.Succeeded() && HdrQuality(format, pixels, first)
                                 && HdrQuality(format, pixels, second);
                    differences  += first != reference ? 1u : 0u;
                    differences  += second != reference ? 1u : 0u;
                    encodedCount += 2;
                }
            }
        }
    }
    std::printf("BC6H stack determinism: calls=%u different=%u quality=%d\n", encodedCount, differences, valid);
    return valid && differences == 0;
}

bool CheckInvalid()
{
    bool                      valid = true;
    std::array<Vec4, 16>      pixels{};
    std::array<std::byte, 16> block{};
    std::array<uint8_t, 64>   rgba{};
    valid = valid && DecodeBlock(PixelFormat::BC7Unorm, block, pixels).code == StatusCode::InvalidData;
    valid = valid && DecodeBlock(PixelFormat::BC7Unorm, {block.data(), 15}, pixels).code == StatusCode::InvalidArgument;
    valid = valid && EncodeBlock(PixelFormat::BC7Unorm, rgba, block, 5).code == StatusCode::InvalidArgument;
    valid = valid && EncodeBlock(PixelFormat::RGBA8Unorm, rgba, block).code == StatusCode::UnsupportedFeature;
    for (uint8_t mode : {19, 23, 27, 31})
    {
        block[0] = static_cast<std::byte>(mode);
        valid    = valid && DecodeBlock(PixelFormat::BC6HUfloat, block, pixels).code == StatusCode::InvalidData;
    }
    return valid;
}
} // namespace

int main()
{
    const bool concurrency = CheckConcurrentInitialization();
    const bool ldr         = CheckLdr();
    const bool narrowRamps = CheckNarrowBC45Ramps();
    const bool hdr         = CheckHdr();
    const bool hdrStack    = CheckHdrStackDeterminism();
    const bool invalid     = CheckInvalid();
    std::printf("Block codecs: concurrent=%d LDR=%d narrowBC45=%d HDR=%d invalid=%d\n", concurrency, ldr, narrowRamps, hdr,
                invalid);
    return concurrency && ldr && narrowRamps && hdr && hdrStack && invalid ? 0 : 1;
}
