#pragma once
#include "ZenAsset/Format/Status.h"
#include <cstdint>

namespace zenasset
{
struct GeometrySettings
{
    // Reorder storage by first index use without changing primitive or corner order.
    bool optimizeVertexFetch{false};
    // Zero preserves positions; otherwise use a 1..24 bit grid with measured
    // error checks. Unsupported deformation/overlap cases retain original data.
    uint32_t positionQuantizationBits{0};
    // Maximum Euclidean error is max(absolute, relative * instance bounds diagonal).
    float positionMaxRelativeError{0.00001f};
    float positionMaxAbsoluteError{0.000001f};
    bool  operator==(const GeometrySettings&) const = default;
};

struct TextureSettings
{
    // Opt-in: the reference path preserves the authored image payloads.
    bool     generateMipmaps{false};
    bool     compressBC7{false};
    uint32_t bc7Quality{4};
    // Oversized images retain their authored representation; no implicit resize.
    uint32_t maxDimension{16384};
    float    colorMinPsnr{40.0f};
    float    colorMaxRmse{0.01f};
    float    scalarMaxError{8.0f / 255.0f};
    float    normalRmsAngleDegrees{1.0f};
    float    normalMaxAngleDegrees{5.0f};
    float    alphaCoverageMaxError{0.01f};
    bool     compressBC5Normals{false};
    bool     compressBC4Occlusion{false};
    bool     compressBC6HEnvironment{false};
    float    hdrMaxRelativeRmse{0.02f};
    // Optional native array pages for qualified, finalized compatible images.
    // Disabled by default; image-count reduction does not imply a VRAM saving.
    bool     arrayPages{false};
    uint32_t arrayMaxDimension{2048};
    uint32_t arrayMaxLayers{256};
    uint32_t arrayMaxPages{64};
    uint32_t arrayMaxImages{8192};
    // Optional spatial pages: single-level, power-of-two RGBA8 tiles with
    // four-texel gutters. Requires a runtime that understands Texture v4 regions.
    // Mutually exclusive with arrayPages; authored mip chains are never truncated.
    bool     spatialPages{false};
    uint32_t spatialMaxDimension{2048};
    uint32_t spatialMaxPages{64};
    uint32_t spatialMaxImages{8192};
    bool     operator==(const TextureSettings&) const = default;
};

struct ProcessingSettings
{
    GeometrySettings geometry;
    TextureSettings  textures;
    bool             operator==(const ProcessingSettings&) const = default;
};

Status ValidateProcessingSettings(const ProcessingSettings& settings);
bool   HasProcessingOperations(const ProcessingSettings& settings);
// Stable field-wise identity, independent of native structure layout and padding.
// Includes disabled-option values so every explicit setting participates in caches.
uint64_t ProcessingSettingsHash(const ProcessingSettings& settings);
} // namespace zenasset
