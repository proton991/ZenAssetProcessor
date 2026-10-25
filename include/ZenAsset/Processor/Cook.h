#pragma once
#include "ZenAsset/Format/Processing.h"
#include "ZenAsset/Reader/Scene.h"

namespace zenasset
{
struct SceneCookOptions
{
    ProcessingSettings settings;
    // New copied record storage, persistent processing additions and concurrent
    // scratch count toward maxAllocationBytes. Caller-owned input and shared
    // immutable image/environment pixels do not. Allocator bookkeeping is excluded.
    SceneReadLimits limits;
};

struct SceneCookResult
{
    Status                       status;
    std::shared_ptr<const Scene> scene;
    std::vector<Diagnostic>      diagnostics;
};

// ImportScene normalizes source data; CookScene applies explicit processing
// policy once. Adapt this same result directly or pass it to WriteScene.
// Disabled processing shares the input owner. Failure never exposes a partial
// scene and never mutates the caller's input. Calls are synchronous and cancellable.
SceneCookResult CookScene(const std::shared_ptr<const Scene>& input,
                          const SceneCookOptions&             options      = {},
                          const Cancellation*                 cancellation = nullptr);
} // namespace zenasset
