#pragma once
#include "Processor/Backend.h"
#include <mutex>
#include <algorithm>

namespace zenasset
{
// The first content error of one import. Texture-decoding workers report through it too,
// and each import stage skips its work once an error is recorded.
class ImportErrors
{
public:
    // Later failures keep the first message.
    void Fail(std::string message, SceneImportStatus status = SceneImportStatus::InvalidData);

    bool Failed() const;

    SceneImportStatus Status() const;

    std::string Message() const;

    void Reset(SceneImportControl* control = nullptr, ImageBudget* images = nullptr, GeometryBudget* geometry = nullptr);

    bool            ReserveGeometryBytes(uint64_t count, uint64_t stride = 1);
    bool            ReserveGeometryBytes(GeometryReservation& reservation, uint64_t count, uint64_t stride = 1);
    GeometryBudget* GetGeometryBudget() const
    {
        return m_geometry.Budget();
    }

    bool         ReserveImageBytes(uint64_t bytes);
    ImageBudget* GetImageBudget() const
    {
        return m_images;
    }

    void DetachControl();

    void SetStage(SceneImportStage stage);

private:
    SceneImportStatus   m_status{SceneImportStatus::InvalidData};
    mutable std::mutex  m_mutex;
    std::string         m_message;
    std::atomic<bool>   m_failed{false};
    SceneImportControl* m_control{nullptr};
    ImageBudget*        m_images{nullptr};
    GeometryReservation m_geometry;
};


template <class T>
bool ReserveGeometryVector(std::vector<T>& values, size_t count, ImportErrors& errors, GeometryReservation* scope = nullptr)
{
    bool valid = !errors.Failed();
    if (valid && count > values.capacity())
    {
        const size_t maximum   = values.max_size();
        const size_t capacity  = values.capacity();
        const size_t requested = capacity <= maximum / 2 ? std::max(count, capacity * 2) : count;
        if (requested > maximum)
        {
            errors.Fail("Geometry buffer exceeds container size limit", SceneImportStatus::ResourceLimit);
            valid = false;
        }
        else
        {
            valid = scope == nullptr ? errors.ReserveGeometryBytes(requested, sizeof(T))
                                     : errors.ReserveGeometryBytes(*scope, requested, sizeof(T));
        }
        if (valid)
        {
            // Retaining previous growth charges conservatively accounts for
            // the old allocation overlapping its replacement during reserve.
            values.reserve(requested);
        }
    }
    return valid;
}

template <class T>
bool ResizeGeometryVector(std::vector<T>& values, size_t count, ImportErrors& errors, GeometryReservation* scope = nullptr)
{
    const bool valid = ReserveGeometryVector(values, count, errors, scope);
    if (valid)
    {
        values.resize(count);
    }
    return valid;
}

} // namespace zenasset
