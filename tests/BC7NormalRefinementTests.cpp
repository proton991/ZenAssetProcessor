#include "Processing/BC7NormalRefinement.h"
#include "Codecs/BlockEncoding.h"
#include "Codecs/BlockDecoding.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace
{
using namespace zenasset;
using namespace zenasset::detail;

bool Check(bool value, const char* message)
{
    if (!value)
    {
        std::printf("FAIL: %s\n", message);
    }
    return value;
}

std::shared_ptr<ImagePixels> Fixture(uint32_t width, uint32_t height)
{
    std::shared_ptr<ImagePixels> image = std::make_shared<ImagePixels>();
    image->width                       = width;
    image->height                      = height;
    image->bytes.resize(size_t(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            const double                nx = 0.32 * std::sin((x + 0.3 * y) * 0.13) + 0.14 * std::cos((0.4 * x - y) * 0.19);
            const double                ny = 0.30 * std::cos((0.7 * x + y) * 0.11);
            const std::array<double, 3> normal{nx, ny, std::sqrt(1.0 - nx * nx - ny * ny)};
            const size_t                pixel = size_t(y) * width + x;
            for (size_t channel = 0; channel < 3; ++channel)
            {
                image->bytes[pixel * 4 + channel] = static_cast<uint8_t>(std::round((normal[channel] + 1.0) * 127.5));
            }
            image->bytes[pixel * 4 + 3] = 255;
        }
    }
    return image;
}

bool Baseline(const ImagePixels& image, uint32_t quality, std::vector<uint8_t>& encoded)
{
    const uint32_t wide = (image.width + 3) / 4;
    const uint32_t high = (image.height + 3) / 4;
    encoded.resize(size_t(wide) * high * 16);
    bool valid = true;
    for (uint32_t by = 0; by < high; ++by)
    {
        for (uint32_t bx = 0; bx < wide; ++bx)
        {
            std::array<uint8_t, 64> source{};
            for (uint32_t y = 0; y < 4; ++y)
            {
                for (uint32_t x = 0; x < 4; ++x)
                {
                    const size_t pixel =
                        size_t(std::min(by * 4 + y, image.height - 1)) * image.width + std::min(bx * 4 + x, image.width - 1);
                    std::copy_n(image.bytes.data() + pixel * 4, 4, source.data() + (y * 4 + x) * 4);
                }
            }
            valid = EncodeBlock(PixelFormat::BC7Unorm, source,
                                std::as_writable_bytes(std::span(encoded)).subspan((size_t(by) * wide + bx) * 16, 16), quality)
                        .Succeeded()
                 && valid;
        }
    }
    return valid;
}

struct Measured
{
    std::array<double, 16> phases{};
    double                 maximum{0};
    uint32_t               alpha{0};
    bool                   valid{true};
};

Measured Measure(const ImagePixels& source, std::span<const uint8_t> encoded, const TextureImageUse& use)
{
    Measured                           result;
    std::vector<std::array<double, 3>> decoded(size_t(source.width) * source.height);
    for (uint32_t by = 0; by < (source.height + 3) / 4; ++by)
    {
        for (uint32_t bx = 0; bx < (source.width + 3) / 4; ++bx)
        {
            std::array<Vec4, 16> block{};
            const size_t         offset = (size_t(by) * ((source.width + 3) / 4) + bx) * 16;
            result.valid = DecodeBlock(PixelFormat::BC7Unorm, std::as_bytes(encoded.subspan(offset, 16)), block).Succeeded()
                        && result.valid;
            for (uint32_t y = 0; y < 4 && by * 4 + y < source.height; ++y)
            {
                for (uint32_t x = 0; x < 4 && bx * 4 + x < source.width; ++x)
                {
                    const size_t pixel = size_t(by * 4 + y) * source.width + bx * 4 + x;
                    const Vec4&  value = block[y * 4 + x];
                    decoded[pixel] = {std::round(value.x * 255.0), std::round(value.y * 255.0), std::round(value.z * 255.0)};
                    result.alpha   = std::max(result.alpha, static_cast<uint32_t>(std::abs(int32_t(std::round(value.w * 255.0))
                                                                                           - source.bytes[pixel * 4 + 3])));
                }
            }
        }
    }
    // Independent whole-image accumulation, with no local-neighborhood cache.
    for (uint32_t y = 0; y < source.height; ++y)
    {
        for (uint32_t x = 0; x < source.width; ++x)
        {
            const uint32_t              nx = x + 1 < source.width ? x + 1 : use.normalRepeatS ? 0 : x;
            const uint32_t              ny = y + 1 < source.height ? y + 1 : use.normalRepeatT ? 0 : y;
            const std::array<size_t, 4> corners{size_t(y) * source.width + x, size_t(y) * source.width + nx,
                                                size_t(ny) * source.width + x, size_t(ny) * source.width + nx};
            for (uint32_t phase = 0; phase < 16; ++phase)
            {
                const double                fx = double(phase % 4) / 4;
                const double                fy = double(phase / 4) / 4;
                const std::array<double, 4> weights{(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
                std::array<double, 3>       a{}, b{};
                for (uint32_t corner = 0; corner < 4; ++corner)
                {
                    for (uint32_t channel = 0; channel < 3; ++channel)
                    {
                        a[channel] += weights[corner] * (double(source.bytes[corners[corner] * 4 + channel]) * 2 - 255);
                        b[channel] += weights[corner] * (decoded[corners[corner]][channel] * 2 - 255);
                    }
                }
                double aa = 0, bb = 0, ab = 0;
                for (uint32_t channel = 0; channel < 3; ++channel)
                {
                    aa += a[channel] * a[channel];
                    bb += b[channel] * b[channel];
                    ab += a[channel] * b[channel];
                }
                const double angle =
                    aa == 0 || bb == 0 ? 3.14159265358979323846 : std::acos(std::clamp(ab / std::sqrt(aa * bb), -1.0, 1.0));
                result.phases[phase] += angle * angle;
                result.maximum        = std::max(result.maximum, angle);
            }
        }
    }
    return result;
}

bool TestNeighborhoods()
{
    bool     valid   = true;
    uint32_t changed = 0;
    for (const std::array<uint32_t, 2>& extent :
         {std::array<uint32_t, 2>{1, 1}, {1, 7}, {2, 3}, {3, 5}, {4, 4}, {5, 7}, {9, 13}})
    {
        for (uint32_t wrapping = 0; wrapping < 4; ++wrapping)
        {
            const std::shared_ptr<ImagePixels> source = Fixture(extent[0], extent[1]);
            TextureImageUse                    use;
            use.role          = TextureImageRole::Normal;
            use.normalRepeatS = (wrapping & 1) != 0;
            use.normalRepeatT = (wrapping & 2) != 0;
            for (uint32_t alphaMode = 0; alphaMode < 3; ++alphaMode)
            {
                for (size_t pixel = 0; pixel < source->bytes.size() / 4; ++pixel)
                {
                    source->bytes[pixel * 4 + 3] = alphaMode == 0 ? 255
                                                 : alphaMode == 1 ? 63
                                                                  : static_cast<uint8_t>(pixel * 37);
                }
                std::vector<uint8_t> encoded;
                valid                               = Baseline(*source, 4, encoded) && valid;
                const Measured             before   = Measure(*source, encoded, use);
                const std::vector<uint8_t> original = encoded;
                uint64_t                   budget   = 1ull << 20;
                const Status               status =
                    RefineBC7NormalLevel(source->width, source->height, source->bytes, encoded, use, budget, nullptr);
                const Measured after = Measure(*source, encoded, use);
                valid                = Check(status.Succeeded() && budget == (1ull << 20) && before.valid && after.valid
                                                 && after.maximum <= before.maximum + 1e-7 && after.alpha <= before.alpha,
                                             "Refinement preserves maximum direction/alpha errors and refunds scratch")
                                    && valid;
                for (uint32_t phase = 0; phase < 16; ++phase)
                {
                    valid = Check(after.phases[phase] <= before.phases[phase] + 1e-9,
                                  "Every whole-image sampling phase is non-worsening at odd/clamped/repeated edges")
                         && valid;
                }
                changed += encoded != original ? 1 : 0;
            }
        }
    }
    valid = Check(changed > 0, "The neighborhood matrix must exercise accepted refinements") && valid;
    return valid;
}

struct PollState
{
    uint32_t count{0};
    uint32_t stop{UINT32_MAX};
};

bool Poll(void* context)
{
    PollState& state = *static_cast<PollState*>(context);
    return ++state.count >= state.stop;
}

bool TestProcessControl()
{
    const std::shared_ptr<ImagePixels> source = Fixture(4, 4);
    TextureSettings                    settings;
    settings.compressBC7 = true;
    TextureImageUse use;
    use.role            = TextureImageRole::Normal;
    use.requiresMipmaps = false;
    PollState                all;
    Cancellation             counter(Poll, &all);
    uint64_t                 budget   = 1ull << 20;
    const TextureImageResult complete = ProcessTextureImage(source, use, settings, budget, &counter);
    bool                     valid = Check(complete.status.Succeeded() && complete.pixels != source && complete.reason.empty()
                                               && budget + complete.ownedBytes == (1ull << 20),
                                           "Public image processing applies the refinement");
    for (uint32_t stop = 1; stop <= all.count; ++stop)
    {
        PollState    state{0, stop};
        Cancellation cancel(Poll, &state);
        budget                          = 1ull << 20;
        const TextureImageResult result = ProcessTextureImage(source, use, settings, budget, &cancel);
        valid = Check(result.status.code == StatusCode::Cancelled && !result.pixels && result.metrics.empty()
                          && budget == (1ull << 20),
                      "Every cancellation point discards unpublished pixels and refunds storage")
             && valid;
    }
    budget                           = 65536;
    const TextureImageResult limited = ProcessTextureImage(source, use, settings, budget);
    valid = Check(limited.status.code == StatusCode::ResourceLimit && !limited.pixels && budget == 65536
                      && limited.status.message == "BC7 normal refinement scratch exceeds budget",
                  "The bounded refinement scratch is charged before allocation")
         && valid;
    for (uint32_t quality = 0; quality < 4; ++quality)
    {
        settings.bc7Quality = quality;
        std::vector<uint8_t> expected;
        valid                           = Baseline(*source, quality, expected) && valid;
        budget                          = 1ull << 20;
        const TextureImageResult result = ProcessTextureImage(source, use, settings, budget);
        valid = Check(result.status.Succeeded() && result.pixels != source && result.pixels->bytes == expected,
                      "Quality levels zero through three retain baseline encoding")
             && valid;
    }
    settings.bc7Quality = 4;
    use.filterNormals   = false;
    std::vector<uint8_t> expected;
    valid                            = Baseline(*source, 4, expected) && valid;
    budget                           = 1ull << 20;
    const TextureImageResult nearest = ProcessTextureImage(source, use, settings, budget);
    valid = Check(nearest.status.Succeeded() && nearest.pixels->bytes == expected, "Nearest-only uses keep baseline encoding")
         && valid;
    return valid;
}

bool TestRegression()
{
    const std::shared_ptr<ImagePixels>          source = Fixture(8, 8);
    const std::array<std::array<uint8_t, 4>, 4> ramp{
        std::array<uint8_t, 4>{18, 128, 193, 255}, {25, 128, 204, 255}, {32, 128, 212, 255}, {40, 128, 220, 255}};
    for (size_t pixel = 0; pixel < source->bytes.size() / 4; ++pixel)
    {
        std::copy(ramp[pixel % 4].begin(), ramp[pixel % 4].end(), source->bytes.begin() + pixel * 4);
    }
    TextureImageUse use;
    use.role            = TextureImageRole::Normal;
    use.requiresMipmaps = false;
    TextureSettings settings;
    settings.compressBC7 = true;
    std::vector<uint8_t>     original;
    bool                     valid  = Baseline(*source, 4, original);
    uint64_t                 budget = 1ull << 20;
    const TextureImageResult result = ProcessTextureImage(source, use, settings, budget);
    valid                           = Check(result.status.Succeeded() && result.pixels != source && result.reason.empty(),
                                            "The repeated normal ramp stays compressed")
                                   && valid;
    if (result.pixels && result.pixels != source)
    {
        const Measured before = Measure(*source, original, use);
        const Measured after  = Measure(*source, result.pixels->bytes, use);
        double         oldSum = 0, newSum = 0;
        for (uint32_t phase = 0; phase < 16; ++phase)
        {
            oldSum += before.phases[phase];
            newSum += after.phases[phase];
        }
        valid = Check(after.valid && before.valid && newSum < oldSum && result.pixels->bytes != original,
                      "Public quality-4 processing improves the measured filtered normal ramp")
             && valid;
    }
    const std::shared_ptr<ImagePixels> undefined = Fixture(2, 1);
    undefined->bytes                             = {0, 0, 0, 255, 255, 255, 255, 255};
    budget                                       = 1ull << 20;
    const TextureImageResult rejected            = ProcessTextureImage(undefined, use, settings, budget);
    valid = Check(rejected.status.Succeeded() && rejected.pixels == undefined && !rejected.reason.empty()
                      && rejected.metrics.size() == 1 && rejected.metrics[0].normalFilteredMaxAngleDegrees == 180,
                  "Undefined interpolated normal directions retain the original image")
         && valid;
    return valid;
}

uint32_t NextCandidateValue(uint32_t& state)
{
    state = state * 1664525u + 1013904223u;
    return state;
}

bool TestCandidateOutput()
{
    BC7NormalSearchScratch scratch;
    uint32_t               state  = 0x17450321;
    uint64_t               digest = 14695981039346656037ull;
    bool                   valid  = true;
    // Recorded before endpoint memoization. Reusing scratch across distinct
    // blocks exercises reset, collisions and both full/partial score entries.
    // The inputs include ties, narrow ramps, arbitrary RGB and varying alpha.
    for (uint32_t test = 0; valid && test < 256; ++test)
    {
        std::array<uint8_t, 64> source{};
        const uint8_t           red  = static_cast<uint8_t>(NextCandidateValue(state) >> 24);
        const uint8_t           blue = static_cast<uint8_t>(NextCandidateValue(state) >> 24);
        for (uint32_t pixel = 0; pixel < 16; ++pixel)
        {
            for (uint32_t channel = 0; channel < 4; ++channel)
            {
                source[pixel * 4 + channel] = static_cast<uint8_t>(NextCandidateValue(state) >> 24);
            }
            if (test % 4 == 0)
            {
                source[pixel * 4]     = red;
                source[pixel * 4 + 1] = 128;
                source[pixel * 4 + 2] = blue;
            }
            else if (test % 4 == 1)
            {
                source[pixel * 4]     = static_cast<uint8_t>(std::min(255u, uint32_t(red) + pixel % 4));
                source[pixel * 4 + 1] = 128;
                source[pixel * 4 + 2] = blue;
            }
            else if (test % 4 == 2)
            {
                const double x        = (double(source[pixel * 4]) - 127.5) / 180;
                const double y        = (double(source[pixel * 4 + 1]) - 127.5) / 180;
                const double z        = std::sqrt(std::max(0.0, 1.0 - x * x - y * y));
                source[pixel * 4]     = static_cast<uint8_t>(std::round((x + 1) * 127.5));
                source[pixel * 4 + 1] = static_cast<uint8_t>(std::round((y + 1) * 127.5));
                source[pixel * 4 + 2] = static_cast<uint8_t>(std::round((z + 1) * 127.5));
            }
            if (test % 3 != 2)
            {
                source[pixel * 4 + 3] = test % 3 == 0 ? 255 : 63;
            }
        }
        BC7NormalCandidates candidates{};
        valid = Check(EncodeBC7NormalCandidates(source, candidates, scratch, nullptr).Succeeded(),
                      "Candidate encoding succeeds with reused scratch");
        for (const std::array<std::byte, 16>& block : candidates)
        {
            for (std::byte value : block)
            {
                digest = (digest ^ std::to_integer<uint8_t>(value)) * 1099511628211ull;
            }
        }
    }
    valid = Check(digest == 0xf728736bcf8bea18ull, "Candidate bytes match the pre-cache regression corpus") && valid;
    return valid;
}
} // namespace

int main()
{
    const bool neighborhoods = TestNeighborhoods();
    const bool control       = TestProcessControl();
    const bool regression    = TestRegression();
    const bool candidates    = TestCandidateOutput();
    std::printf("BC7 normal refinement: neighborhoods=%d control=%d regression=%d candidates=%d\n", neighborhoods, control,
                regression, candidates);
    return neighborhoods && control && regression && candidates ? 0 : 1;
}
