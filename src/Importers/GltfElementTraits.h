#pragma once
#include "ZenAsset/Format/Math.h"
#include <fastgltf/tools.hpp>
namespace fastgltf
{
template <> struct ElementTraits<zenasset::Vec2> : ElementTraitsBase<zenasset::Vec2, AccessorType::Vec2, float>
{};
template <> struct ElementTraits<zenasset::Vec3> : ElementTraitsBase<zenasset::Vec3, AccessorType::Vec3, float>
{};
template <> struct ElementTraits<zenasset::Vec4> : ElementTraitsBase<zenasset::Vec4, AccessorType::Vec4, float>
{};
template <> struct ElementTraits<zenasset::Mat4> : ElementTraitsBase<zenasset::Mat4, AccessorType::Mat4, float>
{};
} // namespace fastgltf
