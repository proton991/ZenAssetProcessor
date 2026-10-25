#include "ZenAsset/Reader/Scene.h"
#include "Diagnostics/StageMeasurements.h"
#include "ZenAsset/Format/Validation.h"
#include "Format/SceneArchive.h"
#include "Format/SceneResources.h"
#include "Format/PackageLayout.h"
#include "Format/MeshEncoding.h"
#include <limits>

namespace zenasset
{
namespace
{
Status ValidateReadLimits(const Package&         package,
                          const SceneReadLimits& limits,
                          uint64_t               objectBytes,
                          uint64_t&              allocationBytes)
{
    Status                                status;
    const std::span<const ResourceRecord> records      = package.Resources();
    uint64_t                              decodedBytes = 0;
    if (records.size() > limits.package.maxResources
        || records.size() > limits.package.maxDirectoryBytes / sizeof(ResourceRecord)
        || records.size() > (UINT64_MAX - objectBytes) / sizeof(ResourceRecord))
    {
        status = {StatusCode::ResourceLimit, "Scene directory exceeds its configured limits"};
    }
    else
    {
        allocationBytes = objectBytes + records.size() * sizeof(ResourceRecord);
        if (allocationBytes > limits.maxAllocationBytes)
        {
            status = {StatusCode::ResourceLimit, "Scene and directory exceed the decoded allocation budget"};
        }
    }
    for (const ResourceRecord& record : records)
    {
        if (status.Succeeded())
        {
            if (record.offset > limits.package.maxFileBytes || record.storedBytes > limits.package.maxFileBytes - record.offset
                || record.decodedBytes > limits.package.maxChunkBytes || record.storedBytes > limits.package.maxChunkBytes
                || record.decodedBytes > limits.package.maxDecodedBytes - decodedBytes)
            {
                status = {StatusCode::ResourceLimit, "Retained package exceeds scene read limits"};
            }
            else
            {
                decodedBytes += record.decodedBytes;
            }
        }
    }
    return status;
}

class SceneDecoder
{
public:
    SceneDecoder(const Package& package, const SceneReadLimits& limits, const Cancellation* cancellation) :
        m_package(package), m_limits(limits), m_cancellation(cancellation), m_budget(limits.maxAllocationBytes)
    {}

    template <class T> void Decode(ResourceType type, uint32_t index, T& value)
    {
        ZENASSET_MEASURE_SCOPE(Deserialize);
        const ResourceRecord* record = m_package.Find(detail::ResourceId(type, index));
        if (m_status.Succeeded() && record == nullptr)
        {
            m_status = {StatusCode::InvalidData, "Required scene resource is missing"};
        }
        if (m_status.Succeeded()
            && (record->type != type || !detail::SupportedResourceVersion(type, record->version)
                || !detail::SupportedEncoding(type, record->encoding) || record->flags != RequiredResource))
        {
            m_status = {StatusCode::InvalidData, "Required scene resource identity or schema is invalid"};
        }
        if (m_status.Succeeded() && (record->decodedBytes > m_budget || record->decodedBytes > m_limits.package.maxChunkBytes))
        {
            m_status = {StatusCode::ResourceLimit, "Scene resource buffer exceeds the decoded allocation budget"};
        }
        if (m_status.Succeeded())
        {
            ResourceData data = m_package.Read(record->id, m_cancellation,
                                               std::min(m_budget - record->decodedBytes, m_limits.package.maxCodecBytes));
            m_status          = data.status;
            if (m_status.Succeeded())
            {
                m_budget -= record->decodedBytes;
                ResourceData               expanded;
                std::span<const std::byte> canonical = data.bytes;
                if (record->encoding != 0)
                {
                    expanded = detail::DecodeMeshResource(*record, data.bytes, m_limits, m_budget, m_cancellation);
                    m_status = expanded.status;
                    if (m_status.Succeeded())
                    {
                        canonical  = expanded.bytes;
                        m_budget  -= expanded.bytes.size();
                    }
                }
                if (m_status.Succeeded() && (type == ResourceType::VertexStream || type == ResourceType::IndexStream))
                {
                    if (canonical.size() < 8 || canonical.size() - 8 > m_limits.maxRuntimeGeometryBytes - m_geometryBytes)
                    {
                        m_status = {StatusCode::ResourceLimit, "Canonical mesh streams exceed the runtime geometry budget"};
                    }
                    else
                    {
                        m_geometryBytes += canonical.size() - 8;
                    }
                }
                const SceneReadLimits limits = detail::ResourceLimits(type, m_limits);
                if (m_status.Succeeded())
                {
                    detail::SceneArchive<true> archive(canonical, nullptr, limits, m_budget, m_cancellation, record->version);
                    archive(value);
                    m_status = archive.Finish();
                }
                m_budget += record->decodedBytes + expanded.bytes.size();
            }
        }
    }

    Status ReadManifest(Scene& scene)
    {
        detail::SceneManifest manifest;
        Decode(ResourceType::Scene, 0, manifest);
        if (m_status.Succeeded())
        {
            m_counts = manifest.counts;
            detail::ResourceCounts found{};
            for (const ResourceRecord& record : m_package.Resources())
            {
                const uint32_t type = static_cast<uint32_t>(record.type);
                if ((record.flags & RequiredResource) == 0)
                {
                    // Optional directory entries do not contribute to the
                    // manifest's required runtime resource graph.
                }
                else if (type > 0 && type < detail::ResourceTypeCount)
                {
                    const uint64_t index = record.id & UINT32_MAX;
                    if ((record.id >> 32) != type || index == 0 || index > m_counts[type]
                        || !detail::SupportedResourceVersion(record.type, record.version)
                        || !detail::SupportedEncoding(record.type, record.encoding) || record.flags != RequiredResource)
                    {
                        m_status = {StatusCode::InvalidData,
                                    "Scene directory identity, schema, encoding or required flag is inconsistent"};
                    }
                    ++found[type];
                }
                else if ((record.flags & RequiredResource) != 0)
                {
                    m_status = {StatusCode::UnsupportedVersion, "Unknown required scene resource"};
                }
            }
            for (size_t type = 0; type < detail::ResourceTypeCount; ++type)
            {
                if (m_counts[type] != found[type] || m_counts[type] > UINT32_MAX || m_counts[type] > m_limits.maxArrayElements)
                {
                    m_status = {StatusCode::InvalidData, "Scene manifest resource counts disagree with the directory"};
                }
            }
            for (ResourceType single :
                 {ResourceType::Scene, ResourceType::Metadata, ResourceType::VertexStream, ResourceType::IndexStream,
                  ResourceType::MaterialVariants, ResourceType::TextureCoordinates, ResourceType::SourceDocument})
            {
                if (m_counts[static_cast<size_t>(single)] != 1)
                {
                    m_status = {StatusCode::InvalidData,
                                "Scene package requires exactly one manifest, provenance and bulk stream"};
                }
            }
            if (m_status.Succeeded())
            {
                scene.name               = std::move(manifest.name);
                scene.roots              = std::move(manifest.roots);
                scene.vertexTexCoordSets = std::move(manifest.texCoordSets);
                scene.imageBasedLight    = manifest.imageBasedLight;
            }
        }
        return m_status;
    }

    template <class T> void Array(ResourceType type, std::vector<T>& values)
    {
        const uint64_t count = m_counts[static_cast<size_t>(type)];
        if (m_status.Succeeded() && (count > m_budget / sizeof(T) || count > SIZE_MAX / sizeof(T)))
        {
            m_status = {StatusCode::ResourceLimit, "Scene resource records exceed the allocation budget"};
        }
        if (m_status.Succeeded())
        {
            m_budget -= count * sizeof(T);
            values.resize(static_cast<size_t>(count));
            for (uint32_t index = 0; m_status.Succeeded() && index < count; ++index)
            {
                Decode(type, index, values[index]);
            }
        }
    }
    template <class T> void Single(ResourceType type, T& value)
    {
        Decode(type, 0, value);
    }
    void Metadata(Scene& scene)
    {
        detail::SceneMetadata metadata;
        Decode(ResourceType::Metadata, 0, metadata);
        if (m_status.Succeeded())
        {
            scene.sourcePath   = std::move(metadata.sourcePath);
            scene.dependencies = std::move(metadata.dependencies);
            m_metadata         = std::move(metadata.build);
        }
    }
    Status ResultStatus() const
    {
        return m_status;
    }
    Status Validate(const Scene& scene)
    {
        ZENASSET_MEASURE_SCOPE(SceneValidate);
        if (m_status.Succeeded() && !detail::FitsValidationBudget(scene, m_budget))
        {
            m_status = {StatusCode::ResourceLimit, "Scene validation scratch exceeds the decoded allocation budget"};
        }
        std::string failure;
        if (m_status.Succeeded() && !ValidateSceneAsset(scene, m_limits.maxRuntimeGeometryBytes, &failure))
        {
            m_status = {StatusCode::InvalidData, "Invalid scene resource graph: " + failure};
        }
        return m_status;
    }
    BuildMetadata TakeMetadata()
    {
        return std::move(m_metadata);
    }

private:
    const Package&         m_package;
    const SceneReadLimits& m_limits;
    const Cancellation*    m_cancellation;
    uint64_t               m_budget;
    uint64_t               m_geometryBytes{0};
    Status                 m_status;
    detail::ResourceCounts m_counts{};
    BuildMetadata          m_metadata;
};
} // namespace
SceneReadResult ReadScene(const Package& package, const SceneReadLimits& limits, const Cancellation* cancellation)
{
    SceneReadResult result;
    uint64_t        allocationBytes = 0;
    result.status                   = ValidateReadLimits(package, limits, sizeof(Scene), allocationBytes);
    if (cancellation != nullptr && cancellation->IsCancelled())
    {
        result.status = {StatusCode::Cancelled, "Scene read cancelled"};
    }
    else if (result.status.Succeeded())
    {
        SceneReadLimits charged       = limits;
        charged.maxAllocationBytes   -= allocationBytes;
        std::shared_ptr<Scene> scene  = std::make_shared<Scene>();
        SceneDecoder           decoder(package, charged, cancellation);
        result.status = decoder.ReadManifest(*scene);
        if (result.status.Succeeded())
        {
            detail::VisitSceneResources(decoder, *scene);
            result.status = decoder.ResultStatus();
        }
        if (result.status.Succeeded())
        {
            result.status = decoder.Validate(*scene);
        }
        if (result.status.Succeeded() && cancellation != nullptr && cancellation->IsCancelled())
        {
            result.status = {StatusCode::Cancelled, "Scene read cancelled"};
        }
        if (result.status.Succeeded())
        {
            result.scene    = std::move(scene);
            result.metadata = decoder.TakeMetadata();
        }
    }
    return result;
}
SceneReadResult ReadScene(const std::string& path, const SceneReadLimits& limits, const Cancellation* cancellation)
{
    ZENASSET_MEASURE_SCOPE(Read);
    SceneReadResult     result;
    const PackageResult package = OpenPackage(path, limits.package, cancellation);
    result.status               = package.status;
    if (result.status.Succeeded())
    {
        result = ReadScene(*package.package, limits, cancellation);
    }
    return result;
}
SceneMetadataResult ReadSceneMetadata(const Package& package, const SceneReadLimits& limits, const Cancellation* cancellation)
{
    SceneMetadataResult result;
    uint64_t            allocationBytes = 0;
    result.status                       = ValidateReadLimits(package, limits, sizeof(SceneMetadataResult), allocationBytes);
    if (cancellation != nullptr && cancellation->IsCancelled())
    {
        result.status = {StatusCode::Cancelled, "Scene metadata read cancelled"};
    }
    if (result.status.Succeeded())
    {
        SceneReadLimits charged     = limits;
        charged.maxAllocationBytes -= allocationBytes;
        SceneDecoder          decoder(package, charged, cancellation);
        detail::SceneMetadata metadata;
        decoder.Decode(ResourceType::Metadata, 0, metadata);
        result.status = decoder.ResultStatus();
        if (result.status.Succeeded() && cancellation != nullptr && cancellation->IsCancelled())
        {
            result.status = {StatusCode::Cancelled, "Scene metadata read cancelled"};
        }
        if (result.status.Succeeded())
        {
            result.metadata     = std::move(metadata.build);
            result.sourcePath   = std::move(metadata.sourcePath);
            result.dependencies = std::move(metadata.dependencies);
        }
    }
    return result;
}
SceneMetadataResult ReadSceneMetadata(const std::string& path, const SceneReadLimits& limits, const Cancellation* cancellation)
{
    SceneMetadataResult result;
    const PackageResult package = OpenPackage(path, limits.package, cancellation);
    result.status               = package.status;
    if (result.status.Succeeded())
    {
        result = ReadSceneMetadata(*package.package, limits, cancellation);
    }
    return result;
}
} // namespace zenasset
