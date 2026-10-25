#include "Processing/HdrCube.h"
#include "Codecs/BlockDecoding.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>

namespace
{
using namespace zenasset;
using namespace zenasset::detail;
using Faces = std::vector<std::vector<Vec4>>;

bool Check(bool valid, const char* message)
{
    if (!valid)
    {
        std::printf("FAIL: %s\n", message);
    }
    return valid;
}

Faces Solid(uint32_t size, uint32_t mips, Vec4 color)
{
    Faces faces(size_t(mips) * 6);
    for (size_t face = 0; face < faces.size(); ++face)
    {
        const uint32_t extent = std::max(1u, size >> (face / 6));
        faces[face].resize(size_t(extent) * extent, color);
    }
    return faces;
}

HdrCubeSettings Settings()
{
    HdrCubeSettings settings;
    settings.compressBC6H = true;
    return settings;
}

HdrCubeResult Process(const HdrCubeView& input, const HdrCubeSettings& settings, uint64_t allowance = 1ull << 20)
{
    uint64_t      remaining   = allowance;
    HdrCubeResult result      = ProcessHdrCube(input, settings, remaining);
    const bool    budgetValid = result.status.Succeeded() ? remaining + result.ownedBytes == allowance
                                                          : remaining == allowance && result.ownedBytes == 0;
    if (!budgetValid)
    {
        result.status = {StatusCode::InvalidData, "HDR cube budget accounting mismatch"};
    }
    return result;
}

bool CheckDecodedFace(const std::vector<Vec4>&    source,
                      const std::vector<uint8_t>& encoded,
                      uint32_t                    extent,
                      const HdrCubeFaceQuality&   measured)
{
    bool                 valid        = true;
    const uint32_t       blocks       = (extent + 3) / 4;
    double               squaredError = 0;
    double               sourceEnergy = 0;
    double               maximum      = 0;
    std::array<Vec4, 16> block;
    for (uint32_t by = 0; by < blocks; ++by)
    {
        for (uint32_t bx = 0; bx < blocks; ++bx)
        {
            const size_t offset = (size_t(by) * blocks + bx) * 16;
            valid &=
                DecodeBlock(PixelFormat::BC6HUfloat, {reinterpret_cast<const std::byte*>(encoded.data() + offset), 16}, block)
                    .Succeeded();
            for (uint32_t y = 0; y < 4 && by * 4 + y < extent; ++y)
            {
                for (uint32_t x = 0; x < 4 && bx * 4 + x < extent; ++x)
                {
                    const Vec4& input = source[size_t(by * 4 + y) * extent + bx * 4 + x];
                    for (uint32_t channel = 0; channel < 3; ++channel)
                    {
                        const double difference  = double(block[y * 4 + x][channel]) - input[channel];
                        squaredError            += difference * difference;
                        sourceEnergy            += double(input[channel]) * input[channel];
                        maximum                  = std::max(maximum, std::abs(difference));
                    }
                }
            }
        }
    }
    const double relative = sourceEnergy == 0 ? (squaredError == 0 ? 0 : std::numeric_limits<double>::infinity())
                                              : std::sqrt(squaredError / sourceEnergy);
    valid &= Check(std::abs(relative - measured.relativeRmse) < 1e-12 && std::abs(maximum - measured.maxAbsoluteError) < 1e-12,
                   "face/mip quality matches independent decode of valid texels");
    return valid;
}

bool TestAuthoredMipsAndOwnership()
{
    bool valid = true;
    for (uint32_t size : {1u, 3u, 5u, 7u})
    {
        const uint32_t mips  = size == 1 ? 1 : 2;
        Faces          faces = Solid(size, mips, {4, 2, 16, 1});
        for (size_t face = 0; face < faces.size(); ++face)
        {
            for (Vec4& pixel : faces[face])
            {
                pixel.x += float(face);
            }
        }
        const Faces         original = faces;
        const HdrCubeView   view{size, mips, faces};
        const HdrCubeResult result   = Process(view, Settings());
        const HdrCubeResult repeated = Process(view, Settings());
        valid &= Check(result.status.Succeeded() && result.pixels && result.reason.empty()
                           && result.pixels->format == PixelFormat::BC6HUfloat && result.pixels->size == size
                           && result.pixels->mipLevels == mips && result.pixels->blockMipFaces.size() == faces.size()
                           && result.metrics.size() == faces.size() && faces == original && result.pixelBytes > 0
                           && result.ownedBytes > result.pixelBytes,
                       "tiny/odd cube preserves authored mip count, face order and source pixels");
        valid &= Check(result.pixels && repeated.pixels && result.pixels->blockMipFaces == repeated.pixels->blockMipFaces,
                       "BC6H cube encoding is deterministic");
        if (result.pixels)
        {
            for (size_t face = 0; face < faces.size(); ++face)
            {
                const uint32_t extent  = std::max(1u, size >> (face / 6));
                uint64_t       bytes   = 0;
                valid                 &= GetImageLevelByteSize(PixelFormat::BC6HUfloat, extent, extent, bytes);
                valid &= Check(result.pixels->blockMipFaces[face].size() == bytes && result.metrics[face].mipLevel == face / 6
                                   && result.metrics[face].face == face % 6 && result.metrics[face].size == extent
                                   && result.metrics[face].relativeRmse <= 0.02,
                               "each authored face has a complete checked BC6H payload and quality metric");
                valid &= CheckDecodedFace(faces[face], result.pixels->blockMipFaces[face], extent, result.metrics[face]);
            }
            faces.clear();
            valid &= CheckDecodedFace(original[0], result.pixels->blockMipFaces[0], size, result.metrics[0]);
        }
    }
    return valid;
}

bool TestQualityAndEligibility()
{
    bool                valid     = true;
    Faces               black     = Solid(4, 3, {0, 0, 0, 1});
    const HdrCubeResult zero      = Process({4, 3, black}, Settings());
    valid                        &= Check(zero.pixels && zero.metrics.size() == 18 && zero.metrics.back().relativeRmse == 0,
                                          "zero-energy faces require exactly zero decoded RGB");
    black.back()[0]               = {1e-10f, 1e-10f, 1e-10f, 1};
    const Faces         original  = black;
    const HdrCubeResult dark      = Process({4, 3, black}, Settings());
    valid &= Check(dark.status.Succeeded() && !dark.pixels && dark.pixelBytes == 0 && !dark.reason.empty()
                       && dark.metrics.size() == 18 && dark.metrics.back().relativeRmse > 0.99 && black == original,
                   "one dark tiny mip failure retains the whole original cube without an epsilon allowance");
    const HdrCubeResult maximum  = Process({1, 1, Solid(1, 1, {65504, 65504, 65504, 1})}, Settings());
    valid                       &= Check(maximum.pixels != nullptr, "maximum finite half-float RGB is eligible");
    for (uint32_t mode = 0; mode < 4; ++mode)
    {
        Faces           faces    = Solid(3, 1, {4, 2, 1, 1});
        HdrCubeSettings settings = Settings();
        if (mode == 0)
        {
            faces[5].back().x = -1;
        }
        else if (mode == 1)
        {
            faces[5].back().z = 65505;
        }
        else if (mode == 2)
        {
            faces[5].back().w = 0.5f;
        }
        else
        {
            settings.maxDimension = 2;
        }
        const Faces         saved  = faces;
        const HdrCubeResult result = Process({3, 1, faces}, settings);
        valid &= Check(result.status.Succeeded() && !result.pixels && result.metrics.empty() && result.pixelBytes == 0
                           && result.ownedBytes == result.reason.size() + 1 && !result.reason.empty() && faces == saved,
                       "ineligible range/alpha/extent preserves the complete original cube with a reason");
    }
    return valid;
}

struct CancelState
{
    uint32_t polls{0};
    uint32_t stop{0};
};

bool CancelAfter(void* user)
{
    CancelState& state = *static_cast<CancelState*>(user);
    ++state.polls;
    return state.stop != 0 && state.polls >= state.stop;
}

bool TestBudgetsAndCancellation()
{
    Faces               faces = Solid(5, 2, {4, 2, 1, 1});
    const HdrCubeView   view{5, 2, faces};
    const HdrCubeResult disabled = Process(view, {}, 0);
    bool valid = Check(disabled.status.Succeeded() && !disabled.pixels && disabled.ownedBytes == 0 && disabled.reason.empty(),
                       "disabled cooking retains the original with zero allocation");
    faces[0][0].x                    = std::numeric_limits<float>::quiet_NaN();
    const HdrCubeResult unchecked    = Process(view, {}, 0);
    valid                           &= Check(unchecked.status.Succeeded() && !unchecked.pixels && unchecked.ownedBytes == 0,
                                             "disabled cooking leaves pixel validation to the owning scene validator");
    faces[0][0].x                    = 4;
    const HdrCubeResult baseline     = Process(view, Settings());
    const uint64_t      exactBudget  = baseline.ownedBytes + 2 * sizeof(std::array<Vec4, 16>);
    const HdrCubeResult exact        = Process(view, Settings(), exactBudget);
    valid &= Check(exact.status.Succeeded() && exact.pixels, "exact output plus fixed working budget is accepted");
    for (uint64_t allowance : {uint64_t(0), exactBudget - 1})
    {
        const HdrCubeResult limited = Process(view, Settings(), allowance);
        valid &= Check(limited.status.code == StatusCode::ResourceLimit && !limited.pixels && limited.metrics.empty()
                           && limited.reason.empty() && limited.ownedBytes == 0,
                       "allocation failure clears output and restores all charges");
    }
    CancelState         state;
    Cancellation        cancellation(CancelAfter, &state);
    uint64_t            remaining  = 1ull << 20;
    const HdrCubeResult counted    = ProcessHdrCube(view, Settings(), remaining, &cancellation);
    valid                         &= Check(counted.pixels != nullptr, "count cancellation checkpoints in a complete cook");
    const uint32_t lastPoll        = state.polls;
    for (uint32_t stop : {1u, 5u, 45u, lastPoll})
    {
        state                         = {0, stop};
        remaining                     = 1ull << 20;
        const HdrCubeResult cancelled = ProcessHdrCube(view, Settings(), remaining, &cancellation);
        valid &= Check(cancelled.status.code == StatusCode::Cancelled && !cancelled.pixels && cancelled.metrics.empty()
                           && cancelled.reason.empty() && cancelled.ownedBytes == 0 && remaining == (1ull << 20),
                       "initial, row, block and final cancellation cannot publish partial output");
    }
    return valid;
}

bool TestInvalidInput()
{
    bool valid = true;
    for (uint32_t mode = 0; mode < 9; ++mode)
    {
        Faces           faces = Solid(1, 1, {4, 2, 1, 1});
        HdrCubeView     view{1, 1, faces};
        HdrCubeSettings settings = Settings();
        if (mode == 0)
        {
            view.size = 0;
        }
        else if (mode == 1)
        {
            view.mipLevels = 2;
        }
        else if (mode == 2)
        {
            view.mipFaces = view.mipFaces.first(5);
        }
        else if (mode == 3)
        {
            faces.back().clear();
        }
        else if (mode == 4)
        {
            faces.back()[0].z = std::numeric_limits<float>::infinity();
        }
        else if (mode == 5)
        {
            faces[0][0].x     = -1;
            faces.back()[0].w = std::numeric_limits<float>::quiet_NaN();
        }
        else if (mode == 6)
        {
            view.size = UINT32_MAX;
        }
        else if (mode == 7)
        {
            settings.maxDimension = 0;
        }
        else
        {
            settings.maxRelativeRmse = std::numeric_limits<float>::quiet_NaN();
        }
        const HdrCubeResult result = Process(view, settings);
        valid &= Check(result.status.code == (mode >= 7 ? StatusCode::InvalidArgument : StatusCode::InvalidData)
                           && !result.pixels && result.ownedBytes == 0 && result.metrics.empty() && result.reason.empty(),
                       "malformed/nonfinite input or invalid settings cannot become fallback output");
    }
    return valid;
}
} // namespace

int main()
{
    bool valid  = TestAuthoredMipsAndOwnership();
    valid      &= TestQualityAndEligibility();
    valid      &= TestBudgetsAndCancellation();
    valid      &= TestInvalidInput();
    return valid ? 0 : 1;
}
