#include "ZenAsset/Format/Assembly.h"
#include "ZenAsset/Format/Utf8.h"
#include "Format/SceneTransforms.h"
#include <cmath>
#include <map>
#include <set>
#include <string_view>

namespace zenasset
{
namespace
{
constexpr uint64_t AssemblyCounts::* CountFields[]{
    &AssemblyCounts::vertices,   &AssemblyCounts::indices,   &AssemblyCounts::triangles, &AssemblyCounts::meshes,
    &AssemblyCounts::primitives, &AssemblyCounts::materials, &AssemblyCounts::images,    &AssemblyCounts::textures,
    &AssemblyCounts::samplers,   &AssemblyCounts::nodes,     &AssemblyCounts::skins,     &AssemblyCounts::morphPrimitives,
    &AssemblyCounts::animations, &AssemblyCounts::cameras,   &AssemblyCounts::lights,    &AssemblyCounts::imageBasedLights,
    &AssemblyCounts::lightNodes};

bool KeyValid(std::string_view value)
{
    bool valid = !value.empty();
    for (const char character : value)
    {
        valid &= (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
              || (character >= '0' && character <= '9') || character == '_' || character == '-';
    }
    return valid;
}

bool PathValid(std::string_view path)
{
    bool valid = !path.empty() && path.front() != '/' && path.back() != '/' && path.ends_with(".zenasset") && IsValidUtf8(path)
              && path.find_first_of("\\:<>\"|?*") == std::string_view::npos;
    size_t first = 0;
    while (valid && first < path.size())
    {
        const size_t           end       = path.find('/', first);
        const std::string_view component = path.substr(first, end == std::string_view::npos ? end : end - first);
        valid =
            !component.empty() && component != "." && component != ".." && component.back() != '.' && component.back() != ' ';
        for (const unsigned char character : component)
        {
            valid &= character >= 32;
        }
        first = end == std::string_view::npos ? path.size() : end + 1;
    }
    return valid;
}

bool BoundsValid(const Bounds& bounds)
{
    bool valid = true;
    for (size_t axis = 0; axis < 3; ++axis)
    {
        valid &= std::isfinite(bounds.min[axis]) && std::isfinite(bounds.max[axis]) && bounds.min[axis] <= bounds.max[axis];
    }
    return valid;
}

bool MatrixValid(const detail::Matrix& matrix)
{
    bool valid = matrix[0][3] == 0 && matrix[1][3] == 0 && matrix[2][3] == 0 && matrix[3][3] == 1;
    for (const std::array<double, 4>& column : matrix)
    {
        for (double value : column)
        {
            valid &= std::isfinite(value);
        }
    }
    return valid;
}

detail::Matrix ToMatrix(const Mat4& value)
{
    detail::Matrix result{};
    for (size_t column = 0; column < 4; ++column)
    {
        for (size_t row = 0; row < 4; ++row)
        {
            result[column][row] = value[column][row];
        }
    }
    return result;
}

bool ExpandBounds(Bounds& destination, const Bounds& source, const detail::Matrix& transform)
{
    bool valid = BoundsValid(source) && MatrixValid(transform);
    for (uint32_t corner = 0; valid && corner < 8; ++corner)
    {
        Vec3 lower;
        Vec3 upper;
        for (size_t row = 0; row < 3; ++row)
        {
            double value = transform[3][row];
            for (size_t axis = 0; axis < 3; ++axis)
            {
                value += transform[axis][row] * ((corner & (1u << axis)) != 0 ? source.max[axis] : source.min[axis]);
            }
            valid &= std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max();
            if (valid)
            {
                const float rounded = static_cast<float>(value);
                lower[row]          = rounded > value ? std::nextafter(rounded, -INFINITY) : rounded;
                upper[row]          = rounded < value ? std::nextafter(rounded, INFINITY) : rounded;
            }
        }
        if (valid)
        {
            destination.SetMin(lower);
            destination.SetMax(upper);
        }
    }
    return valid;
}

Status Aggregate(const SceneAssembly&   assembly,
                 AssemblyCounts&        counts,
                 Bounds&                bounds,
                 bool&                  hasBounds,
                 std::vector<uint64_t>& texCoordSets)
{
    Status                                                           status;
    std::set<uint64_t>                                               coordinateSets;
    std::map<std::string_view, const AssemblyModel*, std::less<>>    models;
    std::map<std::string_view, const AssemblyInstance*, std::less<>> instances;
    if (assembly.models.empty() || assembly.instances.empty() || !IsValidUtf8(assembly.name))
    {
        status = {StatusCode::InvalidData, "Assembly requires models, instances and a valid UTF-8 name"};
    }
    for (const AssemblyModel& model : assembly.models)
    {
        std::set<uint64_t> modelSets;
        for (uint64_t set : model.texCoordSets)
        {
            coordinateSets.insert(set);
            if (!modelSets.insert(set).second)
            {
                status = {StatusCode::InvalidData, "Assembly model contains duplicate UV sets"};
            }
        }
        if (status.Succeeded()
            && (!KeyValid(model.key) || !PathValid(model.path) || model.identity.fileBytes < 64
                || (model.hasBounds && !BoundsValid(model.bounds)) || !IsValidUtf8(model.validationFingerprint)
                || !models.emplace(model.key, &model).second))
        {
            status = {StatusCode::InvalidData, "Invalid or duplicate assembly model key, path, identity or bounds"};
        }
    }
    for (const AssemblyInstance& instance : assembly.instances)
    {
        const std::map<std::string_view, const AssemblyModel*, std::less<>>::const_iterator model = models.find(instance.model);
        const detail::Matrix transform = ToMatrix(instance.transform);
        if (status.Succeeded()
            && (!KeyValid(instance.key) || model == models.end() || !MatrixValid(transform)
                || !instances.emplace(instance.key, &instance).second))
        {
            status = {StatusCode::InvalidData, "Invalid or duplicate assembly instance, model reference or affine transform"};
        }
        if (status.Succeeded())
        {
            for (uint64_t AssemblyCounts::* field : CountFields)
            {
                const uint64_t value = model->second->counts.*field;
                if (value > UINT64_MAX - counts.*field)
                {
                    status = {StatusCode::ResourceLimit, "Assembly aggregate count overflow"};
                }
                else
                {
                    counts.*field += value;
                }
            }
            if (model->second->hasBounds)
            {
                if (!ExpandBounds(bounds, model->second->bounds, transform))
                {
                    status = {StatusCode::InvalidData, "Assembly transformed bounds exceed finite coordinates"};
                }
                hasBounds = true;
            }
        }
    }
    texCoordSets.assign(coordinateSets.begin(), coordinateSets.end());
    const AssemblySelection* selections[]{&assembly.camera, &assembly.environment};
    for (size_t index = 0; status.Succeeded() && index < 2; ++index)
    {
        const AssemblySelection& selection = *selections[index];
        if (!selection.instance.empty() || selection.resource != UINT32_MAX)
        {
            const std::map<std::string_view, const AssemblyInstance*, std::less<>>::const_iterator instance =
                instances.find(selection.instance);
            if (instance == instances.end())
            {
                status = {StatusCode::InvalidData, "Assembly selection references a missing instance"};
            }
            else
            {
                const AssemblyModel& model = *models.find(instance->second->model)->second;
                const uint64_t       limit = index == 0 ? model.counts.cameras : model.counts.imageBasedLights;
                if (selection.resource >= limit)
                {
                    status = {StatusCode::InvalidData, "Assembly selection references a missing resource"};
                }
            }
        }
    }
    return status;
}
} // namespace

Status SummarizeAssemblyModel(const Scene&        scene,
                              AssemblyModel&      model,
                              uint64_t            maxScratchBytes,
                              const Cancellation* cancellation)
{
    Status             status;
    AssemblyCounts     counts;
    constexpr uint64_t nodeBytes = sizeof(detail::Matrix) + sizeof(uint32_t) + sizeof(uint8_t);
    if (cancellation != nullptr && cancellation->IsCancelled())
    {
        status = {StatusCode::Cancelled, "Assembly model summary cancelled"};
    }
    else if (scene.nodes.size() > UINT32_MAX || scene.nodes.size() > maxScratchBytes / nodeBytes
             || scene.vertexTexCoordSets.size() > (maxScratchBytes - scene.nodes.size() * nodeBytes) / sizeof(uint64_t))
    {
        status = {StatusCode::ResourceLimit, "Assembly model summary exceeds scratch budget"};
    }
    counts.vertices         = scene.vertices.size();
    counts.indices          = scene.indices.size();
    counts.meshes           = scene.meshes.size();
    counts.materials        = scene.materials.size();
    counts.images           = scene.images.size();
    counts.textures         = scene.textures.size();
    counts.samplers         = scene.samplers.size();
    counts.nodes            = scene.nodes.size();
    counts.skins            = scene.skins.size();
    counts.morphPrimitives  = scene.morphPrimitives.size();
    counts.animations       = scene.animations.size();
    counts.cameras          = scene.cameras.size();
    counts.lights           = scene.lights.size();
    counts.imageBasedLights = scene.imageBasedLights.size();
    for (const MeshAsset& mesh : scene.meshes)
    {
        if (!status.Succeeded())
        {
            break;
        }
        counts.primitives += mesh.primitives.size();
        for (const MeshPrimitiveAsset& primitive : mesh.primitives)
        {
            counts.triangles += primitive.topology == MeshTopology::Triangles ? primitive.indexCount / 3 : 0;
        }
    }
    std::vector<uint8_t>        marks;
    std::vector<detail::Matrix> world;
    std::vector<uint32_t>       chain;
    if (status.Succeeded())
    {
        marks.resize(scene.nodes.size(), 0);
        world.resize(scene.nodes.size());
        chain.reserve(scene.nodes.size());
    }
    Bounds bounds;
    bool   hasBounds = false;
    for (size_t index = 0; status.Succeeded() && index < scene.nodes.size(); ++index)
    {
        if (cancellation != nullptr && cancellation->IsCancelled())
        {
            status = {StatusCode::Cancelled, "Assembly model summary cancelled"};
        }
        uint32_t current = static_cast<uint32_t>(index);
        chain.clear();
        while (status.Succeeded() && current != UINT32_MAX)
        {
            if (cancellation != nullptr && cancellation->IsCancelled())
            {
                status = {StatusCode::Cancelled, "Assembly model summary cancelled"};
            }
            else if (current >= scene.nodes.size() || marks[current] == 1)
            {
                status = {StatusCode::InvalidData, "Assembly model has an invalid or cyclic node hierarchy"};
            }
            else if (marks[current] == 2)
            {
                break;
            }
            else
            {
                marks[current] = 1;
                chain.push_back(current);
                current = scene.nodes[current].parent.value;
            }
        }
        for (size_t pending = chain.size(); status.Succeeded() && pending > 0; --pending)
        {
            const uint32_t   nodeIndex = chain[pending - 1];
            const NodeAsset& node      = scene.nodes[nodeIndex];
            world[nodeIndex]           = detail::LocalMatrix(node);
            if (node.parent.IsValid())
            {
                world[nodeIndex] = detail::Multiply(world[node.parent.value], world[nodeIndex]);
            }
            marks[nodeIndex] = 2;
        }
        const NodeAsset& node  = scene.nodes[index];
        counts.lightNodes     += node.light.IsValid() ? 1 : 0;
        if (status.Succeeded() && node.mesh.IsValid() && node.renderMesh)
        {
            if (node.mesh.value >= scene.meshes.size())
            {
                status = {StatusCode::InvalidData, "Assembly model node has an invalid mesh"};
            }
            else
            {
                for (const MeshPrimitiveAsset& primitive : scene.meshes[node.mesh.value].primitives)
                {
                    if (primitive.vertexCount != 0)
                    {
                        if (!ExpandBounds(bounds, primitive.bounds, world[index]))
                        {
                            status = {StatusCode::InvalidData, "Assembly model has invalid transformed bounds"};
                        }
                        hasBounds = true;
                    }
                }
            }
        }
    }
    if (status.Succeeded() && cancellation != nullptr && cancellation->IsCancelled())
    {
        status = {StatusCode::Cancelled, "Assembly model summary cancelled"};
    }
    if (status.Succeeded())
    {
        model.counts       = counts;
        model.texCoordSets = scene.vertexTexCoordSets;
        model.bounds       = bounds;
        model.hasBounds    = hasBounds;
    }
    return status;
}

Status FinalizeAssembly(SceneAssembly& assembly)
{
    AssemblyCounts        counts;
    Bounds                bounds;
    bool                  hasBounds = false;
    std::vector<uint64_t> texCoordSets;
    const Status          status = Aggregate(assembly, counts, bounds, hasBounds, texCoordSets);
    if (status.Succeeded())
    {
        assembly.counts       = counts;
        assembly.texCoordSets = std::move(texCoordSets);
        assembly.bounds       = bounds;
        assembly.hasBounds    = hasBounds;
    }
    return status;
}

Status ValidateAssembly(const SceneAssembly& assembly)
{
    AssemblyCounts        counts;
    Bounds                bounds;
    bool                  hasBounds = false;
    std::vector<uint64_t> texCoordSets;
    Status                status = Aggregate(assembly, counts, bounds, hasBounds, texCoordSets);
    if (status.Succeeded()
        && (counts != assembly.counts || hasBounds != assembly.hasBounds || bounds != assembly.bounds
            || texCoordSets != assembly.texCoordSets))
    {
        status = {StatusCode::InvalidData, "Assembly aggregate metadata disagrees with its instances"};
    }
    return status;
}
} // namespace zenasset
