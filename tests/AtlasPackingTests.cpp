#include "Processing/AtlasPacking.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>

namespace
{
using namespace zenasset;
using namespace zenasset::detail;
uint32_t           checks = 0;
constexpr uint64_t Budget = 64ull << 20;

bool Check(bool condition, const char* message)
{
    ++checks;
    if (!condition)
    {
        std::fprintf(stderr, "%s\n", message);
    }
    return condition;
}

std::shared_ptr<ImagePixels> Image(uint32_t width, uint32_t height, uint32_t seed, bool mipmaps = true)
{
    std::shared_ptr<ImagePixels> result = std::make_shared<ImagePixels>();
    result->width                       = width;
    result->height                      = height;
    uint32_t mip                        = 0;
    bool     done                       = false;
    while (!done)
    {
        std::vector<uint8_t> bytes(size_t(width) * height * 4);
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                for (uint32_t channel = 0; channel < 4; ++channel)
                {
                    bytes[(size_t(y) * width + x) * 4 + channel] =
                        static_cast<uint8_t>((seed * 131 + x * 31 + y * 67 + mip * 97 + channel * 59) % 256);
                }
            }
        }
        result->mips.push_back(std::move(bytes));
        done   = !mipmaps || (width == 1 && height == 1);
        width  = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
        ++mip;
    }
    result->bytes = result->mips[0];
    return result;
}

const AtlasTileInput& FindInput(const std::vector<AtlasTileInput>& inputs, uint32_t id)
{
    const AtlasTileInput* found = &inputs.front();
    for (const AtlasTileInput& input : inputs)
    {
        if (input.id == id)
        {
            found = &input;
        }
    }
    return *found;
}

uint32_t Extent(uint32_t dimension, uint32_t gutter)
{
    return (dimension + gutter * 2 + 3) / 4 * 4;
}

bool VerifyPixels(const std::vector<AtlasTileInput>& inputs, const AtlasPackResult& result, const AtlasPackingOptions& options)
{
    bool                              valid = true;
    std::vector<std::vector<uint8_t>> occupied;
    for (const AtlasPage& page : result.pages)
    {
        const ImagePixels& pixels = *page.pixels;
        valid &= Check(pixels.width % 4 == 0 && pixels.height % 4 == 0 && pixels.width <= options.maxPageDimension
                           && pixels.height <= options.maxPageDimension,
                       "page dimensions are bounded and block aligned");
        valid &= Check(pixels.mips.size() == 1 && pixels.bytes == pixels.mips[0], "physical page has exactly one explicit mip");
        if (options.powerOfTwoPages)
        {
            valid &= Check(std::has_single_bit(pixels.width) && std::has_single_bit(pixels.height),
                           "requested power-of-two page dimensions are retained");
        }
        occupied.emplace_back(size_t(pixels.width) * pixels.height, 0);
    }
    for (const AtlasTileRegions& tile : result.tiles)
    {
        const AtlasTileInput& input  = FindInput(inputs, tile.id);
        uint32_t              width  = input.pixels->width;
        uint32_t              height = input.pixels->height;
        valid &= Check(tile.mipCount == std::max(size_t(1), input.pixels->mips.size()), "every authored mip is retained");
        for (uint32_t level = 0; level < tile.mipCount; ++level)
        {
            const AtlasMipRegion&       region   = result.mips[tile.firstMip + level];
            const AtlasPage&            page     = result.pages[region.page];
            const ImagePixels&          pixels   = *page.pixels;
            const std::vector<uint8_t>& source   = input.pixels->mips.empty() ? input.pixels->bytes : input.pixels->mips[level];
            bool                        exact    = true;
            bool                        isolated = true;
            valid &= Check(region.width == width && region.height == height && region.x % 4 == 0 && region.y % 4 == 0,
                           "logical dimensions and aligned interior origin are retained");
            valid &= Check(page.group == input.group && pixels.format == input.pixels->format,
                           "incompatible group or color transfer never shares a page");
            for (uint32_t y = 0; y < Extent(height, options.gutter); ++y)
            {
                for (uint32_t x = 0; x < Extent(width, options.gutter); ++x)
                {
                    const uint32_t sx    = x < options.gutter ? 0 : std::min(x - options.gutter, width - 1);
                    const uint32_t sy    = y < options.gutter ? 0 : std::min(y - options.gutter, height - 1);
                    const size_t   pixel = size_t(region.y - options.gutter + y) * pixels.width + region.x - options.gutter + x;
                    isolated &= occupied[region.page][pixel]++ == 0;
                    for (uint32_t channel = 0; channel < 4; ++channel)
                    {
                        exact &= pixels.bytes[pixel * 4 + channel] == source[(size_t(sy) * width + sx) * 4 + channel];
                    }
                }
            }
            valid  &= Check(exact, "tile pixels and all edge/corner gutters, including alignment padding, are exact");
            valid  &= Check(isolated, "guarded rectangles never overlap");
            width   = std::max(1u, width / 2);
            height  = std::max(1u, height / 2);
        }
    }
    for (size_t index = 0; index < result.pages.size(); ++index)
    {
        const ImagePixels& pixels = *result.pages[index].pixels;
        bool               zero   = true;
        for (size_t pixel = 0; pixel < occupied[index].size(); ++pixel)
        {
            if (occupied[index][pixel] == 0)
            {
                for (uint32_t channel = 0; channel < 4; ++channel)
                {
                    zero &= pixels.bytes[pixel * 4 + channel] == 0;
                }
            }
        }
        valid &= Check(zero, "unoccupied page space, including rounded extents, is deterministic zero");
    }
    return valid;
}

std::array<double, 4> Sample(const std::vector<uint8_t>& pixels, uint32_t width, uint32_t height, double pixelX, double pixelY)
{
    const int64_t         x  = static_cast<int64_t>(std::floor(pixelX));
    const int64_t         y  = static_cast<int64_t>(std::floor(pixelY));
    const double          fx = pixelX - std::floor(pixelX);
    const double          fy = pixelY - std::floor(pixelY);
    std::array<double, 4> result{};
    for (uint32_t dy = 0; dy < 2; ++dy)
    {
        for (uint32_t dx = 0; dx < 2; ++dx)
        {
            const uint32_t sx     = static_cast<uint32_t>(std::clamp(x + dx, int64_t(0), int64_t(width - 1)));
            const uint32_t sy     = static_cast<uint32_t>(std::clamp(y + dy, int64_t(0), int64_t(height - 1)));
            const double   weight = (dx == 0 ? 1 - fx : fx) * (dy == 0 ? 1 - fy : fy);
            for (uint32_t channel = 0; channel < 4; ++channel)
            {
                result[channel] += weight * pixels[(size_t(sy) * width + sx) * 4 + channel];
            }
        }
    }
    return result;
}

bool VerifySampling(const std::vector<AtlasTileInput>& inputs, const AtlasPackResult& result)
{
    bool         valid = true;
    const double coordinates[]{0, 0.001, 0.125, 0.499, 0.5, 0.987, 1};
    for (const AtlasTileRegions& tile : result.tiles)
    {
        const ImagePixels& source = *FindInput(inputs, tile.id).pixels;
        bool               exact  = true;
        for (double u : coordinates)
        {
            for (double v : coordinates)
            {
                for (uint32_t level = 0; level < tile.mipCount; ++level)
                {
                    // Includes the 1x1 tail, partial chains and a blend across
                    // two independently guarded regions/pages. This is a CPU
                    // mathematical check, not a claim about GPU LOD precision.
                    std::array<double, 4> expected{};
                    std::array<double, 4> actual{};
                    for (uint32_t step = 0; step < 2; ++step)
                    {
                        const uint32_t              mip       = std::min(level + step, tile.mipCount - 1);
                        const AtlasMipRegion&       region    = result.mips[tile.firstMip + mip];
                        const ImagePixels&          page      = *result.pages[region.page].pixels;
                        const std::vector<uint8_t>& bytes     = source.mips.empty() ? source.bytes : source.mips[mip];
                        const double                x         = u * region.width - 0.5;
                        const double                y         = v * region.height - 0.5;
                        const std::array<double, 4> reference = Sample(bytes, region.width, region.height, x, y);
                        const std::array<double, 4> packed =
                            Sample(page.bytes, page.width, page.height, region.x + x, region.y + y);
                        const double weight = step == 0 ? 0.37 : 0.63;
                        for (uint32_t channel = 0; channel < 4; ++channel)
                        {
                            expected[channel] += reference[channel] * weight;
                            actual[channel]   += packed[channel] * weight;
                        }
                    }
                    for (uint32_t channel = 0; channel < 4; ++channel)
                    {
                        exact &= std::abs(expected[channel] - actual[channel]) < 1e-10;
                    }
                }
            }
        }
        valid &= Check(exact, "bilinear boundaries and manual logical-mip blending preserve sampled values");
    }
    return valid;
}

bool Same(const AtlasPackResult& a, const AtlasPackResult& b)
{
    bool same = a.tiles == b.tiles && a.mips == b.mips && a.pages.size() == b.pages.size() && a.ownedBytes == b.ownedBytes;
    for (size_t index = 0; same && index < a.pages.size(); ++index)
    {
        const ImagePixels& left  = *a.pages[index].pixels;
        const ImagePixels& right = *b.pages[index].pixels;
        same = a.pages[index].group == b.pages[index].group && left.width == right.width && left.height == right.height
            && left.format == right.format && left.bytes == right.bytes && left.mips == right.mips;
    }
    return same;
}

bool LayoutAndMips()
{
    std::vector<AtlasTileInput> inputs{
        {40, 0, Image(13, 7, 1)}, {5, 0, Image(3, 17, 2)}, {9, 0, Image(1, 1, 3)}, {18, 1, Image(16, 16, 4)}};
    std::shared_ptr<ImagePixels> partial = Image(11, 9, 5);
    partial->mips.resize(2);
    inputs.push_back({80, 0, partial});
    std::shared_ptr<ImagePixels> color = Image(7, 5, 6);
    color->format                      = PixelFormat::RGBA8Srgb;
    inputs.push_back({3, 0, color});
    std::shared_ptr<ImagePixels> implicit = Image(3, 5, 7, false);
    implicit->mips.clear();
    inputs.push_back({27, 0, implicit});
    const std::vector<uint8_t> sourceBefore = inputs.front().pixels->bytes;
    AtlasPackingOptions        options;
    options.maxPageDimension        = 48;
    uint64_t              remaining = Budget;
    const AtlasPackResult result    = PackAtlasTiles(inputs, options, remaining);
    bool                  valid     = Check(result.status.Succeeded(), result.status.message.c_str());
    if (result.status.Succeeded())
    {
        valid &= Check(result.pages.size() > 3, "multiple bounded pages include distinct groups and formats");
        valid &=
            Check(result.tiles.size() == inputs.size() && result.tiles.front().id == 3, "logical output order uses stable IDs");
        valid &= VerifyPixels(inputs, result, options);
        valid &= VerifySampling(inputs, result);
        valid &=
            Check(Budget - remaining == result.ownedBytes && result.ownedBytes != 0, "temporary reservations are refunded");
        std::reverse(inputs.begin(), inputs.end());
        uint64_t              repeatBudget  = Budget;
        const AtlasPackResult repeat        = PackAtlasTiles(inputs, options, repeatBudget);
        valid                              &= Check(repeat.status.Succeeded() && Same(result, repeat),
                                                    "input permutation preserves every output byte and placement");
        valid &= Check(inputs.back().pixels->bytes == sourceBefore, "packing does not modify borrowed source pixels");
    }
    return valid;
}

bool Failed(const std::vector<AtlasTileInput>& inputs,
            const AtlasPackingOptions&         options,
            StatusCode                         expected,
            uint64_t                           budget       = Budget,
            const Cancellation*                cancellation = nullptr)
{
    uint64_t              remaining = budget;
    const AtlasPackResult result    = PackAtlasTiles(inputs, options, remaining, cancellation);
    return Check(result.status.code == expected && !result.status.message.empty(),
                 "failure has the expected status and message")
        && Check(result.tiles.empty() && result.mips.empty() && result.pages.empty() && result.ownedBytes == 0,
                 "failure publishes no partial atlas")
        && Check(remaining == budget, "failure refunds all requested storage");
}

bool FailuresAndLimits()
{
    AtlasPackingOptions options;
    options.maxPageDimension = 16;
    std::vector<AtlasTileInput> inputs{
        {1, 0, Image(4, 4, 1, false)}, {2, 0, Image(4, 4, 2, false)}, {3, 0, Image(4, 4, 3, false)}};
    uint64_t              remaining = Budget;
    const AtlasPackResult multi     = PackAtlasTiles(inputs, options, remaining);
    bool valid        = Check(multi.status.Succeeded() && multi.pages.size() == 3, "page limit counts actual physical pages");
    options.maxPages  = 2;
    valid            &= Failed(inputs, options, StatusCode::ResourceLimit);
    options.maxPages  = 64;
    options.maxTiles  = 2;
    valid            &= Failed(inputs, options, StatusCode::ResourceLimit);
    options.maxTiles  = 8192;
    options.maxMipRegions     = 2;
    valid                    &= Failed(inputs, options, StatusCode::ResourceLimit);
    options.maxMipRegions     = 65536;
    options.gutter            = UINT32_MAX;
    valid                    &= Failed(inputs, options, StatusCode::InvalidArgument);
    options.gutter            = 4;
    options.maxPageDimension  = UINT32_MAX;
    valid                    &= Failed(inputs, options, StatusCode::InvalidArgument);
    options.maxPageDimension  = 16;
    valid                    &= Failed(inputs, options, StatusCode::ResourceLimit, 0);
    valid                    &= Failed(inputs, options, StatusCode::ResourceLimit, 500);
    valid                    &= Failed(inputs, options, StatusCode::ResourceLimit, multi.ownedBytes);
    inputs[1].id              = 1;
    valid                    &= Failed(inputs, options, StatusCode::InvalidArgument);
    inputs.resize(1);
    inputs[0].pixels                  = nullptr;
    valid                            &= Failed(inputs, options, StatusCode::InvalidData);
    std::shared_ptr<ImagePixels> bad  = Image(4, 4, 1);
    inputs[0].pixels                  = bad;
    bad->format                       = PixelFormat::BC7Unorm;
    valid                            &= Failed(inputs, options, StatusCode::UnsupportedFormat);
    bad->format                       = PixelFormat::RGBA8Unorm;
    bad->width                        = 0;
    valid                            &= Failed(inputs, options, StatusCode::InvalidData);
    bad->width                        = UINT32_MAX;
    valid                            &= Failed(inputs, options, StatusCode::ResourceLimit);
    bad->width                        = 4;
    bad->bytes.pop_back();
    valid           &= Failed(inputs, options, StatusCode::InvalidData);
    bad->bytes       = bad->mips[0];
    bad->mips[0][0] ^= 1;
    valid           &= Failed(inputs, options, StatusCode::InvalidData);
    bad->mips[0]     = bad->bytes;
    bad->mips[1].pop_back();
    valid            &= Failed(inputs, options, StatusCode::InvalidData);
    bad               = Image(1, 1, 1);
    inputs[0].pixels  = bad;
    bad->mips.push_back(bad->bytes);
    valid                       &= Failed(inputs, options, StatusCode::InvalidData);
    remaining                    = Budget;
    const AtlasPackResult empty  = PackAtlasTiles({}, options, remaining);
    valid &= Check(empty.status.Succeeded() && empty.pages.empty() && empty.ownedBytes == 0 && remaining == Budget,
                   "empty input is a no-allocation success");
    return valid;
}

bool PowerOfTwoPages()
{
    std::vector<AtlasTileInput> inputs{
        {40, 0, Image(13, 7, 1)}, {5, 0, Image(3, 17, 2)}, {9, 0, Image(1, 1, 3)}, {18, 1, Image(56, 56, 4, false)}};
    std::shared_ptr<ImagePixels> partial = Image(11, 9, 5);
    partial->mips.resize(2);
    inputs.push_back({80, 0, partial});
    std::shared_ptr<ImagePixels> color = Image(7, 5, 6);
    color->format                      = PixelFormat::RGBA8Srgb;
    inputs.push_back({3, 0, color});
    AtlasPackingOptions options;
    options.maxPageDimension            = 64;
    uint64_t              croppedBudget = Budget;
    const AtlasPackResult cropped       = PackAtlasTiles(inputs, options, croppedBudget);
    options.powerOfTwoPages             = true;
    uint64_t              paddedBudget  = Budget;
    const AtlasPackResult padded        = PackAtlasTiles(inputs, options, paddedBudget);
    bool valid = Check(cropped.status.Succeeded() && padded.status.Succeeded(), "both page extent policies pack the fixture");
    if (cropped.status.Succeeded() && padded.status.Succeeded())
    {
        valid &=
            Check(cropped.tiles == padded.tiles && cropped.mips == padded.mips && cropped.pages.size() == padded.pages.size(),
                  "rounding page extents preserves every logical region and page assignment");
        valid               &= VerifyPixels(inputs, padded, options);
        valid               &= VerifySampling(inputs, padded);
        uint64_t extraBytes  = 0;
        for (size_t index = 0; index < padded.pages.size(); ++index)
        {
            const ImagePixels& before = *cropped.pages[index].pixels;
            const ImagePixels& after  = *padded.pages[index].pixels;
            valid      &= Check(after.width == std::bit_ceil(before.width) && after.height == std::bit_ceil(before.height),
                                "pages use the smallest power-of-two extents that contain their used rectangles");
            extraBytes += 2 * (after.bytes.size() - before.bytes.size());
        }
        valid &= Check(extraBytes != 0 && padded.ownedBytes == cropped.ownedBytes + extraBytes
                           && Budget - paddedBudget == padded.ownedBytes,
                       "both retained page copies charge all added padding and refund planning storage");
        std::reverse(inputs.begin(), inputs.end());
        uint64_t              repeatBudget = Budget;
        const AtlasPackResult repeat       = PackAtlasTiles(inputs, options, repeatBudget);
        valid &= Check(repeat.status.Succeeded() && Same(padded, repeat), "rounded pages are permutation deterministic");
        options.maxPages  = static_cast<uint32_t>(padded.pages.size() - 1);
        valid            &= Failed(inputs, options, StatusCode::ResourceLimit);
        options.maxPages  = 64;
    }
    inputs                           = {{1, 0, Image(13, 7, 7, false)}};
    options.powerOfTwoPages          = false;
    uint64_t              remaining  = Budget;
    const AtlasPackResult small      = PackAtlasTiles(inputs, options, remaining);
    valid                           &= Check(small.status.Succeeded(), "small cropped page packs");
    if (small.status.Succeeded())
    {
        const uint64_t limited     = small.ownedBytes + 256;
        remaining                  = limited;
        const AtlasPackResult fits = PackAtlasTiles(inputs, options, remaining);
        valid &= Check(fits.status.Succeeded(), "limited budget can retain the cropped page and its planning storage");
        options.powerOfTwoPages  = true;
        valid                   &= Failed(inputs, options, StatusCode::ResourceLimit, limited);
    }
    options.maxPageDimension     = 48;
    valid                       &= Failed(inputs, options, StatusCode::InvalidArgument);
    options.maxPageDimension     = 64;
    inputs[0].pixels             = Image(57, 56, 8, false);
    valid                       &= Failed(inputs, options, StatusCode::ResourceLimit);
    remaining                    = Budget;
    const AtlasPackResult empty  = PackAtlasTiles({}, options, remaining);
    valid &= Check(empty.status.Succeeded() && empty.pages.empty() && empty.ownedBytes == 0 && remaining == Budget,
                   "rounded page policy preserves empty-input success without allocation");
    return valid;
}

struct CancelState
{
    uint32_t polls{0};
    uint32_t limit{0};
};

bool CancelAfterPolls(void* user)
{
    CancelState& state = *static_cast<CancelState*>(user);
    return ++state.polls >= state.limit;
}

bool CancellationChecks()
{
    const std::vector<AtlasTileInput> inputs{{1, 0, Image(256, 256, 5)}};
    bool                              valid = true;
    for (bool powerOfTwo : {false, true})
    {
        AtlasPackingOptions options;
        options.powerOfTwoPages = powerOfTwo;
        for (uint32_t limit : {1u, 20u, 200u, 500u})
        {
            CancelState  state{0, limit};
            Cancellation cancellation(CancelAfterPolls, &state);
            valid &= Failed(inputs, options, StatusCode::Cancelled, Budget, &cancellation);
            valid &= Check(state.polls == limit, "cancellation is observed during planning and page row copies");
        }
    }
    return valid;
}

bool VariedRectangles()
{
    std::vector<AtlasTileInput> inputs;
    for (uint32_t index = 0; index < 40; ++index)
    {
        inputs.push_back({index * 3, index % 3, Image(1 + (index * 17) % 51, 1 + (index * 29) % 53, index)});
    }
    AtlasPackingOptions options;
    options.maxPageDimension        = 64;
    uint64_t              remaining = Budget;
    const AtlasPackResult result    = PackAtlasTiles(inputs, options, remaining);
    bool                  valid     = Check(result.status.Succeeded(), result.status.message.c_str());
    if (result.status.Succeeded())
    {
        valid &= VerifyPixels(inputs, result, options);
        valid &= VerifySampling(inputs, result);
    }
    return valid;
}
} // namespace

int main()
{
    bool valid  = LayoutAndMips();
    valid      &= FailuresAndLimits();
    valid      &= CancellationChecks();
    valid      &= VariedRectangles();
    valid      &= PowerOfTwoPages();
    std::printf("Atlas packing: %u checks, %s\n", checks, valid ? "passed" : "failed");
    return valid ? 0 : 1;
}
