#include "ZenAsset/Processor/Cook.h"
#include "Diagnostics/StageMeasurements.h"
#include "ZenAsset/Format/Validation.h"
#include "Processing/GeometryProcessing.h"
#include "Processing/EnvironmentProcessing.h"
#include "Processing/SceneStorage.h"
#include "Processing/TextureProcessing.h"
#include "Processing/PageProcessing.h"

namespace zenasset
{
SceneCookResult CookScene(const std::shared_ptr<const Scene>& input,
                          const SceneCookOptions&             options,
                          const Cancellation*                 cancellation)
{
    ZENASSET_MEASURE_SCOPE(Cook);
    SceneCookResult result;
    result.status = ValidateProcessingSettings(options.settings);
    if (result.status.Succeeded() && input == nullptr)
    {
        result.status = {StatusCode::InvalidArgument, "Scene cooking requires an input scene"};
    }
    if (result.status.Succeeded() && cancellation != nullptr && cancellation->IsCancelled())
    {
        result.status = {StatusCode::Cancelled, "Scene cooking cancelled"};
    }
    if (result.status.Succeeded() && !detail::FitsValidationBudget(*input, options.limits.maxAllocationBytes))
    {
        result.status = {StatusCode::ResourceLimit, "Scene validation scratch exceeds the cooking allocation budget"};
    }
    std::string failure;
    if (result.status.Succeeded() && !ValidateSceneAsset(*input, options.limits.maxRuntimeGeometryBytes, &failure))
    {
        result.status = {StatusCode::InvalidData, "Invalid input scene: " + failure};
    }
    const bool processing = HasProcessingOperations(options.settings);
    uint64_t   remaining  = processing ? options.limits.maxAllocationBytes : UINT64_MAX;
    if (result.status.Succeeded())
    {
        result.status = detail::ReserveSceneCopy(*input, options.limits, remaining, cancellation);
    }
    std::shared_ptr<Scene> cooked;
    if (result.status.Succeeded() && processing)
    {
        cooked = std::make_shared<Scene>(*input);
        ZENASSET_MEASURE_BEGIN(geometry, GeometryProcess);
        result.status =
            detail::ProcessGeometry(*cooked, options.settings.geometry, remaining, result.diagnostics, cancellation);
        ZENASSET_MEASURE_END(geometry);
        if (result.status.Succeeded())
        {
            ZENASSET_MEASURE_SCOPE(TextureProcess);
            result.status =
                detail::ProcessTextures(*cooked, options.settings.textures, remaining, result.diagnostics, cancellation);
        }
        if (result.status.Succeeded())
        {
            ZENASSET_MEASURE_SCOPE(EnvironmentProcess);
            result.status =
                detail::ProcessEnvironments(*cooked, options.settings.textures, remaining, result.diagnostics, cancellation);
        }
        if (result.status.Succeeded())
        {
            ZENASSET_MEASURE_SCOPE(PageProcess);
            result.status =
                detail::ProcessTexturePages(*cooked, options.settings.textures, remaining, result.diagnostics, cancellation);
        }
        if (result.status.Succeeded() && !detail::FitsValidationBudget(*cooked, remaining))
        {
            result.status = {StatusCode::ResourceLimit, "Cooked scene validation scratch exceeds the allocation budget"};
        }
        if (result.status.Succeeded() && !ValidateSceneAsset(*cooked, options.limits.maxRuntimeGeometryBytes, &failure))
        {
            result.status = {StatusCode::InvalidData, "Invalid cooked scene: " + failure};
        }
    }
    if (result.status.Succeeded() && cancellation != nullptr && cancellation->IsCancelled())
    {
        result.status = {StatusCode::Cancelled, "Scene cooking cancelled"};
    }
    if (result.status.Succeeded())
    {
        result.scene = processing ? std::shared_ptr<const Scene>(std::move(cooked)) : input;
    }
    return result;
}
} // namespace zenasset
