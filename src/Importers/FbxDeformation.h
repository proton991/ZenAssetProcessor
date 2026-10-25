#pragma once
#include "Importers/ImportErrors.h"
#include "Importers/FbxConversions.h"

namespace zenasset
{
void ValidateFbxDeformers(const ufbx_scene& source, ImportErrors& errors);
// Called by independent mesh workers. All source arrays are immutable.
void ConvertFbxMeshDeformation(const ufbx_mesh&     source,
                               MeshAsset&           mesh,
                               std::vector<Vertex>& vertices,
                               MorphPrimitiveAsset& morph,
                               ImportErrors&        errors);
// Called after nodes/meshes are assembled in source order.
void BuildFbxSkins(const ufbx_scene& source, Scene& asset, ImportErrors& errors);
} // namespace zenasset
