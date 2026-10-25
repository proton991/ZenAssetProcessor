#pragma once
#include "Format/SceneSchema.h"
#include "Format/SceneResources.h"
#include <type_traits>

namespace zenasset::detail
{
// Use the field schema to count copied owning records, avoiding a second list
// of nested attributes. Image/cube pixels are immutable shared storage in Scene copies.
class SceneStorageCounter
{
public:
    static constexpr bool Reading  = false;
    template <class T> using Value = const T;

    SceneStorageCounter(const SceneReadLimits& limits, uint64_t& remaining, const Cancellation* cancellation) :
        m_limits(limits), m_remaining(remaining), m_cancellation(cancellation), m_stringLimit(limits.maxStringBytes)
    {}

    template <class... T> void operator()(const T&... values)
    {
        (One(values), ...);
    }
    template <class T> void Array(ResourceType, const std::vector<T>& values)
    {
        One(values);
    }
    template <class T> void Single(ResourceType type, const T& value)
    {
        m_stringLimit = type == ResourceType::SourceDocument ? m_limits.maxSourceDocumentBytes : m_limits.maxStringBytes;
        One(value);
        m_stringLimit = m_limits.maxStringBytes;
    }
    void Metadata(const Scene& scene)
    {
        (*this)(scene.sourcePath, scene.dependencies);
    }
    void Fail(StatusCode code, const char* message)
    {
        if (m_status.Succeeded())
        {
            m_status = {code, message};
        }
    }
    Status ResultStatus() const
    {
        return m_status;
    }
    uint32_t Version() const
    {
        // Count the current in-memory records, including every versioned field.
        return UINT32_MAX;
    }
    bool Reserve(uint64_t count, size_t stride)
    {
        if (count > SIZE_MAX / stride || count > m_remaining / stride)
        {
            Fail(StatusCode::ResourceLimit, "Cooked scene copy exceeds its count or allocation limit");
        }
        if (m_status.Succeeded())
        {
            m_remaining -= count * stride;
        }
        return m_status.Succeeded();
    }

private:
    template <class T> void One(const T& value)
    {
        using U = std::remove_cv_t<T>;
        if (m_status.Succeeded() && m_cancellation != nullptr && m_cancellation->IsCancelled())
        {
            Fail(StatusCode::Cancelled, "Scene cooking cancelled");
        }
        if (m_status.Succeeded())
        {
            if constexpr (std::is_same_v<U, std::string>)
            {
                if (value.size() > m_stringLimit || value.size() > m_limits.maxArrayElements || !IsValidUtf8(value))
                {
                    Fail(value.size() > m_stringLimit || value.size() > m_limits.maxArrayElements ? StatusCode::ResourceLimit
                                                                                                  : StatusCode::InvalidData,
                         "Scene text exceeds its limit or is not valid UTF-8");
                }
                if (value.size() == SIZE_MAX)
                {
                    Fail(StatusCode::ResourceLimit, "Scene text length exceeds address space");
                }
                else
                {
                    Reserve(value.size() + 1, 1);
                }
            }
            else if constexpr (std::is_same_v<U, ImageAsset>)
            {
                One(value.name);
            }
            else if constexpr (std::is_same_v<U, ImageBasedLightAsset>)
            {
                (*this)(value.name, value.irradianceCoefficients);
            }
            else if constexpr (std::is_trivially_copyable_v<U>)
            {
                // Stored inside an already-counted record or vector allocation.
            }
            else if constexpr (requires {
                                   typename U::value_type;
                                   value.size();
                                   value.data();
                               })
            {
                using Element = typename U::value_type;
                if (value.size() > m_limits.maxArrayElements)
                {
                    Fail(StatusCode::ResourceLimit, "Scene array exceeds the cooking element limit");
                }
                if (Reserve(value.size(), sizeof(Element)))
                {
                    if constexpr (!std::is_trivially_copyable_v<Element>)
                    {
                        for (size_t index = 0; m_status.Succeeded() && index < value.size(); ++index)
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

    const SceneReadLimits& m_limits;
    uint64_t&              m_remaining;
    const Cancellation*    m_cancellation;
    uint64_t               m_stringLimit;
    Status                 m_status;
};

inline Status ReserveSceneCopy(const Scene&           input,
                               const SceneReadLimits& limits,
                               uint64_t&              remaining,
                               const Cancellation*    cancellation)
{
    SceneStorageCounter counter(limits, remaining, cancellation);
    if (counter.Reserve(1, sizeof(Scene)))
    {
        counter(input.name, input.roots, input.vertexTexCoordSets);
        VisitSceneResources(counter, input);
    }
    return counter.ResultStatus();
}
} // namespace zenasset::detail
