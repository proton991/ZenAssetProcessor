#include "ZenAsset/Format/Processing.h"
#include "Format/AssetKernels.h"
#include <bit>
#include <cmath>

namespace zenasset
{
namespace
{
void HashField(kernels::HashState& hash, uint32_t value)
{
    const uint8_t bytes[4]{static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value >> 16),
                           static_cast<uint8_t>(value >> 24)};
    kernels::UpdateHash(hash, bytes, sizeof(bytes));
}

bool WithinRange(float value, float maximum)
{
    return std::isfinite(value) && value >= 0.0f && value <= maximum;
}
} // namespace

Status ValidateProcessingSettings(const ProcessingSettings& settings)
{
    Status                  status;
    const GeometrySettings& geometry = settings.geometry;
    if (geometry.positionQuantizationBits > 24 || !std::isfinite(geometry.positionMaxRelativeError)
        || geometry.positionMaxRelativeError < 0 || !std::isfinite(geometry.positionMaxAbsoluteError)
        || geometry.positionMaxAbsoluteError < 0)
    {
        status = {StatusCode::InvalidArgument, "Geometry quantization requires 0..24 bits and finite nonnegative error bounds"};
    }
    const TextureSettings& textures = settings.textures;
    if (status.Succeeded()
        && (textures.bc7Quality > 4 || textures.maxDimension == 0 || !std::isfinite(textures.colorMinPsnr)
            || textures.colorMinPsnr < 0.0f || !WithinRange(textures.colorMaxRmse, 1.0f)
            || !WithinRange(textures.scalarMaxError, 1.0f) || !WithinRange(textures.normalRmsAngleDegrees, 180.0f)
            || !WithinRange(textures.normalMaxAngleDegrees, 180.0f)
            || textures.normalRmsAngleDegrees > textures.normalMaxAngleDegrees
            || !WithinRange(textures.alphaCoverageMaxError, 1.0f) || !WithinRange(textures.hdrMaxRelativeRmse, 1.0f)))
    {
        status = {StatusCode::InvalidArgument,
                  "Texture processing requires quality 0..4, a positive dimension limit and finite valid error bounds"};
    }
    if (status.Succeeded()
        && (textures.arrayMaxDimension == 0 || textures.arrayMaxDimension > 16384 || textures.arrayMaxLayers < 2
            || textures.arrayMaxLayers > 2048 || textures.arrayMaxPages == 0 || textures.arrayMaxImages == 0))
    {
        status = {StatusCode::InvalidArgument,
                  "Array pages require dimensions 1..16384, layers 2..2048 and positive count limits"};
    }
    if (status.Succeeded()
        && (textures.spatialMaxDimension == 0 || textures.spatialMaxDimension > 16384 || textures.spatialMaxPages == 0
            || textures.spatialMaxImages == 0
            || (textures.spatialPages
                && (!std::has_single_bit(textures.spatialMaxDimension) || textures.spatialMaxDimension < 16
                    || textures.arrayPages))))
    {
        status = {
            StatusCode::InvalidArgument,
            "Spatial pages require positive count limits and dimensions 1..16384; enabled pages require a power-of-two dimension of at least 16 and array pages disabled"};
    }
    return status;
}

bool HasProcessingOperations(const ProcessingSettings& settings)
{
    return settings.geometry.optimizeVertexFetch || settings.geometry.positionQuantizationBits != 0
        || settings.textures.generateMipmaps || settings.textures.compressBC7 || settings.textures.compressBC5Normals
        || settings.textures.compressBC4Occlusion || settings.textures.compressBC6HEnvironment || settings.textures.arrayPages
        || settings.textures.spatialPages;
}

uint64_t ProcessingSettingsHash(const ProcessingSettings& settings)
{
    kernels::HashState hash{};
    kernels::ResetHash(hash, 0);
    HashField(hash, 4); // Processing identity layout version.
    HashField(hash, settings.geometry.optimizeVertexFetch ? 1u : 0u);
    HashField(hash, settings.geometry.positionQuantizationBits);
    HashField(hash, std::bit_cast<uint32_t>(settings.geometry.positionMaxRelativeError));
    HashField(hash, std::bit_cast<uint32_t>(settings.geometry.positionMaxAbsoluteError));
    HashField(hash, settings.textures.generateMipmaps ? 1u : 0u);
    HashField(hash, settings.textures.compressBC7 ? 1u : 0u);
    HashField(hash, settings.textures.bc7Quality);
    HashField(hash, settings.textures.maxDimension);
    HashField(hash, std::bit_cast<uint32_t>(settings.textures.colorMinPsnr));
    HashField(hash, std::bit_cast<uint32_t>(settings.textures.colorMaxRmse));
    HashField(hash, std::bit_cast<uint32_t>(settings.textures.scalarMaxError));
    HashField(hash, std::bit_cast<uint32_t>(settings.textures.normalRmsAngleDegrees));
    HashField(hash, std::bit_cast<uint32_t>(settings.textures.normalMaxAngleDegrees));
    HashField(hash, std::bit_cast<uint32_t>(settings.textures.alphaCoverageMaxError));
    HashField(hash, settings.textures.compressBC5Normals ? 1u : 0u);
    HashField(hash, settings.textures.compressBC4Occlusion ? 1u : 0u);
    HashField(hash, settings.textures.compressBC6HEnvironment ? 1u : 0u);
    HashField(hash, std::bit_cast<uint32_t>(settings.textures.hdrMaxRelativeRmse));
    HashField(hash, settings.textures.arrayPages ? 1u : 0u);
    HashField(hash, settings.textures.arrayMaxDimension);
    HashField(hash, settings.textures.arrayMaxLayers);
    HashField(hash, settings.textures.arrayMaxPages);
    HashField(hash, settings.textures.arrayMaxImages);
    HashField(hash, settings.textures.spatialPages ? 1u : 0u);
    HashField(hash, settings.textures.spatialMaxDimension);
    HashField(hash, settings.textures.spatialMaxPages);
    HashField(hash, settings.textures.spatialMaxImages);
    return kernels::FinishHash(hash);
}
} // namespace zenasset
