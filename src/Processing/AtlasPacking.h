#pragma once
#include "ZenAsset/Format/Image.h"
#include "ZenAsset/Format/Status.h"
#include <span>

namespace zenasset::detail
{
// Internal construction primitive. Callers must first qualify every binding and
// assign a compatibility group (role, sampling, interpretation and quality).
// This does not change a Scene or enable atlas sampling in the runtime.
struct AtlasTileInput
{
    uint32_t                           id{0};
    uint32_t                           group{0};
    std::shared_ptr<const ImagePixels> pixels;
};

struct AtlasPackingOptions
{
    uint32_t maxPageDimension{2048};
    uint32_t maxPages{64};
    uint32_t maxTiles{8192};
    uint32_t maxMipRegions{65536};
    // Both the rectangle origin and gutter are multiples of four. Compression
    // follows page construction; no block can contain two different tiles.
    uint32_t gutter{4};
    // Round each used page extent up to a power of two. The page limit must
    // also be a power of two; padded bytes count toward the allocation budget.
    bool powerOfTwoPages{false};
};

struct AtlasMipRegion
{
    uint32_t page{0};
    uint32_t x{0};
    uint32_t y{0};
    uint32_t width{0};
    uint32_t height{0};
    bool     operator==(const AtlasMipRegion&) const = default;
};

struct AtlasTileRegions
{
    uint32_t id{0};
    uint32_t firstMip{0};
    uint32_t mipCount{0};
    bool     operator==(const AtlasTileRegions&) const = default;
};

struct AtlasPage
{
    uint32_t                           group{0};
    std::shared_ptr<const ImagePixels> pixels;
    // Charged pixel owner and buffers, excluding result tables. Allows a caller
    // that retains only some pages to refund the discarded storage exactly.
    uint64_t ownedBytes{0};
};

struct AtlasPackResult
{
    Status                        status;
    std::vector<AtlasTileRegions> tiles;
    std::vector<AtlasMipRegion>   mips;
    std::vector<AtlasPage>        pages;
    uint64_t                      ownedBytes{0};
};

// Copies already-finalized RGBA8 mip pixels without resampling, rotation or
// compression. Every logical mip, including a 1x1 tail, has its own extruded
// rectangle in a physical level-zero page. Pages have one explicit physical mip;
// they must never receive ordinary whole-page mip generation. Logical sampling
// and LOD blending require separate runtime integration.
//
// Output order/layout is deterministic for stable unique IDs, independent of
// input order. Pages use four-texel-aligned extents, optionally rounded up to
// powers of two without moving the regions or resampling their pixels. Failure or
// cancellation exposes no partial output and restores remainingBytes. Success
// retains only ownedBytes of requested storage; all planning scratch is refunded.
// Input pixels, allocator overhead and fixed stack storage are excluded.
AtlasPackResult PackAtlasTiles(std::span<const AtlasTileInput> tiles,
                               const AtlasPackingOptions&      options,
                               uint64_t&                       remainingBytes,
                               const Cancellation*             cancellation = nullptr);
} // namespace zenasset::detail
