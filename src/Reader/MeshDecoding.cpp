#include "Format/MeshEncoding.h"
#include "Diagnostics/StageMeasurements.h"
#include "Format/PackageLayout.h"
#include "Codecs/MeshoptDecoder.h"
#include <algorithm>

namespace zenasset::detail
{
ResourceData DecodeMeshResource(const ResourceRecord&      record,
                                std::span<const std::byte> encoded,
                                const SceneReadLimits&     limits,
                                uint64_t                   availableBytes,
                                const Cancellation*        cancellation)
{
    ZENASSET_MEASURE_SCOPE(MeshDecode);
    ResourceData   result;
    const uint8_t* input          = reinterpret_cast<const uint8_t*>(encoded.data());
    const bool     vertex         = record.type == ResourceType::VertexStream;
    const uint32_t stride         = vertex ? MeshVertexStride : MeshIndexStride;
    const uint32_t maxBlock       = vertex ? MeshVertexBlockElements : MeshIndexBlockElements;
    uint64_t       count          = 0;
    uint64_t       canonicalBytes = 0;
    if (cancellation != nullptr && cancellation->IsCancelled())
    {
        result.status = {StatusCode::Cancelled, "Mesh decoding cancelled"};
    }
    else if (!SupportedEncoding(record.type, record.encoding) || record.encoding == 0 || encoded.size() < MeshEnvelopeBytes
             || Read32(input) != 1 || Read32(input + 4) != stride)
    {
        result.status = {StatusCode::InvalidData, "Mesh encoding envelope is invalid"};
    }
    else
    {
        count          = Read64(input + 8);
        canonicalBytes = Read64(input + 16);
        if (count > (UINT64_MAX - 8) / stride || canonicalBytes != 8 + count * stride)
        {
            result.status = {StatusCode::InvalidData, "Mesh canonical byte count is inconsistent"};
        }
        else if (count > limits.maxArrayElements || canonicalBytes > SIZE_MAX || canonicalBytes > limits.package.maxChunkBytes
                 || canonicalBytes - 8 > limits.maxRuntimeGeometryBytes)
        {
            result.status = {StatusCode::ResourceLimit, "Expanded mesh stream exceeds its configured limits"};
        }
    }
    const uint64_t scratchElements = vertex ? 0 : std::min<uint64_t>(count, maxBlock);
    const uint64_t scratchBytes    = scratchElements * sizeof(uint32_t);
    if (result.status.Succeeded()
        && (scratchBytes > limits.package.maxCodecBytes || scratchBytes > availableBytes
            || canonicalBytes > availableBytes - scratchBytes))
    {
        result.status = {StatusCode::ResourceLimit, "Expanded mesh buffer and scratch exceed the allocation budget"};
    }
    if (result.status.Succeeded())
    {
        result.bytes.resize(static_cast<size_t>(canonicalBytes));
        std::vector<uint32_t> indices(static_cast<size_t>(scratchElements));
        uint8_t*              output = reinterpret_cast<uint8_t*>(result.bytes.data());
        Write64(output, count);
        size_t   position  = MeshEnvelopeBytes;
        uint64_t completed = 0;
        while (result.status.Succeeded() && completed < count)
        {
            if (cancellation != nullptr && cancellation->IsCancelled())
            {
                result.status = {StatusCode::Cancelled, "Mesh decoding cancelled"};
            }
            else if (encoded.size() - position < MeshBlockHeaderBytes)
            {
                result.status = {StatusCode::InvalidData, "Mesh block header is truncated"};
            }
            else
            {
                const uint32_t elements  = Read32(input + position);
                const uint32_t bytes     = Read32(input + position + 4);
                position                += MeshBlockHeaderBytes;
                if (elements == 0 || elements > maxBlock || elements > count - completed || bytes == 0
                    || bytes > encoded.size() - position)
                {
                    result.status = {StatusCode::InvalidData, "Mesh block element or byte count is invalid"};
                }
                else
                {
                    const int version = vertex ? zenasset_meshopt_decodeVertexVersion(input + position, bytes)
                                               : zenasset_meshopt_decodeIndexVersion(input + position, bytes);
                    int       decoded = -1;
                    if (version == 0 || version == 1)
                    {
                        decoded = vertex ? zenasset_meshopt_decodeVertexBuffer(output + 8 + completed * stride, elements,
                                                                               stride, input + position, bytes)
                                         : zenasset_meshopt_decodeIndexSequence(indices.data(), elements, stride,
                                                                                input + position, bytes);
                    }
                    if (decoded != 0)
                    {
                        result.status = {StatusCode::InvalidData, "Mesh bitstream version or payload is invalid"};
                    }
                    else if (!vertex)
                    {
                        for (size_t index = 0; index < elements; ++index)
                        {
                            Write32(output + 8 + (completed + index) * stride, indices[index]);
                        }
                    }
                    position  += bytes;
                    completed += elements;
                }
            }
        }
        if (result.status.Succeeded() && position != encoded.size())
        {
            result.status = {StatusCode::InvalidData, "Mesh stream has trailing bytes"};
        }
    }
    if (!result.status.Succeeded())
    {
        std::vector<std::byte>().swap(result.bytes);
    }
    ZENASSET_MEASURE_ADD(MeshDecodedResources, result.status.Succeeded() ? 1 : 0);
    return result;
}
} // namespace zenasset::detail
