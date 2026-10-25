#pragma once
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace zenasset
{
// Requested geometry/deformation storage only: excludes parser/codec-private
// allocations, allocator bookkeeping and process RSS. Reservations may be
// conservative preflight bounds rather than the eventual live payload size.
class GeometryBudget
{
public:
    explicit GeometryBudget(uint64_t limit) : m_limit(limit) {}
    bool Reserve(uint64_t count, uint64_t stride = 1)
    {
        bool           valid    = stride == 0 || (count <= UINT64_MAX / stride && count <= SIZE_MAX / stride);
        const uint64_t bytes    = valid ? count * stride : 0;
        uint64_t       used     = m_used.load(std::memory_order_relaxed);
        bool           reserved = false;
        while (valid && !reserved)
        {
            valid = used <= m_limit && bytes <= m_limit - used;
            if (valid)
            {
                reserved = m_used.compare_exchange_weak(used, used + bytes, std::memory_order_relaxed);
            }
        }
        if (reserved)
        {
            const uint64_t peak     = used + bytes;
            uint64_t       previous = m_peak.load(std::memory_order_relaxed);
            while (previous < peak && !m_peak.compare_exchange_weak(previous, peak, std::memory_order_relaxed))
            {
            }
        }
        return reserved;
    }
    void Release(uint64_t bytes)
    {
        [[maybe_unused]] const uint64_t previous = m_used.fetch_sub(bytes, std::memory_order_relaxed);
        assert(previous >= bytes);
    }
    uint64_t Used() const
    {
        return m_used.load(std::memory_order_relaxed);
    }
    uint64_t Peak() const
    {
        return m_peak.load(std::memory_order_relaxed);
    }

private:
    uint64_t              m_limit;
    std::atomic<uint64_t> m_used{0};
    std::atomic<uint64_t> m_peak{0};
};

// An import owns persistent/preflight charges; a local instance owns temporary
// storage. Reset/destruction must occur after users and workers have drained.
// Moves transfer the charge with its storage, without releasing it early.
class GeometryReservation
{
public:
    explicit GeometryReservation(GeometryBudget* budget = nullptr) : m_budget(budget) {}
    ~GeometryReservation()
    {
        Reset();
    }
    GeometryReservation(const GeometryReservation&)            = delete;
    GeometryReservation& operator=(const GeometryReservation&) = delete;
    GeometryReservation(GeometryReservation&& other) noexcept :
        m_budget(std::exchange(other.m_budget, nullptr)), m_bytes(other.m_bytes.exchange(0))
    {}
    GeometryReservation& operator=(GeometryReservation&& other) noexcept
    {
        if (this != &other)
        {
            Reset(std::exchange(other.m_budget, nullptr));
            m_bytes.store(other.m_bytes.exchange(0));
        }
        return *this;
    }
    bool Reserve(uint64_t count, uint64_t stride = 1)
    {
        const bool     valid    = stride == 0 || (count <= UINT64_MAX / stride && count <= SIZE_MAX / stride);
        const uint64_t bytes    = valid ? count * stride : 0;
        const bool     reserved = valid && (m_budget == nullptr || m_budget->Reserve(count, stride));
        if (reserved && m_budget != nullptr)
        {
            m_bytes.fetch_add(bytes, std::memory_order_relaxed);
        }
        return reserved;
    }
    void Reset(GeometryBudget* budget = nullptr)
    {
        const uint64_t bytes = m_bytes.exchange(0, std::memory_order_relaxed);
        if (m_budget != nullptr)
        {
            m_budget->Release(bytes);
        }
        m_budget = budget;
    }
    GeometryBudget* Budget() const
    {
        return m_budget;
    }

private:
    GeometryBudget*       m_budget{nullptr};
    std::atomic<uint64_t> m_bytes{0};
};
} // namespace zenasset
