#pragma once

#include "ZenAsset/Format/Scene.h"
#include <cstdint>
#include <string>

namespace zenasset
{
// Shared graph and geometry checks for imported scenes and decoded packages.
bool ValidateSceneAsset(const Scene& scene,
                        uint64_t     maxRuntimeGeometryBytes = 16ull * 1024 * 1024 * 1024,
                        std::string* failure                 = nullptr);
bool ValidateAnimationAsset(const Scene& scene, const AnimationAsset& animation, std::string& failure);
} // namespace zenasset
