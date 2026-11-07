#pragma once
#include "ZenAsset/Format/Scene.h"
#include "ZenAsset/Format/Status.h"
#include "ZenAsset/Format/PackageIdentity.h"

namespace zenasset
{
struct AssemblyCounts
{
    uint64_t       vertices{0}, indices{0}, triangles{0}, meshes{0}, primitives{0};
    uint64_t       materials{0}, images{0}, textures{0}, samplers{0}, nodes{0};
    uint64_t       skins{0}, morphPrimitives{0}, animations{0}, cameras{0}, lights{0}, imageBasedLights{0};
    uint64_t       lightNodes{0};
    constexpr bool operator==(const AssemblyCounts&) const = default;
};

struct AssemblyModel
{
    std::string key;
    // Portable relative path below the assembly directory, using '/' separators.
    std::string           path;
    PackageIdentity       identity;
    AssemblyCounts        counts;
    std::vector<uint64_t> texCoordSets;
    Bounds                bounds;
    bool                  hasBounds{false};
    // Receipt bound to identity; a builder must validate payload checksums before reuse.
    std::string validationFingerprint;
    bool        operator==(const AssemblyModel&) const = default;
};

struct AssemblyInstance
{
    std::string key;
    std::string model;
    Mat4        transform;
    bool        submitLights{true};
    bool        operator==(const AssemblyInstance&) const = default;
};

struct AssemblySelection
{
    // Empty instance and UINT32_MAX resource mean no selection.
    std::string instance;
    uint32_t    resource{UINT32_MAX};
    bool        operator==(const AssemblySelection&) const = default;
};

struct SceneAssembly
{
    std::string                   name;
    std::vector<AssemblyModel>    models;
    std::vector<AssemblyInstance> instances;
    AssemblySelection             camera;
    AssemblySelection             environment;
    // Conservative append capacity: counts sum each instantiated model, including repeats.
    AssemblyCounts        counts;
    std::vector<uint64_t> texCoordSets;
    Bounds                bounds;
    bool                  hasBounds{false};
    bool                  operator==(const SceneAssembly&) const = default;
};

// Summarizes default-pose bounds from the source hierarchy. Animated/deformed
// bounds are not motion envelopes. Does not change model identity, path or key.
// maxScratchBytes admits node traversal storage and the copied UV-set list before
// allocation. Caller-owned scene/model storage and allocator overhead are excluded.
Status SummarizeAssemblyModel(const Scene&        scene,
                              AssemblyModel&      model,
                              uint64_t            maxScratchBytes = 64ull << 20,
                              const Cancellation* cancellation    = nullptr);
// Validates references, finite affine transforms and selectors, then recomputes
// aggregate counts/bounds. No source/package payloads are read.
Status FinalizeAssembly(SceneAssembly& assembly);
// Includes agreement between recorded aggregate metadata and computed values.
Status ValidateAssembly(const SceneAssembly& assembly);
} // namespace zenasset
