#include "Importers/ObjSceneImporter.h"
#include "Diagnostics/StageMeasurements.h"
#include "Importers/ObjImport.h"
#include "Importers/AssetHash.h"
#include "Importers/AssetPaths.h"
#include "Processing/SceneAssetProcessing.h"
#include "Processing/SurfaceGeometry.h"
#include <mapbox/earcut.hpp>
#include <array>
#include <charconv>
#include <fstream>
#include <sstream>
#include <tuple>

namespace zenasset
{
bool ParseObjNumbers(const std::string& text, std::vector<float>& values)
{
    bool               valid = true;
    std::istringstream stream(text);
    std::string        token;
    while (valid && stream >> token)
    {
        float                        value  = 0;
        const char*                  first  = token.data() + (token[0] == '+' ? 1 : 0);
        const std::from_chars_result parsed = std::from_chars(first, token.data() + token.size(), value);
        valid =
            parsed.ec == std::errc() && parsed.ptr == token.data() + token.size() && std::isfinite(value) && values.size() < 8;
        if (valid)
        {
            values.push_back(value);
        }
    }
    return valid;
}
ObjImportState::ObjImportState(const SceneImportRequest& source,
                               const BackendContext&     context,
                               SceneImportResult&        result,
                               Scene&                    asset) :
    request(source),
    report(result),
    scene(asset),
    remainingSourceBytes(context.maxSourceBytes),
    remainingGeometryBytes(context.maxObjGeometryBytes)
{
    errors.Reset(source.control.get(), context.images);
}
bool ObjImportState::Charge(uint64_t count, uint64_t stride)
{
    const bool valid = stride == 0 || count <= remainingGeometryBytes / stride;
    if (valid)
    {
        remainingGeometryBytes -= count * stride;
    }
    else
    {
        errors.Fail("OBJ normalized source and geometry exceed maxObjGeometryBytes", StatusCode::ResourceLimit);
    }
    return valid && !errors.Failed();
}
void ObjImportState::Warn(const std::string& object, const std::string& message)
{
    if (Charge(1, sizeof(Diagnostic) + request.path.size() + object.size() + message.size()))
    {
        report.diagnostics.push_back(
            {DiagnosticSeverity::Warning, StatusCode::UnsupportedFeature, request.path, object, message});
    }
}
void ObjImportState::CompleteDependency(const std::filesystem::path& path)
{
    if (request.control && !errors.Failed())
    {
        std::error_code error;
        const bool      source = std::filesystem::equivalent(path, std::filesystem::u8path(request.path), error);
        if (!source && !error)
        {
            request.control->CompleteFile(AbsoluteUtf8Path(path));
        }
    }
}
bool ObjImportState::ReadFile(const std::filesystem::path& path, std::string& bytes, bool optional)
{
    const std::string absolute = AbsoluteUtf8Path(path);
    AssetFileStamp    stamp;
    bool              valid = !errors.Failed() && StampAssetFile(absolute, stamp);
    if (!valid && !errors.Failed())
    {
        scene.dependencies.push_back({absolute, false});
        if (!optional)
        {
            errors.Fail("OBJ dependency is unavailable: " + absolute, StatusCode::SourceUnavailable);
        }
    }
    if (valid && (stamp.size > remainingSourceBytes || stamp.size > SIZE_MAX))
    {
        errors.Fail("OBJ source and dependencies exceed maxSourceBytes: " + absolute, StatusCode::ResourceLimit);
        valid = false;
    }
    if (valid)
    {
        remainingSourceBytes -= stamp.size;
        bytes.resize(static_cast<size_t>(stamp.size));
        std::ifstream stream(std::filesystem::u8path(absolute), std::ios::binary);
        size_t        offset = 0;
        while (stream && !errors.Failed() && offset < bytes.size())
        {
            const size_t count = std::min<size_t>(128 * 1024, bytes.size() - offset);
            stream.read(bytes.data() + offset, static_cast<std::streamsize>(count));
            offset += count;
        }
        AssetFileStamp after;
        valid = stream && !errors.Failed() && StampAssetFile(absolute, after) && after.size == stamp.size
             && after.modified == stamp.modified;
        if (valid)
        {
            scene.dependencies.push_back({absolute, true, stamp.size, stamp.modified, 0});
            if (request.control)
            {
                request.control->RegisterFile(absolute);
            }
        }
        else if (!errors.Failed())
        {
            errors.Fail("OBJ dependency changed or could not be read: " + absolute, StatusCode::IOError);
        }
    }
    return valid;
}

namespace
{
struct ObjRun
{
    std::string object;
    std::string groups;
    std::string material;
    uint32_t    objectIndex{0};
    char        topology{'f'};
};
struct ObjDocument
{
    std::string         normalized;
    std::vector<ObjRun> runs;
};
std::string Trim(std::string text)
{
    const size_t first = text.find_first_not_of(" \t\r");
    const size_t last  = text.find_last_not_of(" \t\r");
    return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
}
std::vector<std::string> MaterialFiles(const std::string& text)
{
    std::vector<std::string> files;
    std::string              current;
    for (size_t index = 0; index < text.size(); ++index)
    {
        const char value = text[index];
        if (value == '\\' && index + 1 < text.size() && (text[index + 1] == ' ' || text[index + 1] == '\t'))
        {
            current += text[++index];
        }
        else if (value == ' ' || value == '\t')
        {
            if (!current.empty())
            {
                files.push_back(std::move(current));
                current.clear();
            }
        }
        else
        {
            current += value;
        }
    }
    if (!current.empty())
    {
        files.push_back(std::move(current));
    }
    return files;
}
std::string ObjLine(std::string text)
{
    const size_t comment = text.find('#');
    if (comment != std::string::npos)
    {
        text.resize(comment);
    }
    return Trim(std::move(text));
}
std::string EscapeMaterialFile(const std::string& file)
{
    std::string result;
    for (char value : file)
    {
        if (value == ' ' || value == '\t')
        {
            result += '\\';
        }
        result += value;
    }
    return result;
}
ObjDocument Preflight(ObjImportState& state, const std::string& bytes)
{
    ObjDocument result;
    ObjRun      active;
    bool        changed        = true;
    uint64_t    sourceVertices = 0;
    uint64_t    totalCorners   = 0;
    if (bytes.find('\0') != std::string::npos)
    {
        state.errors.Fail("OBJ source contains a NUL byte");
    }
    if (state.Charge(bytes.size(), 8))
    {
        result.normalized.reserve(bytes.size());
    }
    std::istringstream input;
    if (!state.errors.Failed())
    {
        input.str(bytes);
    }
    std::string line;
    while (!state.errors.Failed() && std::getline(input, line))
    {
        if (line.starts_with("\xef\xbb\xbf"))
        {
            line.erase(0, 3);
        }
        line = ObjLine(std::move(line));
        while (!line.empty() && line.back() == '\\' && !state.errors.Failed())
        {
            line.pop_back();
            std::string continuation;
            if (!std::getline(input, continuation))
            {
                state.errors.Fail("Unterminated OBJ line continuation");
            }
            line += " " + ObjLine(std::move(continuation));
        }
        std::istringstream tokens(line);
        std::string        command;
        tokens >> command;
        std::string rest;
        std::getline(tokens, rest);
        rest = Trim(std::move(rest));
        if (command == "o" || command == "g" || command == "usemtl")
        {
            if (command == "o")
            {
                active.object = rest;
                ++active.objectIndex;
            }
            else if (command == "g")
            {
                active.groups = rest;
            }
            else
            {
                active.material = rest;
            }
            changed = true;
        }
        else if (command == "mtllib")
        {
            // tinyobj treats names on one mtllib line as alternatives; OBJ names
            // are all libraries. Separate statements load every authored file.
            for (const std::string& file : MaterialFiles(rest))
            {
                result.normalized += "mtllib " + EscapeMaterialFile(file) + "\n";
            }
        }
        else if (command == "f" || command == "l" || command == "p")
        {
            std::istringstream corners(rest);
            std::string        corner;
            uint64_t           count = 0;
            while (!state.errors.Failed() && count <= 4096 && corners >> corner)
            {
                ++count;
            }
            const uint64_t minimum = command == "f" ? 3 : command == "l" ? 2 : 1;
            if (count < minimum)
            {
                state.errors.Fail("OBJ primitive has too few corners");
            }
            else if (count > 4096 || totalCorners > UINT32_MAX / 3 - count)
            {
                state.errors.Fail("OBJ primitive exceeds the corner limit (maximum 4096)", StatusCode::ResourceLimit);
            }
            else if (state.Charge(count, sizeof(Vertex) + 3 * sizeof(uint32_t) + 192))
            {
                totalCorners += count;
                if (changed || active.topology != command[0])
                {
                    if (state.Charge(1, 1024 + active.object.size() + active.groups.size() + active.material.size()))
                    {
                        active.topology    = command[0];
                        result.normalized += "o __zen_obj_run_" + std::to_string(result.runs.size()) + "\n";
                        result.runs.push_back(active);
                        changed = false;
                    }
                }
                result.normalized += command + " " + rest + "\n";
            }
        }
        else if (command == "v" || command == "vn" || command == "vt" || command == "s")
        {
            if (command == "s")
            {
                if (rest == "on")
                {
                    rest = "1";
                }
                if (rest != "off")
                {
                    uint32_t                     smoothing = 0;
                    const std::from_chars_result parsed    = std::from_chars(rest.data(), rest.data() + rest.size(), smoothing);
                    if (parsed.ec != std::errc() || parsed.ptr != rest.data() + rest.size())
                    {
                        state.errors.Fail("Invalid OBJ smoothing group");
                    }
                }
            }
            else
            {
                std::vector<float> values;
                bool               valid  = ParseObjNumbers(rest, values);
                valid                    &= command == "v"  ? (values.size() == 3 || values.size() == 4 || values.size() == 6)
                                          : command == "vn" ? values.size() == 3
                                                            : !values.empty() && values.size() <= 3;
                if (!valid)
                {
                    state.errors.Fail("Invalid numeric OBJ attribute: " + command);
                }
                else if ((command == "v" && values.size() == 4 && values[3] != 1)
                         || (command == "vt" && values.size() == 3 && values[2] != 0))
                {
                    state.errors.Fail("Projective OBJ position/texture coordinates are unsupported",
                                      StatusCode::UnsupportedFeature);
                }
            }
            if (command == "v")
            {
                ++sourceVertices;
                if (sourceVertices > INT32_MAX)
                {
                    state.errors.Fail("OBJ position count exceeds signed index range", StatusCode::ResourceLimit);
                }
            }
            result.normalized += command + " " + rest + "\n";
        }
        else if (command == "curv" || command == "curv2" || command == "surf" || command == "vw")
        {
            state.errors.Fail("Unsupported OBJ geometry statement: " + command, StatusCode::UnsupportedFeature);
        }
        else if (!command.empty())
        {
            state.Warn(command, "Unsupported OBJ statement was ignored");
        }
    }
    if (!state.errors.Failed() && result.runs.empty())
    {
        state.errors.Fail("OBJ has no supported primitives");
    }
    return result;
}

using Point2 = std::array<double, 2>;
double Area2(const Point2& a, const Point2& b, const Point2& c)
{
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
}
bool SegmentsIntersect(const Point2& a, const Point2& b, const Point2& c, const Point2& d, double epsilon)
{
    const double abC = Area2(a, b, c);
    const double abD = Area2(a, b, d);
    const double cdA = Area2(c, d, a);
    const double cdB = Area2(c, d, b);
    const bool   boxes =
        std::max(a[0], b[0]) + epsilon >= std::min(c[0], d[0]) && std::max(c[0], d[0]) + epsilon >= std::min(a[0], b[0])
        && std::max(a[1], b[1]) + epsilon >= std::min(c[1], d[1]) && std::max(c[1], d[1]) + epsilon >= std::min(a[1], b[1]);
    return boxes && ((abC >= -epsilon && abD <= epsilon) || (abD >= -epsilon && abC <= epsilon))
        && ((cdA >= -epsilon && cdB <= epsilon) || (cdB >= -epsilon && cdA <= epsilon));
}
std::vector<uint32_t> Triangulate(ObjImportState& state, const std::vector<Vertex>& vertices, Vec3& normal)
{
    std::vector<uint32_t> triangles;
    std::array<double, 3> newell{};
    Bounds                bounds;
    for (size_t index = 0; index < vertices.size(); ++index)
    {
        const Vec3 a(vertices[index].pos);
        const Vec3 b(vertices[(index + 1) % vertices.size()].pos);
        newell[0] += (static_cast<double>(a.y) - b.y) * (static_cast<double>(a.z) + b.z);
        newell[1] += (static_cast<double>(a.z) - b.z) * (static_cast<double>(a.x) + b.x);
        newell[2] += (static_cast<double>(a.x) - b.x) * (static_cast<double>(a.y) + b.y);
        bounds.SetMin(a);
        bounds.SetMax(a);
    }
    const double length   = std::sqrt(newell[0] * newell[0] + newell[1] * newell[1] + newell[2] * newell[2]);
    const double diagonal = Length(bounds.max - bounds.min);
    if (!std::isfinite(length) || length <= diagonal * diagonal * 1e-12)
    {
        state.errors.Fail("OBJ polygon is degenerate or self-intersecting");
    }
    else
    {
        normal      = Vec3(static_cast<float>(newell[0] / length), static_cast<float>(newell[1] / length),
                           static_cast<float>(newell[2] / length));
        size_t axis = 0;
        if (std::abs(newell[1]) > std::abs(newell[axis]))
        {
            axis = 1;
        }
        if (std::abs(newell[2]) > std::abs(newell[axis]))
        {
            axis = 2;
        }
        std::vector<std::vector<Point2>> polygon(1);
        for (const Vertex& vertex : vertices)
        {
            const Vec3   point(vertex.pos);
            const Vec3   delta    = point - Vec3(vertices[0].pos);
            const double distance = std::abs(delta.x * newell[0] + delta.y * newell[1] + delta.z * newell[2]) / length;
            if (distance > std::max(1e-7, diagonal * 1e-6))
            {
                state.errors.Fail("Nonplanar OBJ polygon cannot be triangulated without changing its surface");
            }
            polygon[0].push_back({point[(axis + 1) % 3], point[(axis + 2) % 3]});
        }
        const double epsilon = std::max(1e-20, diagonal * diagonal * 1e-12);
        for (size_t a = 0; !state.errors.Failed() && a < vertices.size(); ++a)
        {
            const size_t b = (a + 1) % vertices.size();
            for (size_t c = a + 1; !state.errors.Failed() && c < vertices.size(); ++c)
            {
                const size_t d = (c + 1) % vertices.size();
                if (c != b && d != a && SegmentsIntersect(polygon[0][a], polygon[0][b], polygon[0][c], polygon[0][d], epsilon))
                {
                    state.errors.Fail("Self-intersecting OBJ polygon is unsupported");
                }
            }
        }
        if (!state.errors.Failed())
        {
            triangles      = vertices.size() == 3 ? std::vector<uint32_t>{0, 1, 2} : mapbox::earcut<uint32_t>(polygon);
            double covered = 0;
            double area    = 0;
            for (size_t index = 0; index < vertices.size(); ++index)
            {
                const Point2& a  = polygon[0][index];
                const Point2& b  = polygon[0][(index + 1) % vertices.size()];
                area            += a[0] * b[1] - a[1] * b[0];
            }
            for (size_t index = 0; index + 2 < triangles.size(); index += 3)
            {
                double triangleArea =
                    Area2(polygon[0][triangles[index]], polygon[0][triangles[index + 1]], polygon[0][triangles[index + 2]]);
                if (triangleArea * area < 0)
                {
                    std::swap(triangles[index + 1], triangles[index + 2]);
                    triangleArea = -triangleArea;
                }
                covered += triangleArea;
            }
            if (triangles.empty() || std::abs(covered - area) > std::max(epsilon, std::abs(area) * 1e-8))
            {
                state.errors.Fail("OBJ polygon triangulation did not preserve its area");
            }
        }
    }
    return triangles;
}

bool ReadCorner(ObjImportState& state, const tinyobj::attrib_t& source, const tinyobj::index_t& index, Vertex& vertex)
{
    bool valid = index.vertex_index >= 0 && static_cast<size_t>(index.vertex_index) < source.vertices.size() / 3
              && index.normal_index >= -1 && index.texcoord_index >= -1
              && (index.normal_index < 0 || static_cast<size_t>(index.normal_index) < source.normals.size() / 3)
              && (index.texcoord_index < 0 || static_cast<size_t>(index.texcoord_index) < source.texcoords.size() / 2);
    if (valid)
    {
        vertex.pos = Vec4(MakeVec3(source.vertices.data() + size_t(index.vertex_index) * 3), 1);
        if (index.normal_index >= 0)
        {
            vertex.normal = Vec4(MakeVec3(source.normals.data() + size_t(index.normal_index) * 3), 0);
        }
        if (index.texcoord_index >= 0)
        {
            vertex.uv0   = MakeVec2(source.texcoords.data() + size_t(index.texcoord_index) * 2);
            vertex.uv0.y = 1 - vertex.uv0.y;
        }
        if (static_cast<size_t>(index.vertex_index) < source.colors.size() / 3)
        {
            vertex.color = Vec4(MakeVec3(source.colors.data() + size_t(index.vertex_index) * 3), 1);
        }
        valid = IsFinite(vertex.pos) && IsFinite(vertex.normal) && IsFinite(vertex.uv0) && IsFinite(vertex.color);
    }
    if (!valid)
    {
        state.errors.Fail("OBJ corner has an invalid index or non-finite attribute");
    }
    return valid;
}

using SmoothKey = std::tuple<uint32_t, uint32_t, int>;
bool RequiresCoordinates(const MaterialAsset& material)
{
    bool required = material.m_pBaseColorTexture.IsValid() || material.m_pMetallicRoughnessTexture.IsValid()
                 || material.m_pNormalTexture.IsValid() || material.m_pEmissiveTexture.IsValid();
    for (MaterialFeatures::TextureMember member : MaterialFeatures::textureMembers)
    {
        required |= (material.features.*member).texture.IsValid();
    }
    return required;
}
struct SmoothCorner
{
    uint32_t  vertex;
    SmoothKey key;
};
void ReadGeometry(ObjImportState&                         state,
                  const ObjDocument&                      document,
                  const tinyobj::attrib_t&                source,
                  const std::vector<tinyobj::shape_t>&    shapes,
                  const std::vector<tinyobj::material_t>& materials)
{
    ZENASSET_MEASURE_SCOPE(SourceMeshes);
    Scene&                     scene = state.scene;
    std::map<SmoothKey, Vec3>  smoothNormals;
    std::vector<SmoothCorner>  smoothCorners;
    std::map<uint32_t, NodeId> objectNodes;
    scene.vertexTexCoordSets.push_back(0);
    for (const tinyobj::shape_t& shape : shapes)
    {
        constexpr std::string_view prefix   = "__zen_obj_run_";
        size_t                     runIndex = SIZE_MAX;
        if (shape.name.starts_with(prefix))
        {
            const char*                  first  = shape.name.data() + prefix.size();
            const std::from_chars_result parsed = std::from_chars(first, shape.name.data() + shape.name.size(), runIndex);
            if (parsed.ec != std::errc() || parsed.ptr != shape.name.data() + shape.name.size())
            {
                runIndex = SIZE_MAX;
            }
        }
        if (runIndex >= document.runs.size())
        {
            state.errors.Fail("OBJ parser changed primitive framing");
        }
        if (!state.errors.Failed())
        {
            const ObjRun& run = document.runs[runIndex];
            MeshAsset     mesh;
            mesh.name = run.groups.empty() ? run.object : run.groups;
            MeshPrimitiveAsset primitive;
            primitive.name           = mesh.name;
            primitive.firstVertex    = static_cast<uint32_t>(scene.vertices.size());
            primitive.firstIndex     = static_cast<uint32_t>(scene.indices.size());
            primitive.material.value = 0;
            for (uint32_t material = 0; material < materials.size(); ++material)
            {
                if (materials[material].name == run.material)
                {
                    primitive.material.value = material + 1;
                }
            }
            if (!run.material.empty() && primitive.material.value == 0)
            {
                state.Warn(run.material, "Unknown OBJ usemtl name; using the default material");
            }
            const bool requireCoordinates                = RequiresCoordinates(scene.materials[primitive.material.value]);
            primitive.topology                           = run.topology == 'f' ? MeshTopology::Triangles
                                                         : run.topology == 'l' ? MeshTopology::Lines
                                                                               : MeshTopology::Points;
            const std::vector<tinyobj::index_t>& corners = run.topology == 'f' ? shape.mesh.indices
                                                         : run.topology == 'l' ? shape.lines.indices
                                                                               : shape.points.indices;
            size_t                               offset  = 0;
            size_t                               primitiveIndex = 0;
            while (!state.errors.Failed() && offset < corners.size())
            {
                const size_t count = run.topology == 'f' ? shape.mesh.num_face_vertices[primitiveIndex]
                                   : run.topology == 'l' ? static_cast<size_t>(shape.lines.num_line_vertices[primitiveIndex])
                                                         : corners.size();
                if (count == 0 || count > corners.size() - offset)
                {
                    state.errors.Fail("OBJ parser returned an invalid primitive range");
                }
                std::vector<Vertex> vertices(count);
                for (size_t index = 0; !state.errors.Failed() && index < count; ++index)
                {
                    ReadCorner(state, source, corners[offset + index], vertices[index]);
                    if (requireCoordinates && corners[offset + index].texcoord_index < 0)
                    {
                        state.errors.Fail("OBJ textured material requires UVs on every referenced corner: " + run.material);
                    }
                }
                Vec3                  normal;
                std::vector<uint32_t> indices;
                if (!state.errors.Failed() && run.topology == 'f')
                {
                    indices = Triangulate(state, vertices, normal);
                }
                else if (run.topology == 'l')
                {
                    for (uint32_t index = 1; index < count; ++index)
                    {
                        indices.push_back(index - 1);
                        indices.push_back(index);
                    }
                }
                else if (run.topology == 'p')
                {
                    for (uint32_t index = 0; index < count; ++index)
                    {
                        indices.push_back(index);
                    }
                }
                const uint32_t smoothing   = run.topology == 'f' ? shape.mesh.smoothing_group_ids[primitiveIndex] : 0;
                const uint32_t firstVertex = static_cast<uint32_t>(scene.vertices.size());
                for (size_t index = 0; !state.errors.Failed() && index < count; ++index)
                {
                    const tinyobj::index_t& corner = corners[offset + index];
                    if (smoothing != 0)
                    {
                        smoothNormals[SmoothKey{run.objectIndex, smoothing, corner.vertex_index}] += normal;
                    }
                    if (corner.normal_index < 0 && run.topology == 'f')
                    {
                        vertices[index].normal = Vec4(normal, 0);
                        if (smoothing != 0)
                        {
                            const SmoothKey key{run.objectIndex, smoothing, corner.vertex_index};
                            smoothCorners.push_back({firstVertex + static_cast<uint32_t>(index), key});
                        }
                        else
                        {
                            primitive.flatNormals = true;
                        }
                    }
                    primitive.bounds.SetMin(Vec3(vertices[index].pos));
                    primitive.bounds.SetMax(Vec3(vertices[index].pos));
                    primitive.sourceVertices.push_back(static_cast<uint32_t>(corner.vertex_index));
                    scene.vertices.push_back(vertices[index]);
                }
                for (uint32_t index : indices)
                {
                    scene.indices.push_back(firstVertex + index);
                }
                offset += count;
                ++primitiveIndex;
            }
            if (!state.errors.Failed())
            {
                primitive.vertexCount = static_cast<uint32_t>(scene.vertices.size()) - primitive.firstVertex;
                primitive.indexCount  = static_cast<uint32_t>(scene.indices.size()) - primitive.firstIndex;
                mesh.primitives.push_back(std::move(primitive));
                NodeId                                           parent;
                const std::map<uint32_t, NodeId>::const_iterator existing = objectNodes.find(run.objectIndex);
                if (existing != objectNodes.end())
                {
                    parent = existing->second;
                }
                else
                {
                    parent.value = static_cast<uint32_t>(scene.nodes.size());
                    NodeAsset object;
                    object.name = run.object;
                    scene.nodes.push_back(std::move(object));
                    scene.roots.push_back(parent);
                    objectNodes.emplace(run.objectIndex, parent);
                }
                NodeAsset node;
                node.name       = mesh.name;
                node.parent     = parent;
                node.mesh.value = static_cast<uint32_t>(scene.meshes.size());
                scene.nodes[parent.value].children.push_back(NodeId{static_cast<uint32_t>(scene.nodes.size())});
                scene.nodes.push_back(std::move(node));
                scene.meshes.push_back(std::move(mesh));
            }
        }
    }
    for (const SmoothCorner& corner : smoothCorners)
    {
        Vec3 normal = smoothNormals[corner.key];
        if (!NormalizeSurfaceNormal(normal))
        {
            state.errors.Fail("OBJ smoothing group has cancelling surface normals");
        }
        else
        {
            scene.vertices[corner.vertex].normal = Vec4(normal, 0);
        }
    }
}
} // namespace

SceneImportResult ObjSceneImporter::Import(const SceneImportRequest& request, const BackendContext& context) const
{
    ZENASSET_MEASURE_BEGIN(sourceParsing, SourceAcquireParse);
    SceneImportResult      result;
    std::unique_ptr<Scene> scene(new Scene);
    scene->sourcePath = request.path;
    scene->name       = PathToUtf8(std::filesystem::u8path(request.path).filename());
    ObjImportState state(request, context, result, *scene);
    if (request.settings.sceneIndex != UINT32_MAX && request.settings.sceneIndex != 0)
    {
        state.errors.Fail("OBJ contains one scene; select scene zero", StatusCode::InvalidRequest);
    }
    std::string bytes;
    if (!state.errors.Failed() && state.ReadFile(std::filesystem::u8path(request.path), bytes))
    {
        // The build/cache layer records the root source independently. Canonical
        // dependency records contain only external libraries and textures.
        scene->dependencies.clear();
        ObjDocument                      document = Preflight(state, bytes);
        tinyobj::attrib_t                attributes;
        std::vector<tinyobj::shape_t>    shapes;
        std::vector<tinyobj::material_t> materials;
        ObjMaterialReader                materialReader(state);
        std::istringstream               stream(document.normalized);
        std::string                      warning;
        std::string                      error;
        if (!state.errors.Failed()
            && !tinyobj::LoadObj(&attributes, &shapes, &materials, &warning, &error, &stream, &materialReader, false))
        {
            state.errors.Fail("OBJ parser: " + error);
        }
        if (!warning.empty())
        {
            state.Warn("parser", warning);
        }
        ZENASSET_MEASURE_END(sourceParsing);
        ZENASSET_MEASURE_SCOPE(SourceConvert);
        if (!state.errors.Failed())
        {
            state.errors.SetStage(SceneImportStage::DecodingTextures);
            ReadObjMaterials(state, materials);
        }
        if (!state.errors.Failed())
        {
            if (request.control)
            {
                // MTL parsing and texture fallback resolution discover inputs
                // while loading. The denominator becomes fixed only now.
                request.control->FinishFileDiscovery();
            }
            state.errors.SetStage(SceneImportStage::ProcessingMeshes);
            ReadGeometry(state, document, attributes, shapes, materials);
        }
    }
    if (state.errors.Failed())
    {
        const StatusCode status =
            request.control && request.control->IsCancelled() ? StatusCode::Cancelled : state.errors.Status();
        SetSceneImportFailure(result, status, request.path, state.errors.Message());
    }
    else if (ProcessImportedAsset(*scene, result, request.path))
    {
        result.asset  = std::move(scene);
        result.status = StatusCode::Success;
    }
    return result;
}
} // namespace zenasset
