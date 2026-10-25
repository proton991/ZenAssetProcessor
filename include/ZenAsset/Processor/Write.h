#pragma once
#include "ZenAsset/Format/BuildMetadata.h"
#include "ZenAsset/Reader/Scene.h"

namespace zenasset
{
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
};
// Writes an exclusive temporary sibling and replaces the destination only after
// complete serialization and flush. Prepublication failure/cancellation preserves
// old output. A durability failure after replacement can report IOError with the
// new complete file already published; the status message identifies that case.
Status WriteScene(const std::string&       path,
                  const Scene&             scene,
                  const SceneWriteOptions& options      = {},
                  const Cancellation*      cancellation = nullptr);
} // namespace zenasset
