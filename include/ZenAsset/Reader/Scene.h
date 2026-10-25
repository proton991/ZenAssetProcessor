#pragma once
#include "ZenAsset/Format/BuildMetadata.h"
#include "ZenAsset/Reader/Package.h"

namespace zenasset
{
struct SceneReadLimits
{
    ReadLimits package;
    // Requested storage for directory, decoded objects/arrays/strings, the
    // current chunk and validation scratch. Excludes allocator bookkeeping,
    // fixed stack objects, file-library buffers and caller-owned inputs.
    uint64_t maxAllocationBytes{24ull << 30};
    uint64_t maxArrayElements{1ull << 30};
    uint64_t maxStringBytes{16ull << 20};
    uint64_t maxSourceDocumentBytes{1ull << 30};
    uint64_t maxRuntimeGeometryBytes{16ull << 30};
};
struct SceneReadResult
{
    Status                       status;
    std::shared_ptr<const Scene> scene;
    BuildMetadata                metadata;
};
struct SceneMetadataResult
{
    Status                       status;
    BuildMetadata                metadata;
    std::string                  sourcePath;
    std::vector<SceneDependency> dependencies;
};
SceneReadResult ReadScene(const std::string&     path,
                          const SceneReadLimits& limits       = {},
                          const Cancellation*    cancellation = nullptr);
SceneReadResult ReadScene(const Package&         package,
                          const SceneReadLimits& limits       = {},
                          const Cancellation*    cancellation = nullptr);
// Validates and decodes provenance only; does not read runtime resource payloads
// or touch source dependencies. Full scene graph validation remains ReadScene's job.
SceneMetadataResult ReadSceneMetadata(const std::string&     path,
                                      const SceneReadLimits& limits       = {},
                                      const Cancellation*    cancellation = nullptr);
SceneMetadataResult ReadSceneMetadata(const Package&         package,
                                      const SceneReadLimits& limits       = {},
                                      const Cancellation*    cancellation = nullptr);
} // namespace zenasset
