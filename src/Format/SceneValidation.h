#pragma once
#include "ZenAsset/Format/Validation.h"

namespace zenasset::detail
{
struct DeferredImageLayout
{
    explicit DeferredImageLayout(const ImagePixels& image) :
        width(image.width), height(image.height), format(image.format), layers(image.arrayLayers), levels(image.mips.size())
    {}
    bool Matches(const ImagePixels& image) const
    {
        bool valid = image.width == width && image.height == height && image.format == format && image.arrayLayers == layers
                  && image.mips.size() == levels && image.bytes.empty();
        for (const std::vector<uint8_t>& mip : image.mips)
        {
            valid &= mip.empty();
        }
        return valid;
    }
    uint32_t    width, height;
    PixelFormat format;
    uint32_t    layers;
    size_t      levels;
};

// Internal sources may reuse a completed pixel check after matching the descriptor
// and proving the payload still matches (immutable bytes or a verified native part).
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
