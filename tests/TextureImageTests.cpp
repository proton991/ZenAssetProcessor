#include "Processing/TextureImage.h"
#include "Codecs/BlockDecoding.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace
{
using namespace zenasset;
using namespace zenasset::detail;

bool Check(bool valid, const char* message)
{
    if (!valid)
    {
        std::printf("FAIL: %s\n", message);
    }
    return valid;
}

std::shared_ptr<ImagePixels> Solid(uint32_t width, uint32_t height, std::array<uint8_t, 4> color)
{
    std::shared_ptr<ImagePixels> image = std::make_shared<ImagePixels>();
    image->width                       = width;
    image->height                      = height;
    image->bytes.resize(size_t(width) * height * 4);
    for (size_t offset = 0; offset < image->bytes.size(); offset += 4)
    {
        std::copy(color.begin(), color.end(), image->bytes.begin() + offset);
    }
    return image;
}

TextureImageResult Process(std::shared_ptr<const ImagePixels> image,
                           const TextureImageUse&             use,
                           const TextureSettings&             settings)
{
    uint64_t           budget = 16ull << 20;
    TextureImageResult result = ProcessTextureImage(std::move(image), use, settings, budget);
    if (result.status.Succeeded() && budget + result.ownedBytes != (16ull << 20))
    {
        result.status = {StatusCode::InvalidData, "Budget accounting mismatch"};
    }
    return result;
}

bool TestMips()
{
    std::shared_ptr<ImagePixels> image = Solid(3, 1, {255, 255, 255, 255});
    image->bytes[0] = image->bytes[1] = image->bytes[2] = 0;
    TextureImageUse use;
    use.srgb = true;
    TextureSettings settings;
    settings.generateMipmaps       = true;
    const TextureImageResult color = Process(image, use, settings);
    bool                     valid = Check(color.status.Succeeded() && color.pixels != image && color.pixels->mips.size() == 2
                                               && color.pixels->mips[0] == image->bytes && color.pixels->mips[1][0] == 213,
                                           "odd-width area filtering in linear light");
    use.role                       = TextureImageRole::Data;
    use.srgb                       = false;
    const TextureImageResult data  = Process(image, use, settings);
    valid = Check(data.status.Succeeded() && data.pixels->mips[1][0] == 170, "linear scalar mip average") && valid;
    image = Solid(5, 7, {20, 40, 60, 255});
    image->mips.push_back(image->bytes);
    image->mips.push_back(Solid(2, 3, {80, 100, 120, 255})->bytes);
    const TextureImageResult partial = Process(image, use, settings);
    valid = Check(partial.status.Succeeded() && partial.pixels->mips.size() == 3 && partial.pixels->bytes == image->bytes
                      && partial.pixels->mips[1] == image->mips[1] && partial.pixels->mips[2][0] == 80,
                  "authored partial chain preserved and tail appended")
         && valid;
    image = Solid(2, 1, {255, 128, 128, 255});
    image->bytes[4]                 = 128;
    image->bytes[5]                 = 255;
    use.role                        = TextureImageRole::Normal;
    const TextureImageResult normal = Process(image, use, settings);
    valid                           = Check(normal.status.Succeeded() && normal.pixels->mips[1][0] == normal.pixels->mips[1][1]
                                                && normal.pixels->mips[1][0] > 210 && normal.pixels->mips[1][2] == 128,
                                            "normal mip averages directions then renormalizes")
                                   && valid;
    return valid;
}

bool TestEncoding()
{
    bool            valid = true;
    TextureSettings settings;
    settings.compressBC7 = true;
    TextureImageUse use;
    use.srgb = true;
    for (const std::array<uint32_t, 2>& extent : {std::array<uint32_t, 2>{1, 1}, {3, 5}, {5, 7}})
    {
        const std::shared_ptr<ImagePixels> image   = Solid(extent[0], extent[1], {200, 40, 20, 128});
        const TextureImageResult           encoded = Process(image, use, settings);
        valid = Check(encoded.status.Succeeded() && encoded.reason.empty() && encoded.pixels != image
                          && encoded.pixels->format == PixelFormat::BC7Srgb && encoded.pixels->mips.back().size() == 16
                          && encoded.pixels->bytes == encoded.pixels->mips[0] && encoded.pixelBytes > 0
                          && encoded.metrics.size() == encoded.pixels->mips.size(),
                      "BC7 tiny/odd mip chain and owned accounting")
             && valid;
        for (const TextureMipQuality& quality : encoded.metrics)
        {
            valid = Check(quality.colorRmse <= settings.colorMaxRmse && quality.colorPsnr >= settings.colorMinPsnr,
                          "every mip independently meets color quality")
                 && valid;
        }
        const TextureImageResult repeated = Process(image, use, settings);
        valid                             = Check(repeated.status.Succeeded() && repeated.pixels->mips == encoded.pixels->mips,
                                                  "deterministic repeated BC7 encoding")
                                         && valid;
    }
    return valid;
}

bool TestFallbacks()
{
    TextureSettings settings;
    settings.compressBC7 = true;
    TextureImageUse use;
    use.requiresMipmaps                = false;
    std::shared_ptr<ImagePixels> image = Solid(4, 4, {0, 0, 0, 255});
    for (size_t offset = 0; offset < image->bytes.size(); ++offset)
    {
        image->bytes[offset] = static_cast<uint8_t>((offset * 113 + offset * offset * 7) & 255);
    }
    settings.colorMaxRmse             = 0;
    settings.colorMinPsnr             = 100;
    const TextureImageResult rejected = Process(image, use, settings);
    bool                     valid = Check(rejected.status.Succeeded() && rejected.pixels == image && !rejected.reason.empty()
                                               && !rejected.metrics.empty() && rejected.pixelBytes == 0,
                                           "quality rejection retains original whole image");
    settings                       = {};
    settings.compressBC7           = true;
    use.requiresMipmaps            = true;
    use.exactAlpha                 = true;
    const TextureImageResult exact = Process(image, use, settings);
    valid                          = Check(exact.status.Succeeded() && exact.pixels == image && !exact.reason.empty(),
                                           "exact alpha retains incomplete original chain")
                                  && valid;
    use.exactAlpha                 = false;
    use.alphaMask                  = true;
    image                          = Solid(2, 1, {100, 100, 100, 0});
    image->bytes[7]                = 255;
    const TextureImageResult coverage = Process(image, use, settings);
    valid                 = Check(coverage.status.Succeeded() && coverage.pixels == image && !coverage.reason.empty(),
                                  "unrepresentable tiny-mip alpha coverage retains whole image")
                         && valid;
    settings.maxDimension = 1;
    const TextureImageResult oversized = Process(image, use, settings);
    valid                        = Check(oversized.status.Succeeded() && oversized.pixels == image && !oversized.reason.empty(),
                                         "dimension limit never silently resizes")
                                && valid;
    settings.maxDimension        = 16384;
    image->format                = PixelFormat::RGBA16Float;
    const TextureImageResult hdr = Process(image, use, settings);
    valid = Check(hdr.status.Succeeded() && hdr.pixels == image && !hdr.reason.empty(), "HDR remains unchanged") && valid;
    return valid;
}

bool Poll(void* data)
{
    uint32_t& calls = *static_cast<uint32_t*>(data);
    ++calls;
    return calls > 5;
}

bool TestControl()
{
    const std::shared_ptr<ImagePixels> image = Solid(16, 16, {80, 120, 160, 255});
    TextureSettings                    settings;
    TextureImageUse                    use;
    uint64_t                           budget    = 0;
    const TextureImageResult           reference = ProcessTextureImage(image, use, settings, budget);
    bool valid = Check(reference.status.Succeeded() && reference.pixels == image && reference.ownedBytes == 0 && budget == 0,
                       "reference settings share original without allocation");
    settings.compressBC7             = true;
    budget                           = 1;
    const TextureImageResult limited = ProcessTextureImage(image, use, settings, budget);
    valid                            = Check(limited.status.code == StatusCode::ResourceLimit && !limited.pixels && budget == 1,
                                             "failed allocation restores all budget")
                                    && valid;
    budget                           = 1ull << 20;
    uint32_t                 calls   = 0;
    Cancellation             cancellation(Poll, &calls);
    const TextureImageResult cancelled = ProcessTextureImage(image, use, settings, budget, &cancellation);
    valid = Check(cancelled.status.code == StatusCode::Cancelled && !cancelled.pixels && cancelled.metrics.empty()
                      && budget == (1ull << 20),
                  "mid-processing cancellation restores budget and clears output")
         && valid;
    image->mips.push_back(std::vector<uint8_t>(3));
    const TextureImageResult malformed = Process(image, use, settings);
    valid =
        Check(malformed.status.code == StatusCode::InvalidData && !malformed.pixels, "malformed mip ownership fails") && valid;
    return valid;
}

bool TestAccumulatedCoverage()
{
    // The base covers 14/16 texels; filtering covers 3/4, then BC7 covers 2/4.
    // Each local drift is <=5/16, but the final drift is 6/16 and must fail.
    const uint8_t                pixels[] = {61,  210, 237, 118, 227, 98,  27,  118, 238, 35,  143, 129, 237, 132, 216, 129,
                                             121, 152, 96,  129, 79,  160, 103, 129, 44,  247, 171, 129, 235, 107, 173, 129,
                                             183, 234, 148, 129, 54,  79,  214, 129, 222, 243, 38,  129, 3,   96,  41,  129,
                                             191, 26,  94,  129, 113, 158, 108, 129, 243, 39,  180, 129, 40,  122, 226, 129};
    std::shared_ptr<ImagePixels> image    = Solid(4, 4, {0, 0, 0, 0});
    image->bytes.assign(std::begin(pixels), std::end(pixels));
    TextureImageUse use;
    use.alphaMask = true;
    TextureSettings settings;
    settings.generateMipmaps       = true;
    settings.alphaCoverageMaxError = 1;
    const TextureImageResult raw   = Process(image, use, settings);
    bool valid = Check(raw.status.Succeeded() && raw.pixels != image && raw.pixels->mips[1][3] < 128
                           && raw.pixels->mips[1][7] >= 128 && raw.pixels->mips[1][11] >= 128 && raw.pixels->mips[1][15] >= 128,
                       "coverage regression filtered coverage is 3/4");
    settings.generateMipmaps       = false;
    settings.compressBC7           = true;
    settings.bc7Quality            = 0;
    settings.colorMinPsnr          = 0;
    settings.colorMaxRmse          = 1;
    settings.scalarMaxError        = 1;
    const TextureImageResult loose = Process(image, use, settings);
    std::array<Vec4, 16>     decoded{};
    bool                     decodedValid = loose.status.Succeeded() && loose.pixels != image;
    if (decodedValid)
    {
        decodedValid = DecodeBlock(PixelFormat::BC7Unorm, std::as_bytes(std::span(loose.pixels->mips[1])), decoded).Succeeded();
    }
    uint32_t covered = 0;
    for (uint32_t y = 0; y < 2; ++y)
    {
        for (uint32_t x = 0; x < 2; ++x)
        {
            covered += decoded[y * 4 + x].w >= 0.5f ? 1 : 0;
        }
    }
    valid                           = Check(decodedValid && covered == 2 && loose.metrics[1].alphaCoverageError == 0.375,
                                            "coverage regression decoded coverage measured against base target")
                                   && valid;
    settings.alphaCoverageMaxError  = 0.3125f;
    const TextureImageResult strict = Process(image, use, settings);
    valid                           = Check(strict.status.Succeeded() && strict.pixels == image && strict.metrics.size() == 2
                                                && strict.metrics[1].alphaCoverageError == 0.375 && !strict.reason.empty(),
                                            "combined filtering and encoding coverage drift rejects whole image")
                                   && valid;
    return valid;
}

bool TestRoleGates()
{
    std::shared_ptr<ImagePixels> image = Solid(4, 4, {0, 0, 0, 255});
    for (size_t pixel = 0; pixel < 16; ++pixel)
    {
        for (size_t channel = 0; channel < 3; ++channel)
        {
            image->bytes[pixel * 4 + channel] = static_cast<uint8_t>((pixel * 113 + pixel * pixel * 7 + channel * 61) & 255);
        }
    }
    TextureImageUse use;
    use.requiresMipmaps = false;
    use.role            = TextureImageRole::Data;
    use.srgb            = true;
    TextureSettings settings;
    settings.compressBC7          = true;
    settings.scalarMaxError       = 0;
    const TextureImageResult data = Process(image, use, settings);
    bool                     valid =
        Check(data.status.Succeeded() && data.pixels == image && !data.metrics.empty() && data.metrics[0].scalarMaxError > 0,
              "sRGB scalar data applies per-channel gate");
    use.role                        = TextureImageRole::Normal;
    use.srgb                        = false;
    settings.scalarMaxError         = 1;
    settings.normalRmsAngleDegrees  = 0;
    settings.normalMaxAngleDegrees  = 0;
    const TextureImageResult normal = Process(image, use, settings);
    valid          = Check(normal.status.Succeeded() && normal.pixels == image && !normal.metrics.empty()
                               && normal.metrics[0].normalRmsAngleDegrees > 0 && normal.metrics[0].normalMaxAngleDegrees > 0,
                           "normal BC7 gates independent decoded angular error")
                  && valid;
    use.role       = TextureImageRole::Color;
    use.exactAlpha = true;
    settings.colorMaxRmse = 1;
    settings.colorMinPsnr = 0;
    for (size_t pixel = 0; pixel < 16; ++pixel)
    {
        image->bytes[pixel * 4 + 3] = static_cast<uint8_t>(pixel * 13 + 7);
    }
    const TextureImageResult alpha = Process(image, use, settings);
    valid = Check(alpha.status.Succeeded() && alpha.pixels == image && !alpha.metrics.empty() && !alpha.metrics[0].exactAlpha,
                  "exact-alpha gate rejects changed alpha bytes")
         && valid;
    return valid;
}

bool TestNormalXYEncoding()
{
    TextureSettings settings;
    settings.compressBC5Normals = true;
    TextureImageUse use;
    use.role   = TextureImageRole::Normal;
    bool valid = true;
    for (const std::array<uint32_t, 2>& extent : {std::array<uint32_t, 2>{1, 1}, {3, 5}, {5, 7}})
    {
        const std::shared_ptr<ImagePixels> image   = Solid(extent[0], extent[1], {128, 128, 255, 17});
        const TextureImageResult           encoded = Process(image, use, settings);
        valid &= Check(encoded.status.Succeeded() && encoded.pixels != image && encoded.reason.empty()
                           && encoded.pixels->format == PixelFormat::BC5Unorm && encoded.pixels->mips.back().size() == 16
                           && encoded.pixels->bytes == encoded.pixels->mips[0],
                       "BC5 odd/tiny complete chains retain 16-byte blocks and base ownership");
        for (const TextureMipQuality& quality : encoded.metrics)
        {
            valid &=
                Check(quality.normalRmsAngleDegrees <= settings.normalRmsAngleDegrees
                          && quality.normalMaxAngleDegrees <= settings.normalMaxAngleDegrees && quality.scalarMaxError == 0,
                      "BC5 gates reconstructed directions independently per mip and ignores unused normal alpha");
        }
        std::array<Vec4, 16> decoded{};
        const Status         decode =
            DecodeBlock(PixelFormat::BC5Unorm, std::as_bytes(std::span(encoded.pixels->mips.back())), decoded);
        valid    &= Check(decode.Succeeded() && decoded[0].w == 1, "BC5 normal alpha is absent from encoded storage");
        use.role  = TextureImageRole::Color;
        const TextureImageResult color = Process(image, use, settings);
        valid    &= Check(color.status.Succeeded() && color.pixels == image && color.pixels->bytes[3] == 17,
                          "BC5-only policy leaves color alpha and authored ownership untouched");
        use.role  = TextureImageRole::Normal;
        const TextureImageResult repeat = Process(image, use, settings);
        valid &= Check(repeat.status.Succeeded() && repeat.pixels->mips == encoded.pixels->mips, "BC5 output is deterministic");
    }
    std::shared_ptr<ImagePixels> image  = Solid(4, 4, {128, 128, 127, 17});
    settings.compressBC7                = true;
    const TextureImageResult negative   = Process(image, use, settings);
    valid                              &= Check(negative.status.Succeeded() && negative.pixels == image
                                                    && negative.reason.find("positive-Z") != std::string::npos,
                                                "negative-Z BC5 rejection retains original instead of falling through to BC7");
    image                               = Solid(4, 4, {128, 128, 255, 17});
    image->mips                         = {image->bytes, Solid(2, 2, {128, 128, 127, 17})->bytes};
    const TextureImageResult negativeTail = Process(image, use, settings);
    valid &= Check(negativeTail.status.Succeeded() && negativeTail.pixels == image && !negativeTail.reason.empty(),
                   "negative-Z authored tail rejects the whole image before filtering can hide it");
    image->mips.clear();
    settings.normalRmsAngleDegrees = settings.normalMaxAngleDegrees = 0;
    const TextureImageResult strict                                 = Process(image, use, settings);
    valid                         &= Check(strict.status.Succeeded() && strict.pixels == image && !strict.metrics.empty()
                                               && strict.metrics[0].normalMaxAngleDegrees > 0,
                                           "BC5 gate includes normalization and XY quantization versus original direction");
    settings.compressBC7           = false;
    use.srgb                       = true;
    const TextureImageResult srgb  = Process(image, use, settings);
    valid &= Check(srgb.status.Succeeded() && srgb.pixels == image, "BC5 is never selected for an sRGB normal view");
    return valid;
}

struct CancellationPosition
{
    uint32_t calls{0};
    uint32_t stop{UINT32_MAX};
};

bool CancelAtPosition(void* context)
{
    CancellationPosition& position = *static_cast<CancellationPosition*>(context);
    return ++position.calls >= position.stop;
}

bool TestFilteredNormalEncoding()
{
    TextureSettings settings;
    settings.compressBC5Normals = true;
    TextureImageUse use;
    use.role            = TextureImageRole::Normal;
    use.requiresMipmaps = false;
    bool valid          = true;
    for (const std::array<uint32_t, 2>& extent : {std::array<uint32_t, 2>{2, 1}, {1, 2}, {5, 7}, {7, 5}})
    {
        // Opposing, nearly unit directions: endpoints encode within the old
        // bounds, but filtering XY before reconstructing Z bends the normal arc.
        std::shared_ptr<ImagePixels> image = Solid(extent[0], extent[1], {26, 128, 204, 255});
        for (uint32_t y = 0; y < image->height; ++y)
        {
            for (uint32_t x = 0; x < image->width; ++x)
            {
                if ((image->width > 1 && x + 1u == image->width) || (image->width == 1 && y + 1u == image->height))
                {
                    image->bytes[(size_t(y) * image->width + x) * 4u] = 230;
                }
            }
        }
        const TextureImageResult result = Process(image, use, settings);
        valid &= Check(result.status.Succeeded() && result.pixels == image && result.metrics.size() == 1
                           && result.reason == "BC5 filtered mip exceeds normal angular limits",
                       "Filtered BC5 errors retain the original image, including odd and one-dimensional extents");
        if (!result.metrics.empty())
        {
            const TextureMipQuality& quality = result.metrics[0];
            valid &= Check(quality.normalRmsAngleDegrees < 1 && quality.normalMaxAngleDegrees < 5
                               && quality.normalFilteredMaxAngleDegrees > 5 && quality.normalFilteredRmsAngleDegrees > 1,
                           "The regression passes texel gates but violates unchanged limits after filtering");
        }
        std::shared_ptr<ImagePixels> authored = Solid(extent[0] * 2u, extent[1] * 2u, {128, 128, 255, 255});
        authored->mips                        = {authored->bytes, image->bytes};
        const TextureImageResult tail         = Process(authored, use, settings);
        valid &= Check(tail.status.Succeeded() && tail.pixels == authored && tail.metrics.size() == 2
                           && tail.reason == "BC5 filtered mip exceeds normal angular limits",
                       "A filtered authored-tail failure preserves every original mip");
    }

    const std::shared_ptr<ImagePixels> flat             = Solid(64, 1, {128, 128, 255, 255});
    bool                               rowLimitObserved = false;
    for (uint64_t available = 4096; available < 12288 && !rowLimitObserved; available += 128)
    {
        uint64_t                 budget = available;
        const TextureImageResult result = ProcessTextureImage(flat, use, settings, budget);
        valid &= Check(result.status.Succeeded()
                           ? budget + result.ownedBytes == available
                           : result.status.code == StatusCode::ResourceLimit && !result.pixels && budget == available,
                       "Filtered-normal allocation failures refund all temporary storage");
        rowLimitObserved = result.status.message == "Filtered normal quality rows exceed budget";
    }
    valid &= Check(rowLimitObserved, "The two-row sampling scratch is charged before allocation");
    CancellationPosition     complete;
    Cancellation             count(CancelAtPosition, &complete);
    uint64_t                 budget  = 1ull << 20;
    const TextureImageResult encoded = ProcessTextureImage(flat, use, settings, budget, &count);
    valid &= Check(encoded.status.Succeeded() && encoded.pixels != flat && budget + encoded.ownedBytes == (1ull << 20),
                   "Flat BC5 normals remain compressed with full accounting");
    for (uint32_t stop = 1; stop <= complete.calls; ++stop)
    {
        CancellationPosition position{0, stop};
        Cancellation         cancel(CancelAtPosition, &position);
        budget                          = 1ull << 20;
        const TextureImageResult result = ProcessTextureImage(flat, use, settings, budget, &cancel);
        valid &= Check(result.status.code == StatusCode::Cancelled && !result.pixels && budget == (1ull << 20),
                       "Cancellation at every checked position, including filtered rows, refunds storage");
    }
    return valid;
}

std::shared_ptr<ImagePixels> UvNormals(uint32_t                      width,
                                       uint32_t                      height,
                                       const std::array<uint8_t, 4>& first,
                                       const std::array<uint8_t, 4>& second)
{
    std::shared_ptr<ImagePixels> image = Solid(width, height, first);
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            if ((x + y) % 2 != 0)
            {
                std::copy(second.begin(), second.end(), image->bytes.begin() + (size_t(y) * width + x) * 4);
            }
        }
    }
    return image;
}

bool TestContinuousNormalUvEncoding()
{
    TextureSettings settings;
    settings.compressBC7 = true;
    TextureImageUse use;
    use.role             = TextureImageRole::Normal;
    use.requiresMipmaps  = false;
    use.filterNormalMips = false;
    // The actual quality-4 block changes red 255 to 254. Quarter phases see
    // <0.041 degrees, while the independent exact 2/3-phase witness is 26.525.
    std::shared_ptr<ImagePixels> missed = Solid(4, 4, {255, 128, 128, 255});
    for (uint32_t y = 0; y < 4; ++y)
    {
        for (uint32_t x = 1; x < 4; x += 2)
        {
            missed->bytes[(y * 4 + x) * 4] = 64;
        }
    }
    bool valid = true;
    for (bool repeat : {false, true})
    {
        use.normalRepeatS = use.normalRepeatT = repeat;
        const TextureImageResult result       = Process(missed, use, settings);
        valid &= Check(result.status.Succeeded() && result.pixels == missed && result.metrics.size() == 1
                           && result.reason == "BC7 bilinear UV sampling exceeds normal angular limits",
                       "The interstitial positive-Z BC7 failure retains the original image for repeat and clamp");
        if (result.metrics.size() == 1)
        {
            const TextureMipQuality& metric = result.metrics[0];
            valid &= Check(metric.normalFilteredRmsAngleDegrees < 0.03 && metric.normalFilteredMaxAngleDegrees < 0.041
                               && metric.normalUvObservedRmsAngleDegrees > 1 && !metric.normalUvContinuousCertified
                               && metric.normalUvRectangleCount > 1 && metric.normalUvRectangleCount <= 127,
                           "Original quarter metrics remain separate from the actual interstitial violation");
        }
    }
    for (bool xy : {false, true})
    {
        settings.compressBC5Normals = xy;
        settings.compressBC7        = !xy;
        for (const std::array<uint32_t, 2>& extent : {std::array<uint32_t, 2>{4, 4}, {5, 7}, {1, 4}, {4, 1}})
        {
            const std::shared_ptr<ImagePixels> smooth =
                UvNormals(extent[0], extent[1], {128, 128, 255, 255}, {140, 132, 254, 255});
            use.normalRepeatS                  = extent[0] % 2 == 0;
            use.normalRepeatT                  = extent[1] % 2 == 0;
            const TextureImageResult qualified = Process(smooth, use, settings);
            valid &= Check(qualified.status.Succeeded() && qualified.pixels != smooth && qualified.reason.empty()
                               && qualified.metrics.size() == 1,
                           "Non-flat BC5/BC7 normals retain compression for square, odd, 1D and sampler-edge cases");
            if (qualified.metrics.size() == 1)
            {
                const TextureMipQuality& metric = qualified.metrics[0];
                valid &= Check(metric.normalUvContinuousCertified && metric.normalUvRectangleCount > 0
                                   && metric.normalUvRectangleCount <= 127
                                   && metric.normalUvRmsUpperBoundDegrees >= metric.normalUvObservedRmsAngleDegrees
                                   && metric.normalUvMaxUpperBoundDegrees >= metric.normalUvObservedMaxAngleDegrees
                                   && metric.normalUvRmsUpperBoundDegrees <= settings.normalRmsAngleDegrees
                                   && metric.normalUvMaxUpperBoundDegrees <= settings.normalMaxAngleDegrees,
                               "Certified UV leaf bounds cover observations and meet unchanged angular limits");
            }
        }
    }
    return valid;
}

bool TestContinuousNormalUvSingularity()
{
    // Exact source/restored curves cross zero at a non-dyadic phase. Agreement
    // at every sampled point cannot certify a defined direction everywhere.
    const std::shared_ptr<ImagePixels> image = UvNormals(4, 4, {129, 129, 129, 129}, {124, 124, 124, 124});
    TextureSettings                    settings;
    settings.compressBC7 = true;
    TextureImageUse use;
    use.role                        = TextureImageRole::Normal;
    use.requiresMipmaps             = false;
    const TextureImageResult result = Process(image, use, settings);
    bool valid = Check(result.status.Succeeded() && result.pixels == image && result.metrics.size() == 1
                           && result.reason == "BC7 bilinear UV sampling could not certify normal angular limits",
                       "A non-dyadic undefined UV direction exhausts bounded qualification and retains authored pixels");
    if (result.metrics.size() == 1)
    {
        const TextureMipQuality& metric = result.metrics[0];
        valid &= Check(metric.normalFilteredMaxAngleDegrees == 0 && metric.normalUvObservedMaxAngleDegrees == 0
                           && !metric.normalUvContinuousCertified && metric.normalUvRectangleCount > 1
                           && metric.normalUvRectangleCount <= 127,
                       "Undefined between-sample directions cannot use the exact-equal patch shortcut");
    }
    return valid;
}

bool TestNormalUvReconstructionCusp()
{
    std::shared_ptr<ImagePixels> image = Solid(4, 4, {255, 128, 128, 255});
    image->bytes[2]                    = 143;
    TextureSettings settings;
    settings.compressBC5Normals = true;
    TextureImageUse use;
    use.role                       = TextureImageRole::Normal;
    use.requiresMipmaps            = false;
    use.filterNormals              = false;
    const TextureImageResult point = Process(image, use, settings);
    bool                     valid = Check(point.status.Succeeded() && point.pixels != image && point.reason.empty(),
                                           "BC5 cusp fixture qualifies at its individual texels");
    if (point.status.Succeeded() && point.pixels && point.pixels != image)
    {
        std::array<Vec4, 16> decoded{};
        const Status status  = DecodeBlock(PixelFormat::BC5Unorm, std::as_bytes(std::span(point.pixels->bytes)), decoded);
        double       minimum = 2, maximum = 0;
        for (const Vec4& pixel : decoded)
        {
            const double x = pixel.x * 2 - 1, y = pixel.y * 2 - 1;
            minimum = std::min(minimum, x * x + y * y);
            maximum = std::max(maximum, x * x + y * y);
        }
        valid &= Check(status.Succeeded() && minimum < 1 && maximum > 1,
                       "Actual BC5 bytes straddle the positive-Z reconstruction cusp");
    }
    use.filterNormals = true;
    CancellationPosition     complete;
    Cancellation             counter(CancelAtPosition, &complete);
    uint64_t                 budget   = 1ull << 20;
    const TextureImageResult filtered = ProcessTextureImage(image, use, settings, budget, &counter);
    valid &= Check(filtered.status.Succeeded() && filtered.pixels == image && filtered.metrics.size() == 1
                       && filtered.reason == "BC5 bilinear UV sampling could not certify normal angular limits"
                       && budget + filtered.ownedBytes == (1ull << 20),
                   "The cusp retains authored data when the conservative bounded-work qualifier cannot certify it");
    if (filtered.metrics.size() == 1)
    {
        const TextureMipQuality& metric = filtered.metrics[0];
        valid &= Check(metric.normalFilteredRmsAngleDegrees < 1 && metric.normalFilteredMaxAngleDegrees < 5
                           && metric.normalUvObservedRmsAngleDegrees < 1 && metric.normalUvObservedMaxAngleDegrees < 5
                           && !metric.normalUvContinuousCertified && metric.normalUvRectangleCount == 127,
                       "Unfinished UV leaves never publish a certificate or fabricate an observed threshold violation");
    }
    for (uint32_t stop : {complete.calls / 2, complete.calls})
    {
        CancellationPosition position{0, stop};
        Cancellation         cancel(CancelAtPosition, &position);
        budget                             = 1ull << 20;
        const TextureImageResult cancelled = ProcessTextureImage(image, use, settings, budget, &cancel);
        valid &= Check(cancelled.status.code == StatusCode::Cancelled && !cancelled.pixels && cancelled.metrics.empty()
                           && budget == (1ull << 20),
                       "Cancellation during subdivided UV work clears partial certification and refunds storage");
    }
    return valid;
}

bool TestNormalUvControl()
{
    const std::shared_ptr<ImagePixels> image = UvNormals(4, 4, {128, 128, 255, 255}, {140, 132, 254, 255});
    TextureSettings                    settings;
    settings.compressBC5Normals = true;
    TextureImageUse use;
    use.role            = TextureImageRole::Normal;
    use.requiresMipmaps = false;
    bool valid = true, limitObserved = false;
    for (uint64_t available = 4096; available < 16384 && !limitObserved; available += 32)
    {
        uint64_t                 budget = available;
        const TextureImageResult result = ProcessTextureImage(image, use, settings, budget);
        valid         &= Check(result.status.Succeeded()
                                   ? budget + result.ownedBytes == available
                                   : result.status.code == StatusCode::ResourceLimit && !result.pixels && budget == available,
                               "UV qualification failures refund temporary storage and discard candidate ownership");
        limitObserved  = result.status.message == "Normal UV quality working storage exceeds budget";
    }
    valid &= Check(limitObserved, "UV rows and fixed qualification state are charged before allocation");
    CancellationPosition     complete;
    Cancellation             counter(CancelAtPosition, &complete);
    uint64_t                 budget    = 1ull << 20;
    const TextureImageResult qualified = ProcessTextureImage(image, use, settings, budget, &counter);
    valid &= Check(qualified.status.Succeeded() && qualified.pixels != image && qualified.metrics.size() == 1
                       && qualified.metrics[0].normalUvContinuousCertified && budget + qualified.ownedBytes == (1ull << 20),
                   "UV cancellation control first qualifies successfully with balanced ownership");
    for (uint32_t stop = 1; stop <= complete.calls; ++stop)
    {
        CancellationPosition position{0, stop};
        Cancellation         cancel(CancelAtPosition, &position);
        budget                          = 1ull << 20;
        const TextureImageResult result = ProcessTextureImage(image, use, settings, budget, &cancel);
        valid &= Check(result.status.code == StatusCode::Cancelled && !result.pixels && result.metrics.empty()
                           && budget == (1ull << 20),
                       "Cancellation at every poll, including UV qualification, clears outputs and refunds storage");
    }
    return valid;
}

std::shared_ptr<ImagePixels> MipNormals(uint32_t                      width,
                                        uint32_t                      height,
                                        const std::array<uint8_t, 4>& first,
                                        const std::array<uint8_t, 4>& second)
{
    std::shared_ptr<ImagePixels> image = Solid(width, height, first);
    image->mips.push_back(image->bytes);
    while (width > 1 || height > 1)
    {
        width  = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
        image->mips.push_back(Solid(width, height, second)->bytes);
    }
    return image;
}

bool TestNormalMipEncoding()
{
    TextureImageUse use;
    use.role = TextureImageRole::Normal;
    TextureSettings settings;
    settings.compressBC5Normals = true;
    bool valid                  = true;
    for (const std::array<uint32_t, 2>& extent : {std::array<uint32_t, 2>{8, 8}, {7, 5}, {1, 8}, {8, 1}})
    {
        const std::shared_ptr<ImagePixels> image = MipNormals(extent[0], extent[1], {216, 179, 203, 255}, {39, 179, 203, 255});
        for (bool linear : {false, true})
        {
            use.filterNormals = use.filterNormalMin = linear;
            use.filterNormalMips                    = false;
            const TextureImageResult nearestMip     = Process(image, use, settings);
            valid &= Check(nearestMip.status.Succeeded() && nearestMip.pixels != image && nearestMip.reason.empty(),
                           "Independent mip samples preserve qualifying BC5 compression");
            use.filterNormalMips             = true;
            const TextureImageResult blended = Process(image, use, settings);
            valid &= Check(blended.status.Succeeded() && blended.pixels == image && blended.metrics.size() == 2
                               && blended.reason == "BC5 joint UV/LOD sampling exceeds normal angular limits",
                           "Linear mip blending rejects the authored chain for both texel filters and odd/1D extents");
            if (blended.metrics.size() == 2)
            {
                const TextureMipQuality& metric = blended.metrics[1];
                valid &= Check(metric.normalMaxAngleDegrees < 1 && metric.normalFilteredMaxAngleDegrees < 1
                                   && std::abs(metric.normalMipMaxAngleDegrees - 10.475082330) < 1e-5
                                   && std::abs(metric.normalMipRmsAngleDegrees - 10.475082330) < 1e-5,
                               "Mip blending finds the independently measured error missed by per-level gates");
            }
        }
    }
    settings.compressBC5Normals                  = false;
    settings.compressBC7                         = true;
    const std::shared_ptr<ImagePixels> opposed   = MipNormals(4, 4, {255, 127, 128, 255}, {0, 128, 127, 255});
    const TextureImageResult           undefined = Process(opposed, use, settings);
    valid &= Check(undefined.status.Succeeded() && undefined.pixels == opposed && undefined.metrics.size() == 2
                       && undefined.reason == "BC7 joint UV/LOD sampling exceeds normal angular limits"
                       && undefined.metrics[1].normalMipMaxAngleDegrees == 180,
                   "Undefined BC7 cross-mip directions preserve every authored level");
    return valid;
}

bool TestContinuousNormalMipEncoding()
{
    TextureImageUse use;
    use.role = TextureImageRole::Normal;
    TextureSettings settings;
    settings.compressBC5Normals = true;
    bool valid                  = true;
    for (const std::array<uint32_t, 2>& extent : {std::array<uint32_t, 2>{8, 8}, {7, 5}, {1, 8}, {8, 1}})
    {
        const std::shared_ptr<ImagePixels> image = MipNormals(extent[0], extent[1], {114, 204, 229, 255}, {103, 135, 252, 255});
        use.filterNormalMips                     = false;
        const TextureImageResult nearest         = Process(image, use, settings);
        valid                &= Check(nearest.status.Succeeded() && nearest.pixels != image && nearest.reason.empty(),
                                      "The between-fraction regression passes independent mip qualification");
        use.filterNormalMips  = true;
        const TextureImageResult blended = Process(image, use, settings);
        valid &= Check(blended.status.Succeeded() && blended.pixels == image && blended.metrics.size() == 2
                           && blended.reason == "BC5 joint UV/LOD sampling exceeds normal angular limits",
                       "Adaptive LOD qualification rejects the RMS violation missed by quarter fractions");
        if (blended.metrics.size() == 2)
        {
            const TextureMipQuality& metric = blended.metrics[1];
            valid &= Check(metric.normalMipRmsAngleDegrees > 1.09 && metric.normalMipRmsAngleDegrees < 1.10
                               && metric.normalMipMaxAngleDegrees < 5 && metric.normalJointBoxCount > 1
                               && metric.normalJointBoxCount <= 127 && !metric.normalJointContinuousCertified,
                           "The observed between-fraction RMS failure is distinct from the maximum-angle limit");
        }
    }
    const std::shared_ptr<ImagePixels> smooth = MipNormals(7, 5, {128, 128, 255, 255}, {140, 128, 254, 255});
    for (bool xy : {false, true})
    {
        settings.compressBC5Normals = xy;
        settings.compressBC7        = !xy;
        for (bool linear : {false, true})
        {
            use.filterNormals = use.filterNormalMin = linear;
            use.normalRepeatS                       = linear;
            use.normalRepeatT                       = !linear;
            const TextureImageResult qualified      = Process(smooth, use, settings);
            valid &= Check(qualified.status.Succeeded() && qualified.pixels != smooth && qualified.reason.empty(),
                           "Qualifying BC5 and BC7 authored chains retain compression across sampler modes");
            for (size_t level = 1; level < qualified.metrics.size(); ++level)
            {
                const TextureMipQuality& metric = qualified.metrics[level];
                valid &= Check(metric.normalJointContinuousCertified && metric.normalJointBoxCount > 0
                                   && metric.normalJointBoxCount <= 127
                                   && metric.normalMipRmsUpperBoundDegrees >= metric.normalMipRmsAngleDegrees
                                   && metric.normalMipMaxUpperBoundDegrees >= metric.normalMipMaxAngleDegrees
                                   && metric.normalMipRmsUpperBoundDegrees <= settings.normalRmsAngleDegrees
                                   && metric.normalMipMaxUpperBoundDegrees <= settings.normalMaxAngleDegrees,
                               "Accepted continuous-LOD bounds cover observations and satisfy unchanged thresholds");
            }
        }
    }
    return valid;
}

bool TestContinuousNormalMipSingularity()
{
    // Signed RGB is (3,3,3)/255 then (-7,-7,-7)/255. The common
    // affine direction is undefined at 3/10, between every finite dyadic grid.
    const std::shared_ptr<ImagePixels> image = MipNormals(4, 4, {129, 129, 129, 129}, {124, 124, 124, 124});
    TextureSettings                    settings;
    settings.compressBC7 = true;
    TextureImageUse use;
    use.role          = TextureImageRole::Normal;
    use.filterNormals = use.filterNormalMin = false;
    use.filterNormalMips                    = false;
    const TextureImageResult nearest        = Process(image, use, settings);
    bool                     valid = Check(nearest.status.Succeeded() && nearest.pixels != image && nearest.reason.empty(),
                                           "Non-dyadic singular fixture has individually qualifying BC7 mips");
    if (nearest.status.Succeeded() && nearest.pixels != image && nearest.pixels)
    {
        for (size_t level = 0; level < nearest.pixels->mips.size(); ++level)
        {
            std::array<Vec4, 16> decoded{};
            const Status         status =
                DecodeBlock(PixelFormat::BC7Unorm, std::as_bytes(std::span(nearest.pixels->mips[level])).first(16), decoded);
            const long expected = level == 0 ? 129 : 124;
            valid &= Check(status.Succeeded() && std::lround(decoded[0].x * 255) == expected
                               && std::lround(decoded[0].y * 255) == expected && std::lround(decoded[0].z * 255) == expected,
                           "The singularity regression exercises exact equal decoded RGB curves");
        }
    }
    use.filterNormalMips             = true;
    const TextureImageResult blended = Process(image, use, settings);
    valid &= Check(blended.status.Succeeded() && blended.pixels == image && blended.metrics.size() == 2
                       && blended.reason == "BC7 joint UV/LOD sampling could not certify normal angular limits",
                   "A non-dyadic zero-length direction prevents an equal-curve shortcut from certifying the interval");
    if (blended.metrics.size() == 2)
    {
        const TextureMipQuality& metric  = blended.metrics[1];
        valid                           &= Check(
            metric.normalMipRmsAngleDegrees == 0 && metric.normalMipMaxAngleDegrees == 0
                && !metric.normalJointContinuousCertified && metric.normalJointBoxCount > 1
                && metric.normalJointBoxCount <= 127,
            "Unresolved singularity retains authored pixels with a bounded-work diagnostic, not a fabricated observed error");
    }
    return valid;
}

std::shared_ptr<ImagePixels> JointNormalRegression(bool singular)
{
    const std::array<uint8_t, 4> first =
        singular ? std::array<uint8_t, 4>{255, 129, 129, 255} : std::array<uint8_t, 4>{255, 207, 128, 255};
    const std::array<uint8_t, 4> low =
        singular ? std::array<uint8_t, 4>{0, 110, 126, 254} : std::array<uint8_t, 4>{0, 0, 127, 255};
    const std::array<uint8_t, 4> high =
        singular ? std::array<uint8_t, 4>{0, 190, 126, 254} : std::array<uint8_t, 4>{0, 255, 127, 255};
    std::shared_ptr<ImagePixels> image = MipNormals(singular ? 4 : 16, singular ? 4 : 1, first, low);
    const uint32_t               width = image->width / 2, height = std::max(1u, image->height / 2);
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 1; x < width; ++x)
        {
            std::copy(high.begin(), high.end(), image->mips[1].begin() + (size_t(y) * width + x) * 4);
        }
    }
    return image;
}

bool TestJointNormalMipEncoding()
{
    TextureSettings settings;
    settings.compressBC7 = true;
    TextureImageUse use;
    use.role          = TextureImageRole::Normal;
    use.normalRepeatS = use.normalRepeatT = false;
    bool valid                            = true;
    for (bool singular : {false, true})
    {
        const std::shared_ptr<ImagePixels>      image    = JointNormalRegression(singular);
        const std::vector<std::vector<uint8_t>> original = image->mips;
        use.filterNormalMips                             = false;
        const TextureImageResult independent             = Process(image, use, settings);
        valid &= Check(independent.status.Succeeded() && independent.pixels != image && independent.reason.empty(),
                       "Joint regressions qualify independently at every authored mip");
        for (const TextureMipQuality& metric : independent.metrics)
        {
            valid &= Check(metric.normalUvContinuousCertified, "Joint regression has certified within-level UV quality");
        }
        if (!singular && independent.pixels && independent.pixels != image)
        {
            std::array<Vec4, 16> decoded{};
            const Status         status =
                DecodeBlock(PixelFormat::BC7Unorm, std::as_bytes(std::span(independent.pixels->mips[1])).first(16), decoded);
            valid &= Check(status.Succeeded() && std::lround(decoded[0].x * 255) == 0 && std::lround(decoded[0].y * 255) == 0
                               && std::lround(decoded[0].z * 255) == 128 && std::lround(decoded[1].x * 255) == 1
                               && std::lround(decoded[1].y * 255) == 255 && std::lround(decoded[1].z * 255) == 127,
                           "Actual BC7 coarse bytes reproduce the independently verified joint-domain defect");
            std::array<Vec4, 16> fineDecoded{};
            const Status fineStatus = DecodeBlock(PixelFormat::BC7Unorm,
                                                  std::as_bytes(std::span(independent.pixels->mips[0])).first(16), fineDecoded);
            const double u = 0.08603941176470588, fraction = 8 * u - 0.5;
            const std::array<double, 3> fine{std::round(fineDecoded[0].x * 255), std::round(fineDecoded[0].y * 255),
                                             std::round(fineDecoded[0].z * 255)};
            const std::array<double, 3> low{std::round(decoded[0].x * 255), std::round(decoded[0].y * 255),
                                            std::round(decoded[0].z * 255)};
            const std::array<double, 3> high{std::round(decoded[1].x * 255), std::round(decoded[1].y * 255),
                                             std::round(decoded[1].z * 255)};
            std::array<double, 3>       restored{};
            double                      squared = 0;
            for (size_t channel = 0; channel < 3; ++channel)
            {
                // At LOD .5, decode to signed channels after the two filters.
                restored[channel]  = (fine[channel] + (1 - fraction) * low[channel] + fraction * high[channel]) / 255 - 1;
                squared           += restored[channel] * restored[channel];
            }
            // The authored direction is exactly +Y, with nonzero length 8e-5.
            const double angle = std::acos(restored[1] / std::sqrt(squared)) * 180 / 3.14159265358979323846;
            valid &=
                Check(fineStatus.Succeeded() && std::abs(fraction - 48.0 / 255.0 - 0.00008) < 1e-12 && angle > 88 && angle < 89,
                      "The actual compressed chain has a defined off-grid joint error above 88 degrees");
        }
        use.filterNormalMips            = true;
        const TextureImageResult result = Process(image, use, settings);
        valid &= Check(result.status.Succeeded() && result.pixels == image && result.metrics.size() == 2
                           && image->mips == original && result.pixelBytes == 0,
                       "Joint qualification retains the entire original authored chain on failure");
        if (result.metrics.size() == 2)
        {
            const TextureMipQuality& metric  = result.metrics[1];
            valid                           &= Check(!metric.normalJointContinuousCertified && metric.normalJointBoxCount > 1
                                                         && metric.normalJointBoxCount <= 127,
                                                     "A partially visited joint domain never publishes a certificate");
            if (singular)
            {
                valid &= Check(result.reason == "BC7 joint UV/LOD sampling could not certify normal angular limits"
                                   && metric.normalMipMaxAngleDegrees < 1e-4,
                               "An exact-encoding interstitial joint singularity remains an unresolved bound");
            }
            else
            {
                // The frozen predecessor accepted this chain; independently decoded
                // bytes give 88.5975 degrees at U=.0860394, V=.5, LOD=.5.
                const bool observed = metric.normalMipRmsAngleDegrees > 1 || metric.normalMipMaxAngleDegrees > 5;
                valid &= Check(observed ? result.reason == "BC7 joint UV/LOD sampling exceeds normal angular limits"
                                        : result.reason == "BC7 joint UV/LOD sampling could not certify normal angular limits",
                               "Joint rejection distinguishes an observed violation from exhausted conservative bounds");
            }
        }
    }
    return valid;
}

std::shared_ptr<ImagePixels> SmoothJointNormals(uint32_t width, uint32_t height)
{
    const std::array<uint8_t, 4> a{128, 128, 255, 255}, b{140, 132, 254, 255};
    std::shared_ptr<ImagePixels> image = UvNormals(width, height, a, b);
    image->mips.push_back(image->bytes);
    while (width > 1 || height > 1)
    {
        width          = std::max(1u, width / 2);
        height         = std::max(1u, height / 2);
        const bool odd = (image->mips.size() % 2) != 0;
        image->mips.push_back(UvNormals(width, height, odd ? b : a, odd ? a : b)->bytes);
    }
    return image;
}

bool TestJointNormalSamplerCoverage()
{
    bool            valid = true;
    TextureImageUse use;
    use.role = TextureImageRole::Normal;
    TextureSettings settings;
    for (bool xy : {false, true})
    {
        settings.compressBC5Normals = xy;
        settings.compressBC7        = !xy;
        // 7->3 has separate nearest cuts at 1/2 and 5/6. 5->2 reaches
        // a new nearest state exactly at phase 1; singleton axes stay 1->1.
        for (const std::array<uint32_t, 2>& extent :
             {std::array<uint32_t, 2>{7, 5}, {5, 7}, {7, 1}, {1, 7}, {5, 1}, {1, 5}, {4, 4}})
        {
            const std::shared_ptr<ImagePixels> image = SmoothJointNormals(extent[0], extent[1]);
            for (bool linear : {false, true})
            {
                use.filterNormals = use.filterNormalMin = linear;
                for (bool repeat : {false, true})
                {
                    use.normalRepeatS               = repeat;
                    use.normalRepeatT               = !repeat;
                    const TextureImageResult result = Process(image, use, settings);
                    valid &= Check(result.status.Succeeded() && result.pixels != image && result.reason.empty()
                                       && result.metrics.size() == image->mips.size(),
                                   "Spatially varying BC5/BC7 chains remain compressed across odd/1D sampler modes");
                    for (size_t level = 1; level < result.metrics.size(); ++level)
                    {
                        const TextureMipQuality& metric = result.metrics[level];
                        valid &= Check(metric.normalJointContinuousCertified && metric.normalJointBoxCount > 0
                                           && metric.normalJointBoxCount <= 127
                                           && metric.normalMipRmsUpperBoundDegrees >= metric.normalMipRmsAngleDegrees
                                           && metric.normalMipMaxUpperBoundDegrees >= metric.normalMipMaxAngleDegrees
                                           && metric.normalMipRmsUpperBoundDegrees <= settings.normalRmsAngleDegrees
                                           && metric.normalMipMaxUpperBoundDegrees <= settings.normalMaxAngleDegrees,
                                       "Every accepted pair has a complete joint certificate within the original limits");
                    }
                }
            }
        }
    }
    const std::shared_ptr<ImagePixels> image = SmoothJointNormals(5, 1);
    use.filterNormals = use.filterNormalMin = true;
    CancellationPosition     complete;
    Cancellation             counter(CancelAtPosition, &complete);
    uint64_t                 budget    = 1ull << 20;
    const TextureImageResult qualified = ProcessTextureImage(image, use, settings, budget, &counter);
    valid &= Check(qualified.status.Succeeded() && qualified.pixels != image && qualified.metrics.size() == 3
                       && qualified.metrics[1].normalJointContinuousCertified,
                   "Linear odd-mip cancellation control first completes joint qualification");
    for (uint32_t stop = 1; stop <= complete.calls; ++stop)
    {
        CancellationPosition position{0, stop};
        Cancellation         cancel(CancelAtPosition, &position);
        budget                          = 1ull << 20;
        const TextureImageResult result = ProcessTextureImage(image, use, settings, budget, &cancel);
        valid &= Check(result.status.code == StatusCode::Cancelled && !result.pixels && result.metrics.empty()
                           && budget == (1ull << 20),
                       "Cancellation at every linear joint poll discards output and refunds every temporary charge");
    }
    return valid;
}

bool TestNormalMipControl()
{
    const std::shared_ptr<ImagePixels> flat = Solid(64, 1, {128, 128, 255, 255});
    TextureSettings                    settings;
    settings.compressBC5Normals = true;
    TextureImageUse use;
    use.role          = TextureImageRole::Normal;
    use.filterNormals = use.filterNormalMin = false;
    bool valid = true, limitObserved = false;
    for (uint64_t available = 4096; available < 16384 && !limitObserved; available += 64)
    {
        uint64_t                 budget = available;
        const TextureImageResult result = ProcessTextureImage(flat, use, settings, budget);
        valid         &= Check(result.status.Succeeded()
                                   ? budget + result.ownedBytes == available
                                   : result.status.code == StatusCode::ResourceLimit && !result.pixels && budget == available,
                               "Mip quality scratch is refunded on every allocation outcome");
        limitObserved  = result.status.message == "Joint normal mip quality working storage exceeds budget";
    }
    valid                          &= Check(limitObserved, "Adjacent-mip decoded rows are charged before allocation");
    settings.normalRmsAngleDegrees  = 1.2f;
    const std::shared_ptr<ImagePixels> adaptive = MipNormals(2, 2, {114, 204, 229, 255}, {103, 135, 252, 255});
    for (const std::shared_ptr<ImagePixels>& image : {flat, adaptive})
    {
        CancellationPosition     complete;
        Cancellation             counter(CancelAtPosition, &complete);
        uint64_t                 budget  = 1ull << 20;
        const TextureImageResult encoded = ProcessTextureImage(image, use, settings, budget, &counter);
        valid &= Check(encoded.status.Succeeded() && encoded.pixels != image && budget + encoded.ownedBytes == (1ull << 20),
                       "Qualifying mip chains remain compressed with bounded accounting");
        if (image == adaptive)
        {
            valid &= Check(encoded.metrics.size() == 2 && encoded.metrics[1].normalJointContinuousCertified
                               && encoded.metrics[1].normalJointBoxCount > 1,
                           "Cancellation fixture exercises interval subdivision before certification");
        }
        for (uint32_t stop = 1; stop <= complete.calls; ++stop)
        {
            CancellationPosition position{0, stop};
            Cancellation         cancel(CancelAtPosition, &position);
            budget                          = 1ull << 20;
            const TextureImageResult result = ProcessTextureImage(image, use, settings, budget, &cancel);
            valid &= Check(result.status.code == StatusCode::Cancelled && !result.pixels && result.metrics.empty()
                               && budget == (1ull << 20),
                           "Cancellation during root or subdivided LOD intervals discards the candidate and refunds storage");
        }
    }
    return valid;
}

bool TestOcclusionEncoding()
{
    TextureSettings settings;
    settings.compressBC4Occlusion = true;
    settings.scalarMaxError       = 0;
    TextureImageUse use;
    use.role                           = TextureImageRole::Occlusion;
    std::shared_ptr<ImagePixels> image = Solid(5, 7, {128, 0, 0, 0});
    for (size_t offset = 0; offset < image->bytes.size(); offset += 4)
    {
        image->bytes[offset + 1] = static_cast<uint8_t>(offset * 71);
        image->bytes[offset + 2] = static_cast<uint8_t>(offset * 13);
        image->bytes[offset + 3] = static_cast<uint8_t>(offset * 37);
    }
    const TextureImageResult encoded = Process(image, use, settings);
    bool valid = Check(encoded.status.Succeeded() && encoded.pixels != image && encoded.reason.empty()
                           && encoded.pixels->format == PixelFormat::BC4Unorm && encoded.pixels->bytes.size() == 32
                           && encoded.pixels->mips.back().size() == 8,
                       "BC4 consumes only R even with arbitrary GBA and a zero scalar tolerance");
    for (const TextureMipQuality& quality : encoded.metrics)
    {
        valid &= Check(quality.scalarMaxError == 0, "BC4 consumed-R quality is measured per mip");
    }
    use.role                      = TextureImageRole::Data;
    const TextureImageResult data = Process(image, use, settings);
    valid    &= Check(data.status.Succeeded() && data.pixels == image && data.pixels->bytes == image->bytes,
                      "BC4-only policy never discards GBA of general packed data");
    use.role  = TextureImageRole::Occlusion;
    const TextureImageResult repeat = Process(image, use, settings);
    valid &= Check(repeat.status.Succeeded() && repeat.pixels->mips == encoded.pixels->mips, "BC4 output is deterministic");
    for (size_t offset = 0; offset < image->bytes.size(); offset += 4)
    {
        image->bytes[offset] = static_cast<uint8_t>((offset * 113 + offset * offset * 7) & 255);
    }
    settings.compressBC7              = true;
    const TextureImageResult rejected = Process(image, use, settings);
    valid                         &= Check(rejected.status.Succeeded() && rejected.pixels == image && !rejected.metrics.empty()
                                               && rejected.metrics[0].scalarMaxError > 0,
                                           "BC4 rejection retains original instead of falling through to BC7");
    settings.compressBC7           = false;
    use.srgb                       = true;
    const TextureImageResult srgb  = Process(image, use, settings);
    valid    &= Check(srgb.status.Succeeded() && srgb.pixels == image, "BC4 is never selected for an sRGB occlusion view");
    use.srgb  = false;
    uint64_t                 budget  = 0;
    const TextureImageResult limited = ProcessTextureImage(image, use, settings, budget);
    valid                          &= Check(limited.status.code == StatusCode::ResourceLimit && !limited.pixels && budget == 0,
                                            "BC4 working budget rejects without leaks");
    settings.compressBC4Occlusion   = false;
    settings.compressBC5Normals     = true;
    use.role                        = TextureImageRole::Normal;
    image                           = Solid(32, 32, {128, 128, 255, 0});
    budget                          = 1ull << 20;
    uint32_t                 calls  = 0;
    Cancellation             cancellation(Poll, &calls);
    const TextureImageResult cancelled = ProcessTextureImage(image, use, settings, budget, &cancellation);
    valid &= Check(cancelled.status.code == StatusCode::Cancelled && !cancelled.pixels && budget == (1ull << 20),
                   "BC5 validation/mip traversal observes cancellation and refunds temporary storage");
    return valid;
}
} // namespace

int main()
{
    bool valid = TestMips();
    valid      = TestEncoding() && valid;
    valid      = TestFallbacks() && valid;
    valid      = TestControl() && valid;
    valid      = TestAccumulatedCoverage() && valid;
    valid      = TestRoleGates() && valid;
    valid      = TestNormalXYEncoding() && valid;
    valid      = TestFilteredNormalEncoding() && valid;
    valid      = TestContinuousNormalUvEncoding() && valid;
    valid      = TestContinuousNormalUvSingularity() && valid;
    valid      = TestNormalUvReconstructionCusp() && valid;
    valid      = TestNormalUvControl() && valid;
    valid      = TestNormalMipEncoding() && valid;
    valid      = TestContinuousNormalMipEncoding() && valid;
    valid      = TestContinuousNormalMipSingularity() && valid;
    valid      = TestJointNormalMipEncoding() && valid;
    valid      = TestJointNormalSamplerCoverage() && valid;
    valid      = TestNormalMipControl() && valid;
    valid      = TestOcclusionEncoding() && valid;
    std::printf("Texture image cooking fixtures: %s\n", valid ? "PASS" : "FAIL");
    return valid ? 0 : 1;
}
