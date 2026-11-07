#include "Format/MeshEncoding.h"
#include "Diagnostics/StageMeasurements.h"
#include "Format/PackageLayout.h"
#include "Codecs/MeshoptDecoder.h"
#include <algorithm>
#include <bit>
#include <type_traits>

namespace zenasset::detail
{
namespace
{
Status ReadEnvelope(const ResourceRecord&      record,
                    std::span<const std::byte> encoded,
                    const SceneReadLimits&     limits,
                    uint64_t                   availableBytes,
                    const Cancellation*        cancellation,
                    uint64_t&                  count,
                    uint64_t&                  canonicalBytes)
{
    Status         status;
    const uint8_t* input    = reinterpret_cast<const uint8_t*>(encoded.data());
    const bool     vertex   = record.type == ResourceType::VertexStream;
    const uint32_t stride   = vertex ? MeshVertexStride : MeshIndexStride;
    const uint32_t maxBlock = vertex ? MeshVertexBlockElements : MeshIndexBlockElements;
    if (cancellation != nullptr && cancellation->IsCancelled())
    {
        status = {StatusCode::Cancelled, "Mesh decoding cancelled"};
    }
    else if (!SupportedEncoding(record.type, record.encoding) || record.encoding == 0 || encoded.size() < MeshEnvelopeBytes
             || Read32(input) != 1 || Read32(input + 4) != stride)
    {
        status = {StatusCode::InvalidData, "Mesh encoding envelope is invalid"};
    }
    else
    {
        count          = Read64(input + 8);
        canonicalBytes = Read64(input + 16);
        if (count > (UINT64_MAX - 8) / stride || canonicalBytes != 8 + count * stride)
        {
            status = {StatusCode::InvalidData, "Mesh canonical byte count is inconsistent"};
        }
        else if (count > limits.maxArrayElements || canonicalBytes > SIZE_MAX || canonicalBytes > limits.package.maxChunkBytes
                 || canonicalBytes - 8 > limits.maxRuntimeGeometryBytes)
        {
            status = {StatusCode::ResourceLimit, "Expanded mesh stream exceeds its configured limits"};
        }
    }
    const uint64_t scratchElements = vertex ? 0 : std::min<uint64_t>(count, maxBlock);
    const uint64_t scratchBytes    = scratchElements * sizeof(uint32_t);
    if (status.Succeeded()
        && (scratchBytes > limits.package.maxCodecBytes || scratchBytes > availableBytes
            || canonicalBytes > availableBytes - scratchBytes))
    {
        status = {StatusCode::ResourceLimit, "Expanded mesh buffer and scratch exceed the allocation budget"};
    }
    return status;
}

Status DecodeBlocks(const ResourceRecord&      record,
                    std::span<const std::byte> encoded,
                    uint64_t                   count,
                    uint8_t*                   output,
                    const Cancellation*        cancellation)
{
    Status         status;
    const uint8_t* input           = reinterpret_cast<const uint8_t*>(encoded.data());
    const bool     vertex          = record.type == ResourceType::VertexStream;
    const uint32_t stride          = vertex ? MeshVertexStride : MeshIndexStride;
    const uint32_t maxBlock        = vertex ? MeshVertexBlockElements : MeshIndexBlockElements;
    const uint64_t scratchElements = vertex ? 0 : std::min<uint64_t>(count, maxBlock);
    {
        std::vector<uint32_t> indices(static_cast<size_t>(scratchElements));
        size_t                position  = MeshEnvelopeBytes;
        uint64_t              completed = 0;
        while (status.Succeeded() && completed < count)
        {
            if (cancellation != nullptr && cancellation->IsCancelled())
            {
                status = {StatusCode::Cancelled, "Mesh decoding cancelled"};
            }
            else if (encoded.size() - position < MeshBlockHeaderBytes)
            {
                status = {StatusCode::InvalidData, "Mesh block header is truncated"};
            }
            else
            {
                const uint32_t elements  = Read32(input + position);
                const uint32_t bytes     = Read32(input + position + 4);
                position                += MeshBlockHeaderBytes;
                if (elements == 0 || elements > maxBlock || elements > count - completed || bytes == 0
                    || bytes > encoded.size() - position)
                {
                    status = {StatusCode::InvalidData, "Mesh block element or byte count is invalid"};
                }
                else
                {
                    const int version = vertex ? zenasset_meshopt_decodeVertexVersion(input + position, bytes)
                                               : zenasset_meshopt_decodeIndexVersion(input + position, bytes);
                    int       decoded = -1;
                    if (version == 0 || version == 1)
                    {
                        decoded = vertex ? zenasset_meshopt_decodeVertexBuffer(output + completed * stride, elements, stride,
                                                                               input + position, bytes)
                                         : zenasset_meshopt_decodeIndexSequence(indices.data(), elements, stride,
                                                                                input + position, bytes);
                    }
                    if (decoded != 0)
                    {
                        status = {StatusCode::InvalidData, "Mesh bitstream version or payload is invalid"};
                    }
                    else if (!vertex)
                    {
                        for (size_t index = 0; index < elements; ++index)
                        {
                            Write32(output + (completed + index) * stride, indices[index]);
                        }
                    }
                    position  += bytes;
                    completed += elements;
                }
            }
        }
        if (status.Succeeded() && position != encoded.size())
        {
            status = {StatusCode::InvalidData, "Mesh stream has trailing bytes"};
        }
    }
    return status;
}

template <class T> Status DecodeTyped(const ResourceRecord&      record,
                                      std::span<const std::byte> encoded,
                                      const SceneReadLimits&     limits,
                                      uint64_t                   availableBytes,
                                      const Cancellation*        cancellation,
                                      std::vector<T>&            output)
{
    ZENASSET_MEASURE_SCOPE(MeshDecode);
    static_assert(std::is_trivially_copyable_v<T>);
    uint64_t           count = 0, canonicalBytes = 0;
    Status             status   = ReadEnvelope(record, encoded, limits, availableBytes, cancellation, count, canonicalBytes);
    const ResourceType expected = std::is_same_v<T, Vertex> ? ResourceType::VertexStream : ResourceType::IndexStream;
    if (status.Succeeded() && (record.type != expected || std::endian::native != std::endian::little))
    {
        status = {StatusCode::InvalidArgument, "Typed mesh decoding requires the matching little-endian stream"};
    }
    if (status.Succeeded())
    {
        output.resize(static_cast<size_t>(count));
        status = DecodeBlocks(record, encoded, count, reinterpret_cast<uint8_t*>(output.data()), cancellation);
    }
    if (!status.Succeeded())
    {
        std::vector<T>().swap(output);
    }
    ZENASSET_MEASURE_ADD(MeshDecodedResources, status.Succeeded() ? 1 : 0);
    return status;
}
} // namespace

ResourceData DecodeMeshResource(const ResourceRecord&      record,
                                std::span<const std::byte> encoded,
                                const SceneReadLimits&     limits,
                                uint64_t                   availableBytes,
                                const Cancellation*        cancellation)
{
    ZENASSET_MEASURE_SCOPE(MeshDecode);
    ResourceData result;
    uint64_t     count = 0, canonicalBytes = 0;
    result.status = ReadEnvelope(record, encoded, limits, availableBytes, cancellation, count, canonicalBytes);
    if (result.status.Succeeded())
    {
        result.bytes.resize(static_cast<size_t>(canonicalBytes));
        uint8_t* output = reinterpret_cast<uint8_t*>(result.bytes.data());
        Write64(output, count);
        result.status = DecodeBlocks(record, encoded, count, output + 8, cancellation);
    }
    if (!result.status.Succeeded())
    {
        std::vector<std::byte>().swap(result.bytes);
    }
    ZENASSET_MEASURE_ADD(MeshDecodedResources, result.status.Succeeded() ? 1 : 0);
    return result;
}

Status DecodeMeshResourceTo(const ResourceRecord&      record,
                            std::span<const std::byte> encoded,
                            const SceneReadLimits&     limits,
                            uint64_t                   availableBytes,
                            const Cancellation*        cancellation,
                            std::vector<Vertex>&       vertices)
{
    static_assert(sizeof(Vertex) == MeshVertexStride && std::is_standard_layout_v<Vertex>);
    static_assert(offsetof(Vertex, pos) == 0 && offsetof(Vertex, normal) == 16 && offsetof(Vertex, tangent) == 32
                  && offsetof(Vertex, uv0) == 48 && offsetof(Vertex, uv1) == 56 && offsetof(Vertex, joint0) == 64
                  && offsetof(Vertex, weight0) == 80 && offsetof(Vertex, color) == 96);
    return DecodeTyped(record, encoded, limits, availableBytes, cancellation, vertices);
}

Status DecodeMeshResourceTo(const ResourceRecord&      record,
                            std::span<const std::byte> encoded,
                            const SceneReadLimits&     limits,
                            uint64_t                   availableBytes,
                            const Cancellation*        cancellation,
                            std::vector<uint32_t>&     indices)
{
    return DecodeTyped(record, encoded, limits, availableBytes, cancellation, indices);
}
} // namespace zenasset::detail
