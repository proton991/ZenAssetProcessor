#include "Processing/SurfaceGeometry.h"

namespace zenasset
{
void GenerateFlatNormals(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices, std::vector<uint32_t>& sourceVertices)
{
    // Indexed vertices can belong to faces with different normals. Split them so both
    // the raster path and voxel attribute resolve receive the same flat-shaded surface.
    std::vector<Vertex> flatVertices(indices.size());

    std::vector<uint32_t> flatSourceVertices(indices.size());

    for (size_t first = 0; first < indices.size(); first += 3)
    {
        const Vec3 a(vertices[indices[first]].pos);

        const Vec3 b(vertices[indices[first + 1]].pos);

        const Vec3 c(vertices[indices[first + 2]].pos);

        Vec3 normal = Cross(b - a, c - a);

        if (!NormalizeSurfaceNormal(normal))
        {
            normal = Vec3(0, 1, 0);
        }

        for (size_t corner = 0; corner < 3; ++corner)
        {
            const size_t index          = first + corner;

            flatVertices[index]         = vertices[indices[index]];

            flatSourceVertices[index]   = sourceVertices[indices[index]];

            flatVertices[index].normal  = Vec4(normal, 0);

            flatVertices[index].tangent = Vec4(0);

            indices[index]              = static_cast<uint32_t>(index);
        }
    }

    vertices       = std::move(flatVertices);

    sourceVertices = std::move(flatSourceVertices);
}

} // namespace zenasset
