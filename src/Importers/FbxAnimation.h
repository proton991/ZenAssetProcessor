#pragma once
#include "Importers/ImportErrors.h"
#include "Importers/FbxConversions.h"

namespace zenasset
{
// Shared by initial material conversion and evaluated animation samples.
void SetFbxMaterialFactors(const ufbx_material& source, MaterialAsset& material);
void BakeFbxAnimations(const ufbx_scene&                   source,
                       Scene&                              asset,
                       const AnimationBakeSettings&        settings,
                       ImportErrors&                       errors,
                       std::vector<SceneImportDiagnostic>& diagnostics);
} // namespace zenasset
