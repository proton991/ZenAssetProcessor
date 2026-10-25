#include "Processing/EnvironmentProcessing.h"
#include "Processing/Diagnostics.h"
#include "Processing/HdrCube.h"
#include <cstdio>

namespace zenasset::detail
{
Status ProcessEnvironments(Scene&                   scene,
                           const TextureSettings&   settings,
                           uint64_t&                remaining,
                           std::vector<Diagnostic>& diagnostics,
                           const Cancellation*      cancellation)
{
    Status          status;
    HdrCubeSettings policy;
    policy.compressBC6H    = settings.compressBC6HEnvironment;
    policy.maxDimension    = settings.maxDimension;
    policy.maxRelativeRmse = settings.hdrMaxRelativeRmse;
    for (size_t index = 0; status.Succeeded() && policy.compressBC6H && index < scene.imageBasedLights.size(); ++index)
    {
        if (cancellation != nullptr && cancellation->IsCancelled())
        {
            status = {StatusCode::Cancelled, "Environment processing cancelled"};
        }
        ImageBasedLightAsset& light = scene.imageBasedLights[index];
        if (status.Succeeded() && light.specular->format == PixelFormat::RGBA32Float)
        {
            const EnvironmentCubePixels& source = *light.specular;
            HdrCubeResult                result =
                ProcessHdrCube({source.size, source.mipLevels, source.rgba32FloatMipFaces}, policy, remaining, cancellation);
            status = result.status;
            if (status.Succeeded() && !result.reason.empty())
            {
                char object[64]{};
                std::snprintf(object, sizeof(object), "environment %zu", index);
                status = AddProcessingWarning(object, result.reason, remaining, diagnostics);
            }
            if (status.Succeeded() && result.pixels)
            {
                light.specular = std::move(result.pixels);
            }
            const uint64_t temporary  = result.ownedBytes - result.pixelBytes;
            result                    = {};
            remaining                += temporary;
        }
    }
    return status;
}
} // namespace zenasset::detail
