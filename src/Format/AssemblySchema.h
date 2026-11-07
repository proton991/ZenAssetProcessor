#pragma once
#include "ZenAsset/Format/Assembly.h"
#include "Format/SceneSchema.h"

namespace zenasset::detail
{
template <class Archive> void Fields(Archive& ar, typename Archive::template Value<PackageIdentity>& v)
{
    ar(v.fileBytes, v.headerHash);
}
template <class Archive> void Fields(Archive& ar, typename Archive::template Value<AssemblyCounts>& v)
{
    ar(v.vertices, v.indices, v.triangles, v.meshes, v.primitives, v.materials, v.images, v.textures, v.samplers, v.nodes,
       v.skins, v.morphPrimitives, v.animations, v.cameras, v.lights, v.imageBasedLights, v.lightNodes);
}
template <class Archive> void Fields(Archive& ar, typename Archive::template Value<AssemblyModel>& v)
{
    ar(v.key, v.path, v.identity, v.counts, v.texCoordSets, v.bounds, v.hasBounds, v.validationFingerprint);
}
template <class Archive> void Fields(Archive& ar, typename Archive::template Value<AssemblyInstance>& v)
{
    ar(v.key, v.model, v.transform, v.submitLights);
}
template <class Archive> void Fields(Archive& ar, typename Archive::template Value<AssemblySelection>& v)
{
    ar(v.instance, v.resource);
}
template <class Archive> void Fields(Archive& ar, typename Archive::template Value<SceneAssembly>& v)
{
    ar(v.name, v.models, v.instances, v.camera, v.environment, v.counts, v.texCoordSets, v.bounds, v.hasBounds);
}

inline bool AssemblyValidationFits(const SceneAssembly& assembly, uint64_t budget)
{
    // Conservative map/set node accounting; strings borrow decoded storage.
    bool fits = assembly.instances.size() <= budget / 128;
    if (fits)
    {
        budget -= assembly.instances.size() * 128;
    }
    for (const AssemblyModel& model : assembly.models)
    {
        fits &= budget >= 128 && model.texCoordSets.size() <= (budget - std::min<uint64_t>(128, budget)) / 128;
        if (fits)
        {
            budget -= 128 + model.texCoordSets.size() * 128;
        }
    }
    return fits;
}
} // namespace zenasset::detail
