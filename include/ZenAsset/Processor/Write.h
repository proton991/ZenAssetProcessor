#pragma once
#include "ZenAsset/Format/BuildMetadata.h"
#include "ZenAsset/Reader/Scene.h"

namespace zenasset
{
struct ScenePartWriteStats
{
    uint64_t imagesEncoded{0};
    uint64_t imagesReused{0};
    uint64_t partBytesWritten{0};
    bool     geometryReused{false};
};
struct SceneWriteOptions
{
    BuildMetadata   metadata;
    SceneReadLimits limits;
    // Authoritative compression policy, also written to build metadata.
    CompressionSettings compression;
    // Encoder workspace bound. maxAllocationBytes additionally bounds directory,
    // serialized resource, compressed output, and this workspace together.
    uint64_t maxEncoderBytes{256ull << 20};
    bool     durable{false};
    // Optional synchronous check after the temporary file is closed, immediately
    // before replacement. Failure removes that temporary and preserves old output.
    void*  publicationUser{nullptr};
    Status (*beforePublish)(void*){nullptr};
    // Set false on entry and true once replacement commits, including a later
    // durability error. The pointed value must outlive this synchronous call.
    bool* published{nullptr};
    // Synchronous resource counts, starting at zero. All resources written does
    // not imply atomic publication has completed. The callback may cancel.
    void* progressUser{nullptr};
    void  (*resourceProgress)(void*, uint64_t completed, uint64_t total){nullptr};
    // Independent image compression jobs share the byte and encoder budgets.
    // One keeps the writer serial. Output ordering and bytes are identical.
    uint32_t maxImageWorkers{4};
    bool                 directoryLayout{false};
    ScenePartWriteStats* partStats{nullptr};
    // Encoded byte total is unknown until compression finishes. completedBytes
    // counts bytes actually written to parts, including staging.
    void (*partProgress)(void*, const SceneTransferState&){nullptr};
};
// Resolve the cache variant and claim its model directory for this source.
Status SceneDirectoryManifest(const std::string&         cacheRoot,
                              const std::string&         source,
                              const BuildMetadata&       metadata,
                              const CompressionSettings& compression,
                              std::string&               manifestPath);

Status TrimSceneDirectoryCache(const std::string& cacheRoot, uint64_t byteBudget, const std::string& keep);
Status MigrateSceneDirectory(const std::string&             destination,
                             std::shared_ptr<const Package> source,
                             const SceneWriteOptions&       options,
                             const Cancellation*            cancellation = nullptr);
Status RecordDirectoryInputs(const Scene& scene, BuildMetadata& metadata);
Status VerifyDirectoryInputs(const BuildMetadata& metadata, const Cancellation* cancellation);

// Writes an exclusive temporary sibling and replaces the destination only after
// complete serialization and flush. Prepublication failure/cancellation preserves
// old output. A durability failure after replacement can report IOError with the
// new complete file already published; the status message identifies that case.
Status WriteScene(const std::string&       path,
                  const Scene&             scene,
                  const SceneWriteOptions& options      = {},
                  const Cancellation*      cancellation = nullptr);
} // namespace zenasset
