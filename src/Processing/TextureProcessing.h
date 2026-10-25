#pragma once
#include "ZenAsset/Format/BuildMetadata.h"

namespace zenasset::detail
{
// The caller owns a validated mutable scene copy and discards it on failure.
// Material/animation identities stay fixed. Bound texture variants append stable
// IDs; image IDs are remapped only after all immutable source owners were used.
Status ProcessTextures(Scene&                   scene,
                       const TextureSettings&   settings,
                       uint64_t&                remainingBytes,
                       std::vector<Diagnostic>& diagnostics,
                       const Cancellation*      cancellation);
} // namespace zenasset::detail
