#pragma once
#include "ZenAsset/Format/Scene.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <memory>
#include <type_traits>

namespace zenasset
{
inline constexpr float Pi        = 3.14159265358979323846f;
inline constexpr float HalfPi    = Pi * 0.5f;
inline constexpr float QuarterPi = Pi * 0.25f;
inline constexpr float TwoPi     = Pi * 2.0f;
inline float           Radians(float value)
{
    return value * (Pi / 180.0f);
}
inline float Mix(float a, float b, float weight)
{
    return a * (1.0f - weight) + b * weight;
}
inline Vec2 MakeVec2(const float* p)
{
    return Vec2(p[0], p[1]);
}
inline Vec3 MakeVec3(const float* p)
{
    return Vec3(p[0], p[1], p[2]);
}
inline Vec4 MakeVec4(const float* p)
{
    return Vec4(p[0], p[1], p[2], p[3]);
}
inline Quat MakeQuat(const float* p)
{
    return Quat(p[3], p[0], p[1], p[2]);
}
inline Mat4 MakeMat4(const float* p)
{
    Mat4 result;
    for (size_t column = 0; column < 4; ++column)
    {
        result[column] = MakeVec4(p + column * 4);
    }
    return result;
}

template <class T>
concept FloatVector = std::is_same_v<T, Vec2> || std::is_same_v<T, Vec3> || std::is_same_v<T, Vec4>;
template <FloatVector T> T operator+(const T& a, const T& b)
{
    T result;
    for (size_t index = 0; index < sizeof(T) / sizeof(float); ++index)
    {
        result[index] = a[index] + b[index];
    }
    return result;
}
template <FloatVector T> T operator-(const T& a, const T& b)
{
    T result;
    for (size_t index = 0; index < sizeof(T) / sizeof(float); ++index)
    {
        result[index] = a[index] - b[index];
    }
    return result;
}
template <FloatVector T> T operator*(const T& a, float b)
{
    T result;
    for (size_t index = 0; index < sizeof(T) / sizeof(float); ++index)
    {
        result[index] = a[index] * b;
    }
    return result;
}
template <FloatVector T> T operator/(const T& a, float b)
{
    T result;
    for (size_t index = 0; index < sizeof(T) / sizeof(float); ++index)
    {
        result[index] = a[index] / b;
    }
    return result;
}
template <FloatVector T> T& operator+=(T& a, const T& b)
{
    a = a + b;
    return a;
}
template <FloatVector T> T& operator/=(T& a, float b)
{
    a = a / b;
    return a;
}
template <FloatVector T> float Dot(const T& a, const T& b)
{
    float result = 0;
    for (size_t index = 0; index < sizeof(T) / sizeof(float); ++index)
    {
        result += a[index] * b[index];
    }
    return result;
}
inline float Dot(const Quat& a, const Quat& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}
inline Vec3 Cross(const Vec3& a, const Vec3& b)
{
    return Vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
template <FloatVector T> float Length(const T& value)
{
    return std::sqrt(Dot(value, value));
}
inline Vec3 Pow(const Vec3& a, const Vec3& b)
{
    return Vec3(std::pow(a.x, b.x), std::pow(a.y, b.y), std::pow(a.z, b.z));
}
inline bool LessThanEqual(const Vec3& a, const Vec3& b)
{
    return a.x <= b.x && a.y <= b.y && a.z <= b.z;
}
inline Quat Normalize(const Quat& value)
{
    const float length = std::sqrt(Dot(value, value));
    Quat        result;
    if (length > 0)
    {
        const float scale = 1.0f / length;
        result            = Quat(value.w * scale, value.x * scale, value.y * scale, value.z * scale);
    }
    return result;
}
inline Quat Slerp(const Quat& a, const Quat& b, float weight)
{
    Quat  target = b;
    float cosine = Dot(a, b);
    if (cosine < 0)
    {
        target = Quat(-b.w, -b.x, -b.y, -b.z);
        cosine = -cosine;
    }
    float left  = 1 - weight;
    float right = weight;
    if (cosine < 1 - std::numeric_limits<float>::epsilon())
    {
        const float angle = std::acos(cosine);
        left              = std::sin((1 - weight) * angle) / std::sin(angle);
        right             = std::sin(weight * angle) / std::sin(angle);
    }
    return Quat(a.w * left + target.w * right, a.x * left + target.x * right, a.y * left + target.y * right,
                a.z * left + target.z * right);
}
template <class T>
    requires std::is_floating_point_v<T>
bool IsFinite(T value)
{
    return std::isfinite(value);
}
template <FloatVector T> bool IsFinite(const T& value)
{
    bool valid = true;
    for (size_t index = 0; index < sizeof(T) / sizeof(float); ++index)
    {
        valid &= std::isfinite(value[index]);
    }
    return valid;
}
inline bool IsFinite(const Quat& value)
{
    return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z) && IsFinite(value.w);
}
inline bool IsFinite(const Mat4& value)
{
    bool valid = true;
    for (size_t column = 0; column < 4; ++column)
    {
        valid &= IsFinite(value[column]);
    }
    return valid;
}
} // namespace zenasset
