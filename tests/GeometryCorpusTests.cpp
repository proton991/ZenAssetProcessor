#include "ZenAsset/Processor/Cook.h"
#include "ZenAsset/Processor/Import.h"
#include "SceneValueComparison.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

namespace
{
using namespace zenasset;
using Clock = std::chrono::steady_clock;
struct Comparison
{
    bool        valid{true};
    const char* failure{""};
    uint64_t    primitives{0};
    uint64_t    corners{0};
    uint64_t    remappedCorners{0};
    uint64_t    sourceCorners{0};
    uint64_t    influenceCorners{0};
    uint64_t    customComponents{0};
};
void Require(Comparison& result, bool valid, const char* message)
{
    if (result.valid && !valid)
    {
        result.valid   = false;
        result.failure = message;
    }
}
template <class T> bool SameBits(const std::vector<T>& a, const std::vector<T>& b)
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
}
bool SamePrimitiveFields(const MeshPrimitiveAsset& a, const MeshPrimitiveAsset& b)
{
    return a.name == b.name && a.firstVertex == b.firstVertex && a.vertexCount == b.vertexCount && a.firstIndex == b.firstIndex
        && a.indexCount == b.indexCount && a.material == b.material && a.materialSlot == b.materialSlot
        && a.topology == b.topology && scene_test::Same(a.bounds, b.bounds) && a.flatNormals == b.flatNormals;
}
bool SameAttributeFields(const VertexAttributeAsset& a, const VertexAttributeAsset& b)
{
    return a.mesh == b.mesh && a.primitive == b.primitive && a.semantic == b.semantic && a.morphTarget == b.morphTarget
        && a.components == b.components && a.componentType == b.componentType && a.normalized == b.normalized
        && a.domain == b.domain && a.values.size() == b.values.size();
}
uint32_t SourceIndex(const MeshPrimitiveAsset& primitive, uint32_t local)
{
    return primitive.sourceVertices.empty() ? local : primitive.sourceVertices[local];
}
void CompareCorner(const Scene& a,
                   const Scene& b,
                   uint32_t     mesh,
                   uint32_t     primitive,
                   uint32_t     corner,
                   bool         sourceDomain,
                   Comparison&  result)
{
    const MeshPrimitiveAsset& left      = a.meshes[mesh].primitives[primitive];
    const MeshPrimitiveAsset& right     = b.meshes[mesh].primitives[primitive];
    const uint32_t            oldVertex = a.indices[left.firstIndex + corner];
    const uint32_t            newVertex = b.indices[right.firstIndex + corner];
    const uint32_t            oldLocal  = oldVertex - left.firstVertex;
    const uint32_t            newLocal  = newVertex - right.firstVertex;
    ++result.corners;
    result.remappedCorners += oldVertex != newVertex ? 1 : 0;
    Require(result, std::memcmp(&a.vertices[oldVertex], &b.vertices[newVertex], sizeof(Vertex)) == 0,
            "referenced vertex bits changed");
    if (!a.vertexTexCoords.empty())
    {
        Require(result, SameBits(a.vertexTexCoords[oldVertex], b.vertexTexCoords[newVertex]), "resolved UV set bits changed");
    }
    if (!left.influences.empty())
    {
        ++result.influenceCorners;
        Require(result, !right.influences.empty() && SameBits(left.influences[oldLocal], right.influences[newLocal]),
                "resolved full joint influences changed");
    }
    if (sourceDomain || !left.sourceVertices.empty())
    {
        ++result.sourceCorners;
        Require(result, SourceIndex(left, oldLocal) == SourceIndex(right, newLocal),
                "authored morph/custom source vertex resolution changed");
    }
    for (size_t index = 0; result.valid && index < a.extraVertexAttributes.size(); ++index)
    {
        const VertexAttributeAsset& attribute = a.extraVertexAttributes[index];
        if (attribute.mesh == mesh && attribute.primitive == primitive)
        {
            const VertexAttributeAsset& cooked = b.extraVertexAttributes[index];
            const uint32_t              before =
                attribute.domain == VertexAttributeDomain::SourceVertex ? SourceIndex(left, oldLocal) : oldLocal;
            const uint32_t after =
                cooked.domain == VertexAttributeDomain::SourceVertex ? SourceIndex(right, newLocal) : newLocal;
            result.customComponents += attribute.components;
            Require(result,
                    std::memcmp(attribute.values.data() + uint64_t(before) * attribute.components,
                                cooked.values.data() + uint64_t(after) * cooked.components,
                                attribute.components * sizeof(double))
                        == 0,
                    "resolved custom or morph custom attribute components changed");
        }
    }
}

Comparison Compare(const Scene& a, const Scene& b)
{
    Comparison result;
    Require(result,
            a.vertices.size() == b.vertices.size() && a.indices.size() == b.indices.size()
                && a.vertexTexCoords.size() == b.vertexTexCoords.size() && a.meshes.size() == b.meshes.size()
                && a.extraVertexAttributes.size() == b.extraVertexAttributes.size(),
            "geometry record counts changed");
#define SAME_FIELD(field) Require(result, scene_test::Same(a.field, b.field), "unchanged scene field changed: " #field)
    SAME_FIELD(name);
    SAME_FIELD(sourcePath);
    SAME_FIELD(dependencies);
    SAME_FIELD(sourceDocument);
    SAME_FIELD(nodes);
    SAME_FIELD(roots);
    SAME_FIELD(vertexTexCoordSets);
    SAME_FIELD(materials);
    SAME_FIELD(images);
    SAME_FIELD(textures);
    SAME_FIELD(samplers);
    SAME_FIELD(skins);
    SAME_FIELD(animations);
    SAME_FIELD(morphPrimitives);
    SAME_FIELD(cameras);
    SAME_FIELD(lights);
    SAME_FIELD(materialVariants);
    SAME_FIELD(variantPrimitives);
    SAME_FIELD(imageBasedLights);
    SAME_FIELD(imageBasedLight);
#undef SAME_FIELD
    for (size_t index = 0; result.valid && index < a.extraVertexAttributes.size(); ++index)
    {
        Require(result, SameAttributeFields(a.extraVertexAttributes[index], b.extraVertexAttributes[index]),
                "custom attribute metadata changed");
        if (a.extraVertexAttributes[index].domain == VertexAttributeDomain::SourceVertex)
        {
            Require(result, SameBits(a.extraVertexAttributes[index].values, b.extraVertexAttributes[index].values),
                    "authored custom attribute payload changed");
        }
    }
    for (uint32_t mesh = 0; result.valid && mesh < a.meshes.size(); ++mesh)
    {
        Require(result,
                a.meshes[mesh].name == b.meshes[mesh].name
                    && a.meshes[mesh].primitives.size() == b.meshes[mesh].primitives.size()
                    && SameBits(a.meshes[mesh].morphWeights, b.meshes[mesh].morphWeights),
                "mesh identity or morph defaults changed");
        for (uint32_t primitive = 0; result.valid && primitive < a.meshes[mesh].primitives.size(); ++primitive)
        {
            ++result.primitives;
            const MeshPrimitiveAsset& left  = a.meshes[mesh].primitives[primitive];
            const MeshPrimitiveAsset& right = b.meshes[mesh].primitives[primitive];
            Require(result, SamePrimitiveFields(left, right), "primitive identity, topology or material changed");
            Require(result, left.influences.size() == right.influences.size(), "full influence record count changed");
            bool sourceDomain = false;
            for (const MorphPrimitiveAsset& morph : a.morphPrimitives)
            {
                sourceDomain |= morph.mesh == mesh && (morph.primitive == UINT32_MAX || morph.primitive == primitive);
            }
            for (const VertexAttributeAsset& attribute : a.extraVertexAttributes)
            {
                sourceDomain |= attribute.mesh == mesh && attribute.primitive == primitive
                             && attribute.domain == VertexAttributeDomain::SourceVertex;
            }
            for (uint32_t corner = 0; result.valid && corner < left.indexCount; ++corner)
            {
                CompareCorner(a, b, mesh, primitive, corner, sourceDomain, result);
            }
        }
    }
    return result;
}

double Milliseconds(Clock::time_point start, Clock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - start).count();
}
} // namespace

int main()
{
    std::string path;
    std::getline(std::cin, path);
    const Clock::time_point importStart = Clock::now();
    SceneImportContext      context;
    context.workerCount               = 2;
    const SceneImportResult imported  = ImportScene({path}, context);
    const Clock::time_point importEnd = Clock::now();
    SceneCookOptions        options;
    options.settings.geometry.optimizeVertexFetch = true;
    SceneCookResult cooked;
    if (imported.asset)
    {
        cooked = CookScene(imported.asset, options);
    }
    const Clock::time_point cookEnd = Clock::now();
    Comparison              comparison;
    if (!imported.asset)
    {
        comparison.valid   = false;
        comparison.failure = "source import failed";
        std::fprintf(stderr, "%s\n", imported.error.c_str());
    }
    else if (!cooked.status.Succeeded() || !cooked.scene)
    {
        comparison.valid   = false;
        comparison.failure = "scene cooking failed";
        std::fprintf(stderr, "%s\n", cooked.status.message.c_str());
    }
    else
    {
        comparison = Compare(*imported.asset, *cooked.scene);
    }
    const Clock::time_point verifyEnd = Clock::now();
    std::printf(
        "{\"ok\":%s,\"failure\":\"%s\",\"primitives\":%llu,\"corners\":%llu,\"remapped_corners\":%llu,"
        "\"source_domain_corners\":%llu,\"influence_corners\":%llu,\"custom_components\":%llu,\"warnings\":%zu,"
        "\"import_ms\":%.3f,\"cook_ms\":%.3f,\"verify_ms\":%.3f}\n",
        comparison.valid ? "true" : "false", comparison.failure, static_cast<unsigned long long>(comparison.primitives),
        static_cast<unsigned long long>(comparison.corners), static_cast<unsigned long long>(comparison.remappedCorners),
        static_cast<unsigned long long>(comparison.sourceCorners), static_cast<unsigned long long>(comparison.influenceCorners),
        static_cast<unsigned long long>(comparison.customComponents), cooked.diagnostics.size(),
        Milliseconds(importStart, importEnd), Milliseconds(importEnd, cookEnd), Milliseconds(cookEnd, verifyEnd));
    return comparison.valid ? 0 : 1;
}
