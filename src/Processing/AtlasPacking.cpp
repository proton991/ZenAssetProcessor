#include "Processing/AtlasPacking.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <numeric>
#include <tuple>

namespace zenasset::detail
{
namespace
{
constexpr uint32_t Block   = 4;
constexpr uint32_t Invalid = UINT32_MAX;

struct Rectangle
{
    uint32_t input;
    uint32_t mip;
    uint32_t output;
    uint32_t width;
    uint32_t height;
};

struct Shelf
{
    uint32_t y;
    uint32_t height;
    uint32_t nextX;
    uint32_t next{Invalid};
};

struct PageLayout
{
    uint32_t    group;
    PixelFormat format;
    uint32_t    width{0};
    uint32_t    height{0};
    uint32_t    firstShelf{Invalid};
    uint32_t    lastShelf{Invalid};
};

struct InputOrder
{
    std::span<const AtlasTileInput> inputs;
    bool                            operator()(uint32_t a, uint32_t b) const
    {
        return inputs[a].id < inputs[b].id;
    }
};

struct RectangleOrder
{
    std::span<const AtlasTileInput> inputs;
    bool                            operator()(const Rectangle& a, const Rectangle& b) const
    {
        const AtlasTileInput& left  = inputs[a.input];
        const AtlasTileInput& right = inputs[b.input];
        // Descending height/width gives stable first-fit decreasing shelves.
        return std::tuple(left.group, left.pixels->format, UINT32_MAX - a.height, UINT32_MAX - a.width, left.id, a.mip)
             < std::tuple(right.group, right.pixels->format, UINT32_MAX - b.height, UINT32_MAX - b.width, right.id, b.mip);
    }
};

uint32_t AlignedExtent(uint32_t value, uint32_t gutter)
{
    // Validation bounds all dimensions/gutters before this function is used.
    return (value + 2 * gutter + Block - 1) / Block * Block;
}

const std::vector<uint8_t>& MipBytes(const ImagePixels& pixels, uint32_t mip)
{
    return pixels.mips.empty() ? pixels.bytes : pixels.mips[mip];
}

class AtlasPacker
{
public:
    AtlasPacker(std::span<const AtlasTileInput> inputs,
                const AtlasPackingOptions&      options,
                uint64_t&                       remaining,
                const Cancellation*             cancellation) :
        m_inputs(inputs), m_options(options), m_remaining(remaining), m_cancellation(cancellation)
    {}

    AtlasPackResult Run()
    {
        Validate();
        if (Active())
        {
            MakeRectangles();
        }
        if (Active())
        {
            PlaceRectangles();
        }
        if (Active())
        {
            MakePages();
        }
        if (Active())
        {
            m_result.ownedBytes = m_owned;
        }
        else
        {
            Status failure   = std::move(m_result.status);
            m_result         = {};
            m_result.status  = std::move(failure);
            m_remaining     += m_owned;
        }
        m_remaining += m_scratch;
        return std::move(m_result);
    }

private:
    bool Active()
    {
        if (m_result.status.Succeeded() && m_cancellation != nullptr && m_cancellation->IsCancelled())
        {
            m_result.status = {StatusCode::Cancelled, "Atlas packing cancelled"};
        }
        return m_result.status.Succeeded();
    }

    bool Reserve(uint64_t count, uint64_t stride, bool owned)
    {
        if (m_result.status.Succeeded() && (count > SIZE_MAX / stride || count > m_remaining / stride))
        {
            m_result.status = {StatusCode::ResourceLimit, "Atlas packing exceeds its allocation budget"};
        }
        if (m_result.status.Succeeded())
        {
            const uint64_t bytes           = count * stride;
            m_remaining                   -= bytes;
            (owned ? m_owned : m_scratch) += bytes;
        }
        return m_result.status.Succeeded();
    }

    void Validate()
    {
        if (m_options.maxPageDimension < Block || m_options.maxPageDimension > 16384 || m_options.maxPageDimension % Block != 0
            || m_options.gutter == 0 || m_options.gutter % Block != 0 || m_options.gutter >= m_options.maxPageDimension / 2
            || m_options.maxPages == 0 || m_options.maxTiles == 0 || m_options.maxMipRegions == 0
            || (m_options.powerOfTwoPages && !std::has_single_bit(m_options.maxPageDimension)))
        {
            m_result.status = {StatusCode::InvalidArgument, "Invalid atlas page, gutter or count limits"};
        }
        else if (m_inputs.size() > m_options.maxTiles || m_inputs.size() > UINT32_MAX)
        {
            m_result.status = {StatusCode::ResourceLimit, "Too many atlas tiles"};
        }
        if (Active() && Reserve(m_inputs.size(), sizeof(uint32_t), false))
        {
            m_order.resize(m_inputs.size());
            std::iota(m_order.begin(), m_order.end(), 0u);
            std::sort(m_order.begin(), m_order.end(), InputOrder{m_inputs});
        }
        for (size_t index = 0; Active() && index < m_order.size(); ++index)
        {
            const AtlasTileInput& input = m_inputs[m_order[index]];
            if (index != 0 && input.id == m_inputs[m_order[index - 1]].id)
            {
                m_result.status = {StatusCode::InvalidArgument, "Atlas tile IDs must be unique"};
            }
            else if (!input.pixels || input.pixels->width == 0 || input.pixels->height == 0 || input.pixels->arrayLayers != 1)
            {
                m_result.status = {StatusCode::InvalidData, "Atlas tile requires a single-layer image with nonzero dimensions"};
            }
            else if (input.pixels->format != PixelFormat::RGBA8Unorm && input.pixels->format != PixelFormat::RGBA8Srgb)
            {
                m_result.status = {StatusCode::UnsupportedFormat, "Atlas pages require uncompressed RGBA8 input"};
            }
            else
            {
                ValidateImage(*input.pixels);
            }
        }
    }

    void ValidateImage(const ImagePixels& pixels)
    {
        const uint32_t interiorLimit = m_options.maxPageDimension - 2 * m_options.gutter;
        if (pixels.width > interiorLimit || pixels.height > interiorLimit)
        {
            m_result.status = {StatusCode::ResourceLimit, "Atlas tile and gutters exceed the page dimension"};
        }
        const size_t mipCount = std::max(size_t(1), pixels.mips.size());
        if (Active() && (mipCount > m_options.maxMipRegions - m_mipCount))
        {
            m_result.status = {StatusCode::ResourceLimit, "Too many atlas mip regions"};
        }
        uint32_t width  = pixels.width;
        uint32_t height = pixels.height;
        uint64_t bytes  = 0;
        if (Active() && (!GetImageLevelByteSize(pixels.format, width, height, bytes) || pixels.bytes.size() != bytes))
        {
            m_result.status = {StatusCode::InvalidData, "Atlas base image byte count is invalid"};
        }
        for (size_t mip = 0; Active() && mip < mipCount; ++mip)
        {
            if (!GetImageLevelByteSize(pixels.format, width, height, bytes)
                || MipBytes(pixels, static_cast<uint32_t>(mip)).size() != bytes)
            {
                m_result.status = {StatusCode::InvalidData, "Atlas mip byte count is invalid"};
            }
            else if (mip + 1 < mipCount && width == 1 && height == 1)
            {
                m_result.status = {StatusCode::InvalidData, "Atlas mip chain extends beyond its 1x1 tail"};
            }
            width  = std::max(1u, width / 2);
            height = std::max(1u, height / 2);
        }
        if (Active() && !pixels.mips.empty() && pixels.bytes != pixels.mips[0])
        {
            m_result.status = {StatusCode::InvalidData, "Atlas base image differs from its first explicit mip"};
        }
        if (Active())
        {
            m_mipCount += static_cast<uint32_t>(mipCount);
        }
    }

    void MakeRectangles()
    {
        if (Reserve(m_inputs.size(), sizeof(AtlasTileRegions), true) && Reserve(m_mipCount, sizeof(AtlasMipRegion), true)
            && Reserve(m_mipCount, sizeof(Rectangle), false) && Reserve(m_mipCount, sizeof(Shelf), false)
            && Reserve(std::min(m_mipCount, m_options.maxPages), sizeof(PageLayout), false))
        {
            m_result.tiles.reserve(m_inputs.size());
            m_result.mips.reserve(m_mipCount);
            m_rectangles.reserve(m_mipCount);
            m_shelves.reserve(m_mipCount);
            m_layouts.reserve(std::min(m_mipCount, m_options.maxPages));
            for (size_t index = 0; Active() && index < m_order.size(); ++index)
            {
                const uint32_t     input    = m_order[index];
                const ImagePixels& pixels   = *m_inputs[input].pixels;
                const uint32_t     mipCount = static_cast<uint32_t>(std::max(size_t(1), pixels.mips.size()));
                m_result.tiles.push_back({m_inputs[input].id, static_cast<uint32_t>(m_result.mips.size()), mipCount});
                uint32_t width  = pixels.width;
                uint32_t height = pixels.height;
                for (uint32_t mip = 0; mip < mipCount; ++mip)
                {
                    m_rectangles.push_back({input, mip, static_cast<uint32_t>(m_result.mips.size()),
                                            AlignedExtent(width, m_options.gutter), AlignedExtent(height, m_options.gutter)});
                    m_result.mips.push_back({0, 0, 0, width, height});
                    width  = std::max(1u, width / 2);
                    height = std::max(1u, height / 2);
                }
            }
            std::sort(m_rectangles.begin(), m_rectangles.end(), RectangleOrder{m_inputs});
        }
    }

    void PlaceRectangles()
    {
        for (size_t index = 0; Active() && index < m_rectangles.size(); ++index)
        {
            const Rectangle&      rectangle     = m_rectangles[index];
            const AtlasTileInput& input         = m_inputs[rectangle.input];
            uint32_t              selectedPage  = Invalid;
            uint32_t              selectedShelf = Invalid;
            for (uint32_t page = 0; Active() && selectedPage == Invalid && page < m_layouts.size(); ++page)
            {
                PageLayout& layout = m_layouts[page];
                if (layout.group == input.group && layout.format == input.pixels->format)
                {
                    for (uint32_t shelf = layout.firstShelf; Active() && shelf != Invalid && selectedShelf == Invalid;
                         shelf          = m_shelves[shelf].next)
                    {
                        if (rectangle.height <= m_shelves[shelf].height
                            && rectangle.width <= m_options.maxPageDimension - m_shelves[shelf].nextX)
                        {
                            selectedShelf = shelf;
                        }
                    }
                    if (Active()
                        && (selectedShelf != Invalid || rectangle.height <= m_options.maxPageDimension - layout.height))
                    {
                        selectedPage = page;
                    }
                }
            }
            if (Active() && selectedPage == Invalid)
            {
                if (m_layouts.size() == m_options.maxPages)
                {
                    m_result.status = {StatusCode::ResourceLimit, "Atlas packing exceeds its page count limit"};
                }
                else
                {
                    selectedPage = static_cast<uint32_t>(m_layouts.size());
                    m_layouts.push_back({input.group, input.pixels->format});
                }
            }
            if (Active())
            {
                PageLayout& layout = m_layouts[selectedPage];
                if (selectedShelf == Invalid)
                {
                    selectedShelf = static_cast<uint32_t>(m_shelves.size());
                    m_shelves.push_back({layout.height, rectangle.height, 0});
                    if (layout.lastShelf != Invalid)
                    {
                        m_shelves[layout.lastShelf].next = selectedShelf;
                    }
                    else
                    {
                        layout.firstShelf = selectedShelf;
                    }
                    layout.lastShelf  = selectedShelf;
                    layout.height    += rectangle.height;
                }
                Shelf&          shelf   = m_shelves[selectedShelf];
                AtlasMipRegion& region  = m_result.mips[rectangle.output];
                region.page             = selectedPage;
                region.x                = shelf.nextX + m_options.gutter;
                region.y                = shelf.y + m_options.gutter;
                shelf.nextX            += rectangle.width;
                layout.width            = std::max(layout.width, shelf.nextX);
            }
        }
    }

    void CopyRectangle(const Rectangle& rectangle, ImagePixels& page)
    {
        const AtlasMipRegion&       region = m_result.mips[rectangle.output];
        const std::vector<uint8_t>& source = MipBytes(*m_inputs[rectangle.input].pixels, rectangle.mip);
        const uint32_t              startX = region.x - m_options.gutter;
        const uint32_t              startY = region.y - m_options.gutter;
        for (uint32_t y = 0; Active() && y < rectangle.height; ++y)
        {
            const uint32_t sourceY = y < m_options.gutter ? 0 : std::min(y - m_options.gutter, region.height - 1);
            for (uint32_t x = 0; x < rectangle.width; ++x)
            {
                const uint32_t sourceX = x < m_options.gutter ? 0 : std::min(x - m_options.gutter, region.width - 1);
                const size_t   from    = (size_t(sourceY) * region.width + sourceX) * 4;
                const size_t   to      = (size_t(startY + y) * page.width + startX + x) * 4;
                std::memcpy(page.bytes.data() + to, source.data() + from, 4);
            }
        }
    }

    void MakePages()
    {
        if (Reserve(m_layouts.size(), sizeof(AtlasPage), true))
        {
            m_result.pages.reserve(m_layouts.size());
        }
        for (uint32_t index = 0; Active() && index < m_layouts.size(); ++index)
        {
            const uint64_t    before    = m_owned;
            const PageLayout& layout    = m_layouts[index];
            const uint32_t    width     = m_options.powerOfTwoPages ? std::bit_ceil(layout.width) : layout.width;
            const uint32_t    height    = m_options.powerOfTwoPages ? std::bit_ceil(layout.height) : layout.height;
            const uint64_t    byteCount = uint64_t(width) * height * 4;
            if (Reserve(1, sizeof(ImagePixels) + sizeof(std::vector<uint8_t>), true) && Reserve(byteCount, 2, true))
            {
                std::shared_ptr<ImagePixels> page = std::make_shared<ImagePixels>();
                page->width                       = width;
                page->height                      = height;
                page->format                      = layout.format;
                page->bytes.resize(static_cast<size_t>(byteCount), 0);
                for (size_t rectangle = 0; Active() && rectangle < m_rectangles.size(); ++rectangle)
                {
                    if (m_result.mips[m_rectangles[rectangle].output].page == index)
                    {
                        CopyRectangle(m_rectangles[rectangle], *page);
                    }
                }
                if (Active())
                {
                    page->mips.resize(1);
                    page->mips[0] = page->bytes;
                    m_result.pages.push_back({layout.group, std::move(page), m_owned - before});
                }
            }
        }
    }

    std::span<const AtlasTileInput> m_inputs;
    const AtlasPackingOptions&      m_options;
    uint64_t&                       m_remaining;
    const Cancellation*             m_cancellation;
    AtlasPackResult                 m_result;
    uint64_t                        m_owned{0};
    uint64_t                        m_scratch{0};
    uint32_t                        m_mipCount{0};
    std::vector<uint32_t>           m_order;
    std::vector<Rectangle>          m_rectangles;
    std::vector<Shelf>              m_shelves;
    std::vector<PageLayout>         m_layouts;
};
} // namespace

AtlasPackResult PackAtlasTiles(std::span<const AtlasTileInput> tiles,
                               const AtlasPackingOptions&      options,
                               uint64_t&                       remainingBytes,
                               const Cancellation*             cancellation)
{
    AtlasPacker packer(tiles, options, remainingBytes, cancellation);
    return packer.Run();
}
} // namespace zenasset::detail
