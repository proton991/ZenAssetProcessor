#pragma once
#include "ZenAsset/Processor/Cook.h"

namespace zenasset::detail
{
// Internal import pipeline only: input must originate from ImportScene, whose
// owning Scene allocation is mutable before publication. Transfer the owner's
// last reference to cook in place; a shared owner falls back to an immutable copy.
// Failure consumes the input and never exposes its partially processed scene.
SceneCookResult ConsumeCookScene(std::shared_ptr<const Scene> input,
                                 const SceneCookOptions&      options      = {},
                                 const Cancellation*          cancellation = nullptr);
} // namespace zenasset::detail
