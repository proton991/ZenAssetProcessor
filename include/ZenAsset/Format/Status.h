#pragma once
#include <atomic>
#include <cstdint>
#include <string>

namespace zenasset
{
enum class StatusCode : uint32_t
{
    Success,
    InvalidArgument,
    SourceUnavailable,
    InvalidData,
    UnsupportedVersion,
    UnsupportedCodec,
    Cancelled,
    ResourceLimit,
    IOError,
    UnsupportedFormat,
    UnsupportedFeature,
    InvalidRequest = InvalidArgument
};

struct Status
{
    StatusCode  code{StatusCode::Success};
    std::string message;

    bool Succeeded() const
    {
        return code == StatusCode::Success;
    }
};

class Cancellation
{
public:
    // The optional poll callback can bridge an owner's cancellation state. It
    // may run on workers; its user data must outlive every operation using it.
    explicit Cancellation(bool (*poll)(void*) = nullptr, void* user = nullptr) : m_poll(poll), m_user(user) {}

    void Cancel()
    {
        m_cancelled.store(true, std::memory_order_release);
    }

    bool IsCancelled() const
    {
        return m_cancelled.load(std::memory_order_acquire) || (m_poll != nullptr && m_poll(m_user));
    }

private:
    std::atomic<bool> m_cancelled{false};
    bool              (*m_poll)(void*){nullptr};
    void*             m_user{nullptr};
};
} // namespace zenasset
