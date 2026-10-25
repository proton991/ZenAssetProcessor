#pragma once
#include <array>
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <limits>

namespace zenasset
{
struct Vec4;
// Value records, not a linear algebra dependency. Adapters explicitly convert
// them to their implementation's math types. Coordinates use meters, RH Y-up.
struct Vec2
{
    float x{0};
    float y{0};

    constexpr Vec2() = default;
    constexpr explicit Vec2(float value) : x(value), y(value) {}
    constexpr Vec2(float xValue, float yValue) : x(xValue), y(yValue) {}
    constexpr bool operator==(const Vec2&) const = default;

    constexpr float& operator[](size_t index)
    {
        assert(index < 2);
        return index == 0 ? x : y;
    }

    constexpr const float& operator[](size_t index) const
    {
        assert(index < 2);
        return index == 0 ? x : y;
    }
};

struct Vec3
{
    float x{0};
    float y{0};
    float z{0};

    constexpr Vec3() = default;
    constexpr explicit Vec3(float value) : x(value), y(value), z(value) {}
    constexpr Vec3(float xValue, float yValue, float zValue) : x(xValue), y(yValue), z(zValue) {}
    constexpr explicit Vec3(const Vec4& value);
    constexpr bool operator==(const Vec3&) const = default;

    constexpr float& operator[](size_t index)
    {
        assert(index < 3);
        return index == 0 ? x : index == 1 ? y : z;
    }

    constexpr const float& operator[](size_t index) const
    {
        assert(index < 3);
        return index == 0 ? x : index == 1 ? y : z;
    }
};

struct Vec4
{
    float x{0};
    float y{0};
    float z{0};
    float w{0};

    constexpr Vec4() = default;
    constexpr explicit Vec4(float value) : x(value), y(value), z(value), w(value) {}
    constexpr Vec4(float xValue, float yValue, float zValue, float wValue) : x(xValue), y(yValue), z(zValue), w(wValue) {}
    constexpr Vec4(const Vec3& value, float wValue) : x(value.x), y(value.y), z(value.z), w(wValue) {}
    constexpr Vec4(const Vec2& value, float zValue, float wValue) : x(value.x), y(value.y), z(zValue), w(wValue) {}
    constexpr bool operator==(const Vec4&) const = default;

    constexpr float& operator[](size_t index)
    {
        assert(index < 4);
        return index == 0 ? x : index == 1 ? y : index == 2 ? z : w;
    }

    constexpr const float& operator[](size_t index) const
    {
        assert(index < 4);
        return index == 0 ? x : index == 1 ? y : index == 2 ? z : w;
    }
};

constexpr Vec3::Vec3(const Vec4& value) : x(value.x), y(value.y), z(value.z) {}

struct Quat
{
    float w{1};
    float x{0};
    float y{0};
    float z{0};

    constexpr Quat() = default;
    constexpr Quat(float wValue, float xValue, float yValue, float zValue) : w(wValue), x(xValue), y(yValue), z(zValue) {}
    constexpr bool operator==(const Quat&) const = default;
};

struct Mat4
{
    // Column vectors; matrix[column][row]. Local transforms multiply T * R * S * baseMatrix.
    std::array<Vec4, 4> columns{Vec4(1, 0, 0, 0), Vec4(0, 1, 0, 0), Vec4(0, 0, 1, 0), Vec4(0, 0, 0, 1)};

    constexpr Mat4() = default;
    constexpr explicit Mat4(float diagonal) :
        columns{Vec4(diagonal, 0, 0, 0), Vec4(0, diagonal, 0, 0), Vec4(0, 0, diagonal, 0), Vec4(0, 0, 0, diagonal)}
    {}
    constexpr Mat4(float x0,
                   float y0,
                   float z0,
                   float w0,
                   float x1,
                   float y1,
                   float z1,
                   float w1,
                   float x2,
                   float y2,
                   float z2,
                   float w2,
                   float x3,
                   float y3,
                   float z3,
                   float w3) :
        columns{Vec4(x0, y0, z0, w0), Vec4(x1, y1, z1, w1), Vec4(x2, y2, z2, w2), Vec4(x3, y3, z3, w3)}
    {}
    constexpr bool operator==(const Mat4&) const = default;

    constexpr Vec4& operator[](size_t column)
    {
        assert(column < 4);
        return columns[column];
    }

    constexpr const Vec4& operator[](size_t column) const
    {
        assert(column < 4);
        return columns[column];
    }
};

struct Bounds
{
    Vec3 min{std::numeric_limits<float>::max()};
    Vec3 max{std::numeric_limits<float>::lowest()};

    constexpr Bounds() = default;
    constexpr Bounds(const Vec3& minimum, const Vec3& maximum) : min(minimum), max(maximum) {}
    constexpr bool operator==(const Bounds&) const = default;

    constexpr Vec3 GetMin() const
    {
        return min;
    }

    constexpr Vec3 GetMax() const
    {
        return max;
    }

    // Expand extrema; these retain the importer convention of accumulating bounds.
    constexpr void SetMin(const Vec3& point)
    {
        min = Vec3(std::min(min.x, point.x), std::min(min.y, point.y), std::min(min.z, point.z));
    }

    constexpr void SetMax(const Vec3& point)
    {
        max = Vec3(std::max(max.x, point.x), std::max(max.y, point.y), std::max(max.z, point.z));
    }
};
} // namespace zenasset
