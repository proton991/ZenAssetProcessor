#pragma once
#include "Processor/Backend.h"

namespace zenasset
{
// Shared processing every source backend runs before publishing its asset. Clips
// that cannot play and morph weight lists that do not match their targets are
// dropped with a warning instead of rejecting the scene; the result is then
// validated once. A failure is recorded in result with the invalid area named.
// Geometry admission covers the normalization's mesh-count and animation-header
// scratch. General graph-validation metadata is outside that narrower contract.
bool ProcessImportedAsset(Scene&             asset,
                          SceneImportResult& result,
                          const std::string& source,
                          GeometryBudget*    geometry = nullptr);
} // namespace zenasset
