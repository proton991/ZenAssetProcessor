#pragma once
#include "ZenAsset/Format/Assembly.h"
#include "ZenAsset/Processor/Write.h"

namespace zenasset
{
// Existing container, one required Assembly v1 resource. Atomic replacement and
// beforePublish/published follow WriteScene. Referenced models are not rewritten.
Status WriteAssembly(const std::string&       path,
                     const SceneAssembly&     assembly,
                     const SceneWriteOptions& options      = {},
                     const Cancellation*      cancellation = nullptr);

struct AssemblyCleanupOptions
{
    bool            dryRun{true};
    uint64_t        maxEntries{100000};
    uint64_t        maxPlanningBytes{64ull << 20};
    SceneReadLimits limits;
};
struct AssemblyCleanupResult
{
    Status                   status;
    bool                     dryRun{true};
    uint64_t                 candidateBytes{0};
    std::vector<std::string> candidates;
    // A filesystem failure can occur after earlier deletions; these report actual progress.
    std::vector<std::string> deleted;
};
// Scans every .zenscene below root and verifies all referenced native packages.
// Only unreferenced, valid identity-named model files directly under root/models
// are eligible. Symlinks/reparse points are rejected. Same-process assembly builds
// are serialized; other processes must coordinate publishing and cleanup externally.
AssemblyCleanupResult CleanupAssemblies(const std::string&            root,
                                        const AssemblyCleanupOptions& options      = {},
                                        const Cancellation*           cancellation = nullptr);
} // namespace zenasset
