#include "Processing/GeometryProcessing.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <numeric>
#include <utility>

namespace zenasset::detail
{
namespace
{
class Scratch
{
public:
    explicit Scratch(uint64_t& remaining) : m_remaining(remaining) {}
    ~Scratch()
    {
        m_remaining += m_bytes;
    }
    bool Reserve(uint64_t bytes)
    {
        const bool fits = bytes <= m_remaining;
        if (fits)
        {
            m_remaining -= bytes;
            m_bytes     += bytes;
        }
        return fits;
    }

private:
    uint64_t& m_remaining;
    uint64_t  m_bytes{0};
};

struct PrimitiveRef
{
    uint32_t mesh;
    uint32_t primitive;
    uint32_t first;
    uint32_t count;
};
struct PrimitiveOrder
{
    bool operator()(const PrimitiveRef& a, const PrimitiveRef& b) const
    {
        return a.first < b.first
            || (a.first == b.first
                && (a.count < b.count
                    || (a.count == b.count && (a.mesh < b.mesh || (a.mesh == b.mesh && a.primitive < b.primitive)))));
    }
};
struct RangeGroup
{
    size_t begin;
    size_t end;
    bool   overlap{false};
};
struct IndexRange
{
    uint32_t first;
    uint32_t count;
};
struct IndexOrder
{
    bool operator()(const IndexRange& a, const IndexRange& b) const
    {
        return a.first < b.first;
    }
};

Status CancelStatus(const Cancellation* cancellation)
{
    Status status;
    if (cancellation != nullptr && cancellation->IsCancelled())
    {
        status = {StatusCode::Cancelled, "Geometry processing cancelled"};
    }
    return status;
}

Status Warn(std::vector<Diagnostic>& diagnostics, uint64_t& remaining, const PrimitiveRef& ref, const char* reason)
{
    Status status;
    char   object[80]{};
    std::snprintf(object, sizeof(object), "mesh %u, primitive %u", ref.mesh, ref.primitive);
    const uint64_t textBytes  = std::strlen(reason) + std::strlen(object) + 2;
    const uint64_t newRecords = diagnostics.size() == diagnostics.capacity() ? diagnostics.size() + 1 : 0;
    if (newRecords > (UINT64_MAX - textBytes) / sizeof(Diagnostic)
        || textBytes + std::max<uint64_t>(newRecords, 1) * sizeof(Diagnostic) > remaining)
    {
        status = {StatusCode::ResourceLimit, "Geometry diagnostics exceed the allocation budget"};
    }
    else
    {
        if (newRecords != 0)
        {
            diagnostics.reserve(static_cast<size_t>(newRecords));
        }
        Diagnostic diagnostic;
        diagnostic.severity  = DiagnosticSeverity::Warning;
        diagnostic.status    = StatusCode::UnsupportedFeature;
        diagnostic.object    = object;
        diagnostic.message   = reason;
        remaining           -= textBytes + sizeof(Diagnostic);
        diagnostics.push_back(std::move(diagnostic));
    }
    return status;
}

bool HasAuthoredDomain(const Scene& scene, const PrimitiveRef& ref)
{
    bool needed = false;
    for (const MorphPrimitiveAsset& morph : scene.morphPrimitives)
    {
        needed |= morph.mesh == ref.mesh && (morph.primitive == ref.primitive || morph.primitive == UINT32_MAX);
    }
    for (const VertexAttributeAsset& attribute : scene.extraVertexAttributes)
    {
        needed |= attribute.mesh == ref.mesh && attribute.primitive == ref.primitive
               && attribute.domain == VertexAttributeDomain::SourceVertex;
    }
    return needed;
}

template <class T> Status Permute(std::vector<T>&              values,
                                  size_t                       offset,
                                  const std::vector<uint32_t>& order,
                                  std::vector<uint8_t>&        visited,
                                  const Cancellation*          cancellation)
{
    Status   status;
    uint64_t steps = 0;
    std::fill(visited.begin(), visited.end(), 0);
    for (size_t start = 0; status.Succeeded() && start < order.size(); ++start)
    {
        if ((start & 4095u) == 0)
        {
            status = CancelStatus(cancellation);
        }
        if (!visited[start])
        {
            T      saved   = std::move(values[offset + start]);
            size_t current = start;
            while (status.Succeeded() && order[current] != start)
            {
                if ((steps++ & 4095u) == 0)
                {
                    status = CancelStatus(cancellation);
                }
                values[offset + current] = std::move(values[offset + order[current]]);
                visited[current]         = 1;
                current                  = order[current];
            }
            values[offset + current] = std::move(saved);
            visited[current]         = 1;
        }
    }
    return status;
}

Status PermuteAttributes(VertexAttributeAsset&        attribute,
                         const std::vector<uint32_t>& order,
                         std::vector<uint8_t>&        visited,
                         const Cancellation*          cancellation)
{
    Status   status;
    uint64_t steps = 0;
    for (uint32_t component = 0; status.Succeeded() && component < attribute.components; ++component)
    {
        std::fill(visited.begin(), visited.end(), 0);
        for (size_t start = 0; status.Succeeded() && start < order.size(); ++start)
        {
            if ((start & 4095u) == 0)
            {
                status = CancelStatus(cancellation);
            }
            if (!visited[start])
            {
                const double saved   = attribute.values[start * attribute.components + component];
                size_t       current = start;
                while (status.Succeeded() && order[current] != start)
                {
                    if ((steps++ & 4095u) == 0)
                    {
                        status = CancelStatus(cancellation);
                    }
                    attribute.values[current * attribute.components + component] =
                        attribute.values[order[current] * attribute.components + component];
                    visited[current] = 1;
                    current          = order[current];
                }
                attribute.values[current * attribute.components + component] = saved;
                visited[current]                                             = 1;
            }
        }
    }
    return status;
}

Status OptimizeGroup(Scene&                           scene,
                     const std::vector<PrimitiveRef>& refs,
                     const RangeGroup&                group,
                     uint64_t&                        remaining,
                     const Cancellation*              cancellation)
{
    Status              status;
    Scratch             scratch(remaining);
    const PrimitiveRef& first = refs[group.begin];
    const uint64_t      temporary =
        uint64_t(first.count) * (2 * sizeof(uint32_t) + sizeof(uint8_t)) + (group.end - group.begin) * sizeof(IndexRange);
    if (!scratch.Reserve(temporary))
    {
        status = {StatusCode::ResourceLimit, "Vertex fetch remapping exceeds the allocation budget"};
    }
    if (status.Succeeded())
    {
        std::vector<IndexRange> ranges;
        ranges.reserve(group.end - group.begin);
        for (size_t index = group.begin; index < group.end; ++index)
        {
            const PrimitiveRef&       ref       = refs[index];
            const MeshPrimitiveAsset& primitive = scene.meshes[ref.mesh].primitives[ref.primitive];
            ranges.push_back({primitive.firstIndex, primitive.indexCount});
        }
        std::sort(ranges.begin(), ranges.end(), IndexOrder{});
        size_t rangeCount = 0;
        for (const IndexRange& range : ranges)
        {
            if (rangeCount != 0
                && uint64_t(range.first) <= uint64_t(ranges[rangeCount - 1].first) + ranges[rangeCount - 1].count)
            {
                IndexRange& previous = ranges[rangeCount - 1];
                previous.count       = static_cast<uint32_t>(
                    std::max(uint64_t(previous.first) + previous.count, uint64_t(range.first) + range.count) - previous.first);
            }
            else
            {
                ranges[rangeCount++] = range;
            }
        }
        ranges.resize(rangeCount);
        std::vector<uint32_t> remap(first.count, UINT32_MAX);
        std::vector<uint32_t> order(first.count);
        std::vector<uint8_t>  visited(first.count);
        uint32_t              next = 0;
        for (const IndexRange& range : ranges)
        {
            for (uint32_t index = 0; status.Succeeded() && index < range.count; ++index)
            {
                if ((index & 4095u) == 0)
                {
                    status = CancelStatus(cancellation);
                }
                const uint32_t original = scene.indices[range.first + index] - first.first;
                if (remap[original] == UINT32_MAX)
                {
                    remap[original] = next;
                    order[next++]   = original;
                }
            }
        }
        for (uint32_t original = 0; original < first.count; ++original)
        {
            if (remap[original] == UINT32_MAX)
            {
                remap[original] = next;
                order[next++]   = original;
            }
        }
        bool changed = false;
        for (uint32_t original = 0; original < first.count; ++original)
        {
            changed |= remap[original] != original;
        }
        if (status.Succeeded() && changed)
        {
            for (size_t index = group.begin; status.Succeeded() && index < group.end; ++index)
            {
                const PrimitiveRef& ref       = refs[index];
                MeshPrimitiveAsset& primitive = scene.meshes[ref.mesh].primitives[ref.primitive];
                if (primitive.sourceVertices.empty() && HasAuthoredDomain(scene, ref))
                {
                    const uint64_t bytes = uint64_t(first.count) * sizeof(uint32_t);
                    if (bytes > remaining)
                    {
                        status = {StatusCode::ResourceLimit, "Authored vertex mapping exceeds the allocation budget"};
                    }
                    else
                    {
                        remaining -= bytes;
                        primitive.sourceVertices.resize(first.count);
                        std::iota(primitive.sourceVertices.begin(), primitive.sourceVertices.end(), uint32_t(0));
                    }
                }
                if (status.Succeeded())
                {
                    if (!primitive.sourceVertices.empty())
                    {
                        status = Permute(primitive.sourceVertices, 0, order, visited, cancellation);
                    }
                    if (status.Succeeded() && !primitive.influences.empty())
                    {
                        status = Permute(primitive.influences, 0, order, visited, cancellation);
                    }
                    for (VertexAttributeAsset& attribute : scene.extraVertexAttributes)
                    {
                        if (status.Succeeded() && attribute.mesh == ref.mesh && attribute.primitive == ref.primitive
                            && attribute.domain == VertexAttributeDomain::PrimitiveVertex)
                        {
                            status = PermuteAttributes(attribute, order, visited, cancellation);
                        }
                    }
                }
            }
            if (status.Succeeded())
            {
                status = Permute(scene.vertices, first.first, order, visited, cancellation);
                if (status.Succeeded() && !scene.vertexTexCoords.empty())
                {
                    status = Permute(scene.vertexTexCoords, first.first, order, visited, cancellation);
                }
                for (const IndexRange& range : ranges)
                {
                    for (uint32_t index = 0; status.Succeeded() && index < range.count; ++index)
                    {
                        if ((index & 4095u) == 0)
                        {
                            status = CancelStatus(cancellation);
                        }
                        uint32_t& vertex = scene.indices[range.first + index];
                        vertex           = first.first + remap[vertex - first.first];
                    }
                }
            }
        }
    }
    return status;
}

using Matrix = std::array<std::array<double, 4>, 4>;
Matrix Identity()
{
    Matrix result{};
    for (size_t index = 0; index < 4; ++index)
    {
        result[index][index] = 1;
    }
    return result;
}
Matrix Multiply(const Matrix& a, const Matrix& b)
{
    Matrix result{};
    for (size_t column = 0; column < 4; ++column)
    {
        for (size_t row = 0; row < 4; ++row)
        {
            for (size_t inner = 0; inner < 4; ++inner)
            {
                result[column][row] += a[inner][row] * b[column][inner];
            }
        }
    }
    return result;
}
Matrix LocalMatrix(const NodeAsset& node)
{
    const double x = node.rotation.x, y = node.rotation.y, z = node.rotation.z, w = node.rotation.w;
    Matrix       trs = Identity();
    trs[0][0]        = (1 - 2 * (y * y + z * z)) * node.scale.x;
    trs[0][1]        = (2 * (x * y + w * z)) * node.scale.x;
    trs[0][2]        = (2 * (x * z - w * y)) * node.scale.x;
    trs[1][0]        = (2 * (x * y - w * z)) * node.scale.y;
    trs[1][1]        = (1 - 2 * (x * x + z * z)) * node.scale.y;
    trs[1][2]        = (2 * (y * z + w * x)) * node.scale.y;
    trs[2][0]        = (2 * (x * z + w * y)) * node.scale.z;
    trs[2][1]        = (2 * (y * z - w * x)) * node.scale.z;
    trs[2][2]        = (1 - 2 * (x * x + y * y)) * node.scale.z;
    for (size_t component = 0; component < 3; ++component)
    {
        trs[3][component] = node.translation[component];
    }
    Matrix base{};
    for (size_t column = 0; column < 4; ++column)
    {
        for (size_t row = 0; row < 4; ++row)
        {
            base[column][row] = node.baseMatrix[column][row];
        }
    }
    return Multiply(trs, base);
}
Matrix WorldMatrix(const Scene& scene, uint32_t index)
{
    Matrix result = Identity();
    while (index != UINT32_MAX)
    {
        const NodeAsset& node = scene.nodes[index];
        result                = Multiply(LocalMatrix(node), result);
        index                 = node.parent.value;
    }
    return result;
}
bool NodeAnimated(const Scene& scene, uint32_t node)
{
    bool animated = false;
    for (const AnimationAsset& animation : scene.animations)
    {
        for (const AnimationChannel& channel : animation.channels)
        {
            animated |= channel.path != AnimationPath::Property && channel.node == node;
            animated |= channel.path == AnimationPath::Property && channel.target.object == AnimatedObject::Node
                     && channel.target.index == node
                     && (channel.target.property == AnimatedProperty::Translation
                         || channel.target.property == AnimatedProperty::Rotation
                         || channel.target.property == AnimatedProperty::Scale
                         || channel.target.property == AnimatedProperty::Weights);
        }
    }
    return animated;
}
bool UnsafeQuantization(const Scene& scene, const std::vector<PrimitiveRef>& refs, const RangeGroup& group)
{
    bool unsafe = false;
    for (size_t index = group.begin; index < group.end; ++index)
    {
        const PrimitiveRef&       ref        = refs[index];
        const MeshPrimitiveAsset& primitive  = scene.meshes[ref.mesh].primitives[ref.primitive];
        unsafe                              |= !primitive.influences.empty();
        for (const MorphPrimitiveAsset& morph : scene.morphPrimitives)
        {
            unsafe |= morph.mesh == ref.mesh;
        }
        for (size_t nodeIndex = 0; nodeIndex < scene.nodes.size(); ++nodeIndex)
        {
            const NodeAsset& node = scene.nodes[nodeIndex];
            if (node.mesh.value == ref.mesh)
            {
                unsafe            |= node.skin.IsValid();
                uint32_t ancestor  = static_cast<uint32_t>(nodeIndex);
                while (ancestor != UINT32_MAX)
                {
                    unsafe   |= NodeAnimated(scene, ancestor);
                    ancestor  = scene.nodes[ancestor].parent.value;
                }
            }
        }
    }
    return unsafe;
}

bool ErrorWithinBound(const Scene&             scene,
                      const PrimitiveRef&      ref,
                      const std::vector<Vec3>& quantized,
                      const Matrix&            world,
                      const GeometrySettings&  settings,
                      const Cancellation*      cancellation)
{
    double minimum[3]{INFINITY, INFINITY, INFINITY};
    double maximum[3]{-INFINITY, -INFINITY, -INFINITY};
    double maximumErrorSquared = 0;
    bool   valid               = world[0][3] == 0 && world[1][3] == 0 && world[2][3] == 0 && world[3][3] == 1;
    for (uint32_t index = 0; valid && index < ref.count; ++index)
    {
        if ((index & 4095u) == 0 && cancellation != nullptr && cancellation->IsCancelled())
        {
            valid = false;
        }
        const Vec4& original     = scene.vertices[ref.first + index].pos;
        double      errorSquared = 0;
        for (size_t row = 0; row < 3; ++row)
        {
            double position = world[3][row] * original.w;
            double delta    = 0;
            for (size_t component = 0; component < 3; ++component)
            {
                position += world[component][row] * original[component];
                delta    += world[component][row] * (double(quantized[index][component]) - original[component]);
            }
            valid        &= std::isfinite(position) && std::isfinite(delta);
            minimum[row]  = std::min(minimum[row], position);
            maximum[row]  = std::max(maximum[row], position);
            errorSquared += delta * delta;
        }
        maximumErrorSquared = std::max(maximumErrorSquared, errorSquared);
    }
    double diagonalSquared = 0;
    for (size_t component = 0; component < 3; ++component)
    {
        const double extent  = maximum[component] - minimum[component];
        diagonalSquared     += extent * extent;
    }
    const double bound  = std::max(double(settings.positionMaxAbsoluteError),
                                   double(settings.positionMaxRelativeError) * std::sqrt(diagonalSquared));
    valid              &= std::isfinite(bound) && std::isfinite(maximumErrorSquared) && maximumErrorSquared <= bound * bound;
    return valid;
}

Status QuantizeGroup(Scene&                           scene,
                     const std::vector<PrimitiveRef>& refs,
                     const RangeGroup&                group,
                     const GeometrySettings&          settings,
                     uint64_t&                        remaining,
                     std::vector<Diagnostic>&         diagnostics,
                     const Cancellation*              cancellation)
{
    Status              status;
    const PrimitiveRef& ref = refs[group.begin];
    if (UnsafeQuantization(scene, refs, group))
    {
        status = Warn(diagnostics, remaining, ref,
                      "Position quantization skipped for a shared range with skin, morph, or animated transforms.");
    }
    else
    {
        Scratch scratch(remaining);
        if (!scratch.Reserve(uint64_t(ref.count) * sizeof(Vec3)))
        {
            status = {StatusCode::ResourceLimit, "Position quantization scratch exceeds the allocation budget"};
        }
        else
        {
            std::vector<Vec3> quantized(ref.count);
            double            minimum[3]{INFINITY, INFINITY, INFINITY};
            double            maximum[3]{-INFINITY, -INFINITY, -INFINITY};
            for (uint32_t index = 0; index < ref.count; ++index)
            {
                for (size_t component = 0; component < 3; ++component)
                {
                    const double value = scene.vertices[ref.first + index].pos[component];
                    minimum[component] = std::min(minimum[component], value);
                    maximum[component] = std::max(maximum[component], value);
                }
            }
            const double levels = double((uint32_t(1) << settings.positionQuantizationBits) - 1);
            for (uint32_t index = 0; status.Succeeded() && index < ref.count; ++index)
            {
                if ((index & 4095u) == 0)
                {
                    status = CancelStatus(cancellation);
                }
                for (size_t component = 0; component < 3; ++component)
                {
                    const double value  = scene.vertices[ref.first + index].pos[component];
                    const double extent = maximum[component] - minimum[component];
                    const double quantizedValue =
                        extent == 0
                            ? value
                            : minimum[component] + std::round((value - minimum[component]) / extent * levels) / levels * extent;
                    quantized[index][component] = static_cast<float>(quantizedValue);
                }
            }
            bool accepted = true;
            for (size_t owner = group.begin; status.Succeeded() && accepted && owner < group.end; ++owner)
            {
                bool instanced = false;
                for (size_t index = 0; accepted && index < scene.nodes.size(); ++index)
                {
                    if (scene.nodes[index].mesh.value == refs[owner].mesh)
                    {
                        instanced = true;
                        accepted  = ErrorWithinBound(scene, ref, quantized, WorldMatrix(scene, static_cast<uint32_t>(index)),
                                                     settings, cancellation);
                    }
                }
                if (!instanced)
                {
                    accepted = ErrorWithinBound(scene, ref, quantized, Identity(), settings, cancellation);
                }
            }
            if (status.Succeeded())
            {
                status = CancelStatus(cancellation);
            }
            if (status.Succeeded() && !accepted)
            {
                status =
                    Warn(diagnostics, remaining, ref,
                         "Position quantization retained original positions because the world-space error bound was exceeded.");
            }
            else if (status.Succeeded())
            {
                Bounds bounds;
                for (uint32_t index = 0; index < ref.count; ++index)
                {
                    Vec4& position = scene.vertices[ref.first + index].pos;
                    position.x     = quantized[index].x;
                    position.y     = quantized[index].y;
                    position.z     = quantized[index].z;
                    bounds.SetMin(quantized[index]);
                    bounds.SetMax(quantized[index]);
                }
                for (size_t owner = group.begin; owner < group.end; ++owner)
                {
                    scene.meshes[refs[owner].mesh].primitives[refs[owner].primitive].bounds = bounds;
                }
            }
        }
    }
    return status;
}
} // namespace

Status ProcessGeometry(Scene&                   scene,
                       const GeometrySettings&  settings,
                       uint64_t&                remainingBytes,
                       std::vector<Diagnostic>& diagnostics,
                       const Cancellation*      cancellation)
{
    Status status = CancelStatus(cancellation);
    if (status.Succeeded())
    {
        status = ValidateProcessingSettings(ProcessingSettings{settings});
    }
    if (status.Succeeded() && (settings.optimizeVertexFetch || settings.positionQuantizationBits != 0))
    {
        uint64_t count = 0;
        for (const MeshAsset& mesh : scene.meshes)
        {
            count += mesh.primitives.size();
        }
        Scratch scratch(remainingBytes);
        if (count > SIZE_MAX / sizeof(PrimitiveRef) || count > SIZE_MAX / sizeof(RangeGroup)
            || count > UINT64_MAX / (sizeof(PrimitiveRef) + sizeof(RangeGroup))
            || !scratch.Reserve(count * (sizeof(PrimitiveRef) + sizeof(RangeGroup))))
        {
            status = {StatusCode::ResourceLimit, "Geometry range analysis exceeds the allocation budget"};
        }
        else
        {
            std::vector<PrimitiveRef> refs;
            std::vector<RangeGroup>   groups;
            refs.reserve(static_cast<size_t>(count));
            groups.reserve(static_cast<size_t>(count));
            for (uint32_t mesh = 0; mesh < scene.meshes.size(); ++mesh)
            {
                for (uint32_t primitive = 0; primitive < scene.meshes[mesh].primitives.size(); ++primitive)
                {
                    const MeshPrimitiveAsset& value = scene.meshes[mesh].primitives[primitive];
                    if (value.vertexCount != 0)
                    {
                        refs.push_back({mesh, primitive, value.firstVertex, value.vertexCount});
                    }
                }
            }
            std::sort(refs.begin(), refs.end(), PrimitiveOrder{});
            for (size_t index = 0; index < refs.size(); ++index)
            {
                if (index == 0 || refs[index].first != refs[index - 1].first || refs[index].count != refs[index - 1].count)
                {
                    groups.push_back({index, index + 1});
                }
                else
                {
                    groups.back().end = index + 1;
                }
            }
            uint64_t largestEnd   = 0;
            size_t   largestGroup = 0;
            for (size_t index = 0; index < groups.size(); ++index)
            {
                const PrimitiveRef& ref = refs[groups[index].begin];
                if (ref.first < largestEnd)
                {
                    groups[index].overlap        = true;
                    groups[largestGroup].overlap = true;
                }
                if (uint64_t(ref.first) + ref.count > largestEnd)
                {
                    largestEnd   = uint64_t(ref.first) + ref.count;
                    largestGroup = index;
                }
            }
            for (const RangeGroup& group : groups)
            {
                if (status.Succeeded())
                {
                    status = CancelStatus(cancellation);
                }
                if (status.Succeeded() && group.overlap)
                {
                    status = Warn(
                        diagnostics, remainingBytes, refs[group.begin],
                        "Geometry processing skipped a partially overlapping vertex range to preserve shared primitive data.");
                }
                else if (status.Succeeded())
                {
                    if (settings.optimizeVertexFetch)
                    {
                        status = OptimizeGroup(scene, refs, group, remainingBytes, cancellation);
                    }
                    if (status.Succeeded() && settings.positionQuantizationBits != 0)
                    {
                        status = QuantizeGroup(scene, refs, group, settings, remainingBytes, diagnostics, cancellation);
                    }
                }
            }
        }
    }
    return status;
}
} // namespace zenasset::detail
