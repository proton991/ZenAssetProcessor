#pragma once
#include "ZenAsset/Format/BuildMetadata.h"

namespace zenasset::detail
{
// Caller owns a validated scene copy. Failed/cancelled passes discard that copy;
// immutable source cubes remain owned and unmodified by the input scene.
Status ProcessEnvironments(Scene&                   scene,
                           const TextureSettings&   settings,
                           uint64_t&                remaining,
                           std::vector<Diagnostic>& diagnostics,
                           const Cancellation*      cancellation);
} // namespace zenasset::detail
