#pragma once
#include "ZenAsset/Format/Validation.h"

namespace zenasset::detail
{
// Internal sources that own immutable bytes may reuse a completed pixel check.
// File-backed and client-supplied sources are resolved on each validation.
class ValidatedImagePayloadSource : public ImagePayloadSource
{
public:
    virtual bool HasValidatedPixels(const ImagePixels& descriptor) const = 0;
};

// The package reader and writer validate pixel payloads while each image is
// resident. This checks the remaining graph, including image/texture references.
bool ValidateSceneGraph(const Scene& scene, uint64_t maxRuntimeGeometryBytes, std::string* failure);

inline bool FitsValidationBudget(const Scene& scene, uint64_t budget)
{
    // Reference validation holds parent/visited/root marks simultaneously;
    // geometry validation separately sorts a copy of the coordinate-set list.
    return scene.nodes.size() <= budget / (sizeof(uint32_t) + 2 * sizeof(uint8_t))
        && scene.vertexTexCoordSets.size() <= budget / sizeof(uint64_t);
}
} // namespace zenasset::detail
