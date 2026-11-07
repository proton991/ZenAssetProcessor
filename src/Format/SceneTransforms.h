#pragma once
#include "ZenAsset/Format/Scene.h"
#include <array>

namespace zenasset::detail
{
using Matrix = std::array<std::array<double, 4>, 4>;
inline Matrix Identity()
{
    Matrix result{};
    for (size_t index = 0; index < 4; ++index)
    {
        result[index][index] = 1;
    }
    return result;
}
inline Matrix Multiply(const Matrix& a, const Matrix& b)
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
inline Matrix LocalMatrix(const NodeAsset& node)
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
inline Matrix WorldMatrix(const Scene& scene, uint32_t index)
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
} // namespace zenasset::detail
