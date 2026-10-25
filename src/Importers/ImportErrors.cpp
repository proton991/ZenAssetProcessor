#include "Importers/ImportErrors.h"

namespace zenasset
{
void ImportErrors::Fail(std::string message, SceneImportStatus status)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_failed.load(std::memory_order_relaxed))
    {
        m_message = std::move(message);
        m_status  = status;

        m_failed.store(true, std::memory_order_release);
    }
}

SceneImportStatus ImportErrors::Status() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

bool ImportErrors::Failed() const
{
    return m_failed.load(std::memory_order_acquire) || (m_control != nullptr && m_control->IsCancelled());
}

std::string ImportErrors::Message() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    return m_message.empty() && m_control != nullptr && m_control->IsCancelled() ? "Scene import cancelled" : m_message;
}

void ImportErrors::Reset(SceneImportControl* control, ImageBudget* images, GeometryBudget* geometry)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    m_message.clear();
    m_status  = SceneImportStatus::InvalidData;

    m_control = control;
    m_images  = images;
    m_geometry.Reset(geometry);

    m_failed.store(false, std::memory_order_release);
}

bool ImportErrors::ReserveGeometryBytes(uint64_t count, uint64_t stride)
{
    return ReserveGeometryBytes(m_geometry, count, stride);
}

bool ImportErrors::ReserveGeometryBytes(GeometryReservation& reservation, uint64_t count, uint64_t stride)
{
    const bool valid = !Failed() && reservation.Reserve(count, stride);
    if (!valid && !Failed())
    {
        Fail("Geometry/deformation buffers exceed maxGeometryBytes", SceneImportStatus::ResourceLimit);
    }
    return valid;
}

bool ImportErrors::ReserveImageBytes(uint64_t bytes)
{
    const bool valid = !Failed() && (m_images == nullptr || m_images->Reserve(bytes));
    if (!valid && !Failed())
    {
        Fail("Decoded image buffers exceed the configured byte limit", SceneImportStatus::ResourceLimit);
    }
    return valid;
}

void ImportErrors::DetachControl()
{
    // Called only after every processing future has completed.
    m_control = nullptr;
}

void ImportErrors::SetStage(SceneImportStage stage)
{
    if (m_control != nullptr)
    {
        m_control->SetStage(stage);
    }
}

} // namespace zenasset
