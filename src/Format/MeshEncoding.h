#pragma once
#include "ZenAsset/Reader/Scene.h"

namespace zenasset::detail
{
enum class MeshEncoding : uint32_t
{
    Raw           = 0,
    Vertex        = 1,
    IndexSequence = 2
};
inline constexpr size_t   MeshEnvelopeBytes       = 24;
inline constexpr size_t   MeshBlockHeaderBytes    = 8;
inline constexpr uint32_t MeshVertexStride        = 112;
inline constexpr uint32_t MeshIndexStride         = 4;
inline constexpr uint32_t MeshVertexBlockElements = 4096;
inline constexpr uint32_t MeshIndexBlockElements  = 32768;
inline bool               SupportedEncoding(ResourceType type, uint32_t encoding)
{
    return encoding == 0 || (type == ResourceType::VertexStream && encoding == uint32_t(MeshEncoding::Vertex))
        || (type == ResourceType::IndexStream && encoding == uint32_t(MeshEncoding::IndexSequence));
}
struct EncodedMeshResource
{
    Status                 status;
    uint32_t               encoding{0};
    std::vector<std::byte> bytes;
};
EncodedMeshResource EncodeMeshResource(ResourceType               type,
                                       std::span<const std::byte> canonical,
                                       bool                       enabled,
                                       uint64_t                   availableBytes,
                                       uint64_t                   maxEncoderBytes,
                                       const Cancellation*        cancellation);
ResourceData        DecodeMeshResource(const ResourceRecord&      record,
                                       std::span<const std::byte> encoded,
                                       const SceneReadLimits&     limits,
                                       uint64_t                   availableBytes,
                                       const Cancellation*        cancellation);
} // namespace zenasset::detail
