#pragma once
#include "Processing/AtlasPacking.h"

namespace zenasset::detail
{
struct ArrayPackingOptions
{
    uint32_t maxDimension{16384};
    uint32_t maxLayers{256};
    uint32_t maxPages{64};
    uint32_t maxTiles{8192};
};

struct ArrayTileLayer
{
    uint32_t id{0};
    // UINT32_MAX retains the original standalone image: no compatible partner.
    uint32_t page{UINT32_MAX};
    uint32_t layer{0};
    bool     operator==(const ArrayTileLayer&) const = default;
};

struct ArrayPackResult
{
    Status                      status;
    std::vector<ArrayTileLayer> tiles;
    std::vector<AtlasPage>      pages;
    uint64_t                    ownedBytes{0};
};

// Groups finalized single-layer images by caller-assigned compatibility group,
// exact format, dimensions and explicit mip count. Full and partial chains are
// copied byte-for-byte into native array layers, including compressed blocks.
// No resampling, mip generation, compression or shader UV transform is applied.
// Input IDs are unique; page/layer placement and ID-sorted mappings are invariant
// under input permutation. Singleton groups/remainders remain standalone.
//
// Like PackAtlasTiles, charges requested retained/scratch storage before allocation
// (not allocator overhead or borrowed input). Scratch is refunded; failure or
// cancellation clears output and refunds everything. This primitive does not
// qualify bindings, select profitable groups or change a Scene.
ArrayPackResult PackArrayPages(std::span<const AtlasTileInput> inputs,
                               const ArrayPackingOptions&      options,
                               uint64_t&                       remainingBytes,
                               const Cancellation*             cancellation = nullptr);
} // namespace zenasset::detail
