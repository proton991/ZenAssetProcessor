#pragma once
#include <atomic>
#include <cstdint>

namespace zenasset
{
// A conservative per-import cumulative allowance for buffers produced by image
// decoding, conversion and packing. Counting temporary copies as well ensures
// concurrency cannot allocate more accounted bytes than this limit.
class ImageBudget
{
public:
    explicit ImageBudget(uint64_t limit) : m_limit(limit) {}
    bool Reserve(uint64_t bytes)
    {
        uint64_t current   = m_used.load(std::memory_order_relaxed);
        bool     reserved  = false;
        bool     available = true;
        while (available && !reserved)
        {
            available = current <= m_limit && bytes <= m_limit - current;
            if (available)
            {
                reserved = m_used.compare_exchange_weak(current, current + bytes, std::memory_order_relaxed);
            }
        }
        return reserved;
    }

private:
    uint64_t              m_limit;
    std::atomic<uint64_t> m_used{0};
};
} // namespace zenasset
