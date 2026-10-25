#pragma once
#include "ZenAsset/Format/BuildMetadata.h"

namespace zenasset::detail
{
// Requires a validated scene. CookScene owns the working copy and publishes it
// only after all passes succeed. Texture/material IDs and authored UV state stay
// unchanged; image references and texture layer/region selections are replaced.
Status ProcessTexturePages(Scene&                   scene,
                           const TextureSettings&   settings,
                           uint64_t&                remaining,
                           std::vector<Diagnostic>& diagnostics,
                           const Cancellation*      cancellation = nullptr);
} // namespace zenasset::detail
