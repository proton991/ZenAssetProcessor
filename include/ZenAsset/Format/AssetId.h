#pragma once
#include <cstdint>

namespace zenasset
{
// IDs index the corresponding array within one Scene. They are not persistent
// editor identities. UINT32_MAX denotes an absent optional resource.
template <class Tag> struct AssetId
{
    uint32_t value{UINT32_MAX};

    constexpr bool IsValid() const
    {
        return value != UINT32_MAX;
    }

    constexpr bool operator==(const AssetId&) const = default;
};

using NodeId     = AssetId<struct NodeTag>;
using MeshId     = AssetId<struct MeshTag>;
using MaterialId = AssetId<struct MaterialTag>;
using ImageId    = AssetId<struct ImageTag>;
using TextureId  = AssetId<struct TextureTag>;
using SamplerId  = AssetId<struct SamplerTag>;
using SkinId     = AssetId<struct SkinTag>;
using CameraId   = AssetId<struct CameraTag>;
using LightId    = AssetId<struct LightTag>;
} // namespace zenasset
