#include "Processing/ArrayPacking.h"
#include "Format/BlockValidation.h"
#include <algorithm>
#include <cstring>
#include <numeric>
#include <tuple>

namespace zenasset::detail
{
namespace
{
struct PagePlan
{
    uint32_t first;
    uint32_t count;
};

struct InputOrder
{
    std::span<const AtlasTileInput> inputs;
    bool                            byId;
    bool                            operator()(uint32_t left, uint32_t right) const
    {
        const AtlasTileInput& a = inputs[left];
        const AtlasTileInput& b = inputs[right];
        return byId
                 ? a.id < b.id
                 : std::tuple(a.group, a.pixels->format, a.pixels->width, a.pixels->height, a.pixels->mips.size(), a.id)
                       < std::tuple(b.group, b.pixels->format, b.pixels->width, b.pixels->height, b.pixels->mips.size(), b.id);
    }
};

bool SameGroup(const AtlasTileInput& a, const AtlasTileInput& b)
{
    return a.group == b.group && a.pixels->format == b.pixels->format && a.pixels->width == b.pixels->width
        && a.pixels->height == b.pixels->height && a.pixels->mips.size() == b.pixels->mips.size();
}

bool TileOrder(const ArrayTileLayer& a, const ArrayTileLayer& b)
{
    return a.id < b.id;
}

class ArrayPacker
{
public:
    ArrayPacker(std::span<const AtlasTileInput> inputs,
                const ArrayPackingOptions&      options,
                uint64_t&                       remaining,
                const Cancellation*             cancellation) :
        m_inputs(inputs), m_options(options), m_remaining(remaining), m_cancellation(cancellation)
    {}

    ArrayPackResult Run()
    {
        Validate();
        if (Active())
        {
            Plan();
        }
        if (Active())
        {
            CopyPages();
        }
        if (Active())
        {
            std::sort(m_result.tiles.begin(), m_result.tiles.end(), TileOrder);
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
        if (m_result.status.Succeeded() && m_cancellation && m_cancellation->IsCancelled())
        {
            m_result.status = {StatusCode::Cancelled, "Array packing cancelled"};
        }
        return m_result.status.Succeeded();
    }

    bool Reserve(uint64_t count, uint64_t stride, bool owned)
    {
        if (Active() && (count > SIZE_MAX / stride || count > m_remaining / stride))
        {
            m_result.status = {StatusCode::ResourceLimit, "Array packing exceeds its allocation budget"};
        }
        if (m_result.status.Succeeded())
        {
            m_remaining                   -= count * stride;
            (owned ? m_owned : m_scratch) += count * stride;
        }
        return m_result.status.Succeeded();
    }

    void Validate()
    {
        if (m_options.maxDimension == 0 || m_options.maxDimension > 16384 || m_options.maxLayers < 2
            || m_options.maxLayers > 2048 || m_options.maxPages == 0 || m_options.maxTiles == 0)
        {
            m_result.status = {StatusCode::InvalidArgument, "Invalid array extent, layer or page limits"};
        }
        else if (m_inputs.size() > m_options.maxTiles || m_inputs.size() > UINT32_MAX)
        {
            m_result.status = {StatusCode::ResourceLimit, "Too many array inputs"};
        }
        if (Active() && Reserve(m_inputs.size(), sizeof(uint32_t), false))
        {
            m_order.resize(m_inputs.size());
            std::iota(m_order.begin(), m_order.end(), 0u);
            std::sort(m_order.begin(), m_order.end(), InputOrder{m_inputs, true});
        }
        for (size_t index = 0; Active() && index < m_order.size(); ++index)
        {
            const AtlasTileInput& input = m_inputs[m_order[index]];
            if (index != 0 && input.id == m_inputs[m_order[index - 1]].id)
            {
                m_result.status = {StatusCode::InvalidArgument, "Array input IDs must be unique"};
            }
            else if (!input.pixels || input.pixels->arrayLayers != 1 || input.pixels->mips.empty())
            {
                m_result.status = {StatusCode::InvalidData, "Array input requires finalized single-layer explicit mips"};
            }
            else
            {
                ValidateImage(*input.pixels);
            }
        }
    }

    void ValidateImage(const ImagePixels& pixels)
    {
        const bool supported = pixels.format == PixelFormat::RGBA8Unorm || pixels.format == PixelFormat::RGBA8Srgb
                            || pixels.format == PixelFormat::BC4Unorm || pixels.format == PixelFormat::BC5Unorm
                            || pixels.format == PixelFormat::BC7Unorm || pixels.format == PixelFormat::BC7Srgb;
        if (!supported)
        {
            m_result.status = {StatusCode::UnsupportedFormat, "Unsupported array page format"};
        }
        else if (pixels.width > m_options.maxDimension || pixels.height > m_options.maxDimension)
        {
            m_result.status = {StatusCode::ResourceLimit, "Array input exceeds the dimension limit"};
        }
        else if (pixels.bytes != pixels.mips[0])
        {
            m_result.status = {StatusCode::InvalidData, "Array base bytes differ from mip zero"};
        }
        for (size_t level = 0; Active() && level < pixels.mips.size(); ++level)
        {
            std::span<const uint8_t> bytes;
            if (level >= 32 || !GetImageLayerMipBytes(pixels, 0, static_cast<uint32_t>(level), bytes)
                || !ValidBlockPayload(pixels.format, bytes))
            {
                m_result.status = {StatusCode::InvalidData, "Invalid array input extent, mip count or block payload"};
            }
        }
    }

    void Plan()
    {
        const size_t capacity = std::min(m_inputs.size() / 2, size_t(m_options.maxPages));
        if (Reserve(capacity, sizeof(PagePlan), false))
        {
            m_plans.reserve(capacity);
            std::sort(m_order.begin(), m_order.end(), InputOrder{m_inputs, false});
        }
        size_t first = 0;
        while (Active() && first < m_order.size())
        {
            size_t end = first + 1;
            while (end < m_order.size() && end - first < m_options.maxLayers
                   && SameGroup(m_inputs[m_order[first]], m_inputs[m_order[end]]))
            {
                ++end;
            }
            if (end - first > 1)
            {
                if (m_plans.size() == m_options.maxPages)
                {
                    m_result.status = {StatusCode::ResourceLimit, "Array packing exceeds its page limit"};
                }
                else
                {
                    m_plans.push_back({static_cast<uint32_t>(first), static_cast<uint32_t>(end - first)});
                }
            }
            first = end;
        }
    }

    void CopyBytes(std::vector<uint8_t>& target, size_t offset, const std::vector<uint8_t>& source)
    {
        for (size_t first = 0; Active() && first < source.size(); first += std::min(size_t(1 << 20), source.size() - first))
        {
            const size_t count = std::min(size_t(1 << 20), source.size() - first);
            std::memcpy(target.data() + offset + first, source.data() + first, count);
        }
    }

    void CopyPages()
    {
        if (Reserve(m_inputs.size(), sizeof(ArrayTileLayer), true) && Reserve(m_plans.size(), sizeof(AtlasPage), true))
        {
            m_result.tiles.resize(m_inputs.size());
            m_result.pages.reserve(m_plans.size());
            for (size_t index = 0; index < m_inputs.size(); ++index)
            {
                m_result.tiles[index].id = m_inputs[index].id;
            }
        }
        for (size_t page = 0; Active() && page < m_plans.size(); ++page)
        {
            const uint64_t               before = m_owned;
            const PagePlan&              plan   = m_plans[page];
            const AtlasTileInput&        first  = m_inputs[m_order[plan.first]];
            const ImagePixels&           source = *first.pixels;
            std::shared_ptr<ImagePixels> pixels;
            if (Reserve(1, sizeof(ImagePixels), true) && Reserve(source.mips.size(), sizeof(std::vector<uint8_t>), true))
            {
                pixels              = std::make_shared<ImagePixels>();
                pixels->width       = source.width;
                pixels->height      = source.height;
                pixels->format      = source.format;
                pixels->arrayLayers = plan.count;
                pixels->mips.resize(source.mips.size());
            }
            for (size_t level = 0; Active() && level < source.mips.size(); ++level)
            {
                if (Reserve(plan.count, source.mips[level].size(), true))
                {
                    pixels->mips[level].resize(source.mips[level].size() * plan.count);
                    for (uint32_t layer = 0; Active() && layer < plan.count; ++layer)
                    {
                        CopyBytes(pixels->mips[level], size_t(layer) * source.mips[level].size(),
                                  m_inputs[m_order[plan.first + layer]].pixels->mips[level]);
                    }
                }
            }
            if (Active() && Reserve(pixels->mips[0].size(), 1, true))
            {
                pixels->bytes.resize(pixels->mips[0].size());
                CopyBytes(pixels->bytes, 0, pixels->mips[0]);
            }
            if (Active())
            {
                m_result.pages.push_back({first.group, std::move(pixels), m_owned - before});
                for (uint32_t layer = 0; layer < plan.count; ++layer)
                {
                    const uint32_t input  = m_order[plan.first + layer];
                    m_result.tiles[input] = {m_inputs[input].id, static_cast<uint32_t>(page), layer};
                }
            }
        }
    }

    std::span<const AtlasTileInput> m_inputs;
    const ArrayPackingOptions&      m_options;
    uint64_t&                       m_remaining;
    const Cancellation*             m_cancellation;
    ArrayPackResult                 m_result;
    std::vector<uint32_t>           m_order;
    std::vector<PagePlan>           m_plans;
    uint64_t                        m_owned{0};
    uint64_t                        m_scratch{0};
};
} // namespace

ArrayPackResult PackArrayPages(std::span<const AtlasTileInput> inputs,
                               const ArrayPackingOptions&      options,
                               uint64_t&                       remainingBytes,
                               const Cancellation*             cancellation)
{
    return ArrayPacker(inputs, options, remainingBytes, cancellation).Run();
}
} // namespace zenasset::detail
