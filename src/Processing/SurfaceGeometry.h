#pragma once
#include "Format/MathOps.h"
#include "ZenAsset/Format/Scene.h"
#include <cmath>

namespace zenasset
{
// Requires validated triangle indices and one source mapping per input vertex.
void GenerateFlatNormals(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices, std::vector<uint32_t>& sourceVertices);

inline bool NormalizeSurfaceNormal(Vec3& normal)
{
    const float lengthSquared = Dot(normal, normal);

    const bool valid          = std::isfinite(lengthSquared) && lengthSquared > 1e-12f;

    if (valid)
    {
        normal /= std::sqrt(lengthSquared);
    }

    return valid;
}

} // namespace zenasset
