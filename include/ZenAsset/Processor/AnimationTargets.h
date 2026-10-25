#pragma once
#include "ZenAsset/Format/Scene.h"
#include <string>

namespace zenasset
{
// Resolve a supported glTF animation pointer against canonical source object
// definitions. Returns false for unknown paths, object indices, or shapes.
// Runtime application and value validation belong to the consuming engine.
bool CompileGltfAnimationTarget(const Scene& scene, const std::string& pointer, AnimationTarget& target);
} // namespace zenasset
