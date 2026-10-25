#pragma once
#include "ZenAsset/Format/Processing.h"
#include "ZenAsset/Format/BuildMetadata.h"

namespace zenasset::detail
{
// Caller owns a validated mutable scene copy and discards it on failure.
Status ProcessGeometry(Scene&                   scene,
                       const GeometrySettings&  settings,
                       uint64_t&                remainingBytes,
                       std::vector<Diagnostic>& diagnostics,
                       const Cancellation*      cancellation);
} // namespace zenasset::detail
