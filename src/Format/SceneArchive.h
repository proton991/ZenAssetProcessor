#pragma once
#include "Format/SceneSchema.h"
#include "Format/SceneResources.h"
#include <limits>

namespace zenasset::detail
{
// Raw transfers are an optimization of the field schema, never native object
// serialization: only padding-free arrays matching that schema qualify.
template <class T> constexpr bool RawSceneElement()
{
    return std::endian::native == std::endian::little && std::is_trivially_copyable_v<T>
        && ((std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) || std::is_same_v<T, Vec2> || std::is_same_v<T, Vec3>
            || std::is_same_v<T, Vec4> || std::is_same_v<T, Mat4> || std::is_same_v<T, Vertex>
            || std::is_same_v<T, VertexJointInfluence>);
}
static_assert(sizeof(Vec2) == 8 && sizeof(Vec3) == 12 && sizeof(Vec4) == 16 && sizeof(Mat4) == 64 && sizeof(Vertex) == 112
              && sizeof(VertexJointInfluence) == 8);
static_assert(std::numeric_limits<float>::is_iec559 && std::numeric_limits<double>::is_iec559);

template <bool IsReading> class SceneArchive
{
public:
    static constexpr bool Reading  = IsReading;
    template <class T> using Value = std::conditional_t<Reading, T, const T>;
    SceneArchive(std::span<const std::byte> input,
                 std::vector<std::byte>*    output,
                 const SceneReadLimits&     limits,
                 uint64_t&                  budget,
                 const Cancellation*        cancellation,
                 uint32_t                   version = 1) :
        m_input(input), m_output(output), m_limits(limits), m_budget(budget), m_cancellation(cancellation), m_version(version)
    {}

    template <class... T> void operator()(T&... value)
    {
        (One(value), ...);
    }
    bool Good() const
    {
        return m_status.Succeeded();
    }
    const Status& ResultStatus() const
    {
        return m_status;
    }
    size_t ByteSize() const
    {
        return m_position;
    }
    uint32_t Version() const
    {
        return m_version;
    }
    void Fail(StatusCode code, const char* message)
    {
        if (Good())
        {
            m_status = {code, message};
        }
    }
    bool Reserve(uint64_t count, size_t stride, uint64_t minimumStored = 1)
    {
        if (count > m_limits.maxArrayElements || count > SIZE_MAX / stride)
        {
            Fail(StatusCode::ResourceLimit, "Scene array exceeds its configured count limit");
        }
        if constexpr (Reading)
        {
            if (minimumStored != 0 && count > (m_input.size() - m_position) / minimumStored)
            {
                Fail(StatusCode::InvalidData, "Scene array count exceeds remaining resource bytes");
            }
            if (count > m_budget / stride)
            {
                Fail(StatusCode::ResourceLimit, "Scene decoded allocation budget exceeded");
            }
            if (Good())
            {
                m_budget -= count * stride;
            }
        }
        return Good();
    }
    Status Finish()
    {
        if constexpr (Reading)
        {
            if (m_position != m_input.size())
            {
                Fail(StatusCode::InvalidData, "Unexpected trailing scene resource bytes");
            }
        }
        return m_status;
    }

private:
    template <class Byte> void Bytes(Byte* data, uint64_t count)
    {
        static_assert(sizeof(Byte) == 1);
        if (m_cancellation != nullptr && m_cancellation->IsCancelled())
        {
            Fail(StatusCode::Cancelled, "Scene serialization cancelled");
        }
        if constexpr (Reading)
        {
            if (count > m_input.size() - m_position)
            {
                Fail(StatusCode::InvalidData, "Scene field exceeds its resource bounds");
            }
            if (Good() && count > 0)
            {
                std::memcpy(data, m_input.data() + m_position, static_cast<size_t>(count));
                m_position += static_cast<size_t>(count);
            }
        }
        else
        {
            if (count > m_limits.package.maxChunkBytes || m_position > m_limits.package.maxChunkBytes - count
                || count > SIZE_MAX - m_position || count > m_budget || m_position > m_budget - count)
            {
                Fail(StatusCode::ResourceLimit, "Scene resource exceeds its configured byte limit");
            }
            if (Good() && count > 0)
            {
                if (m_output != nullptr)
                {
                    if (m_position > m_output->size() || count > m_output->size() - m_position)
                    {
                        Fail(StatusCode::InvalidData, "Scene changed between sizing and serialization");
                    }
                    else
                    {
                        std::memcpy(m_output->data() + m_position, data, static_cast<size_t>(count));
                    }
                }
                m_position += static_cast<size_t>(count);
            }
        }
    }
    template <class T> void Scalar(T& value)
    {
        using U    = std::remove_cv_t<T>;
        using Bits = std::conditional_t<sizeof(U) == 1, uint8_t, std::conditional_t<sizeof(U) == 4, uint32_t, uint64_t>>;
        static_assert(sizeof(U) == 1 || sizeof(U) == 4 || sizeof(U) == 8);
        Bits    bits = std::bit_cast<Bits>(value);
        uint8_t bytes[sizeof(U)]{};
        if constexpr (!Reading)
        {
            for (size_t index = 0; index < sizeof(U); ++index)
            {
                bytes[index] = static_cast<uint8_t>(bits >> (index * 8));
            }
        }
        Bytes(bytes, sizeof(U));
        if constexpr (Reading)
        {
            bits = 0;
            for (size_t index = 0; index < sizeof(U); ++index)
            {
                bits |= static_cast<Bits>(bytes[index]) << (index * 8);
            }
            value = std::bit_cast<U>(bits);
        }
    }
    template <class T> void One(T& value)
    {
        using U = std::remove_cv_t<T>;
        if (Good())
        {
            if constexpr (std::is_same_v<U, bool>)
            {
                uint8_t encoded = value ? 1 : 0;
                Scalar(encoded);
                if constexpr (Reading)
                {
                    if (encoded > 1)
                    {
                        Fail(StatusCode::InvalidData, "Invalid scene boolean");
                    }
                    value = encoded != 0;
                }
            }
            else if constexpr (std::is_enum_v<U>)
            {
                uint32_t encoded = static_cast<uint32_t>(value);
                Scalar(encoded);
                if constexpr (Reading)
                {
                    value = static_cast<U>(encoded);
                }
            }
            else if constexpr (std::is_arithmetic_v<U>)
            {
                Scalar(value);
            }
            else if constexpr (std::is_same_v<U, std::string>)
            {
                uint64_t count = value.size();
                Scalar(count);
                if (count > m_limits.maxStringBytes)
                {
                    Fail(StatusCode::ResourceLimit, "Scene string exceeds its configured byte limit");
                }
                if (Reserve(count, 1))
                {
                    if constexpr (Reading)
                    {
                        value.resize(static_cast<size_t>(count));
                    }
                    Bytes(value.data(), count);
                    if (Good() && !IsValidUtf8(value))
                    {
                        Fail(StatusCode::InvalidData, "Scene string is not valid UTF-8");
                    }
                }
            }
            else if constexpr (requires {
                                   value.IsValid();
                                   value.value;
                               })
            {
                Scalar(value.value);
            }
            else if constexpr (requires(U& collection) {
                                   typename U::value_type;
                                   collection.resize(size_t(0));
                               })
            {
                using Element  = typename U::value_type;
                uint64_t count = value.size();
                Scalar(count);
                if (Reserve(count, sizeof(Element), RawSceneElement<Element>() ? sizeof(Element) : 1))
                {
                    if constexpr (Reading)
                    {
                        value.resize(static_cast<size_t>(count));
                    }
                    if constexpr (RawSceneElement<Element>())
                    {
                        Bytes(reinterpret_cast<std::conditional_t<Reading, uint8_t, const uint8_t>*>(value.data()),
                              count * sizeof(Element));
                    }
                    else
                    {
                        for (size_t index = 0; Good() && index < value.size(); ++index)
                        {
                            One(value[index]);
                        }
                    }
                }
            }
            else
            {
                Fields(*this, value);
            }
        }
    }
    std::span<const std::byte> m_input;
    std::vector<std::byte>*    m_output;
    const SceneReadLimits&     m_limits;
    uint64_t&                  m_budget;
    const Cancellation*        m_cancellation;
    uint32_t                   m_version;
    size_t                     m_position{0};
    Status                     m_status;
};
} // namespace zenasset::detail
