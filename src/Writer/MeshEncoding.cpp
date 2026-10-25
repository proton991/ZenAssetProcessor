#include "Format/MeshEncoding.h"
#include "Diagnostics/StageMeasurements.h"
#include "Format/PackageLayout.h"
#include "Codecs/MeshoptEncoder.h"
#include <meshoptimizer.h>
#include <algorithm>

namespace zenasset::detail
{
namespace
{
size_t EncodeBlock(ResourceType           type,
                   unsigned char*         output,
                   size_t                 outputBytes,
                   const uint8_t*         input,
                   size_t                 count,
                   std::vector<uint32_t>& indices)
{
    size_t bytes = 0;
    if (type == ResourceType::VertexStream)
    {
        bytes = meshopt_encodeVertexBufferLevel(output, outputBytes, input, count, MeshVertexStride, 2, 1);
    }
    else
    {
        for (size_t index = 0; index < count; ++index)
        {
            indices[index] = Read32(input + index * MeshIndexStride);
        }
        bytes = zenasset_meshopt_encodeIndexSequenceV1(output, outputBytes, indices.data(), count);
    }
    return bytes;
}
} // namespace

EncodedMeshResource EncodeMeshResource(ResourceType               type,
                                       std::span<const std::byte> canonical,
                                       bool                       enabled,
                                       uint64_t                   availableBytes,
                                       uint64_t                   maxEncoderBytes,
                                       const Cancellation*        cancellation)
{
    ZENASSET_MEASURE_SCOPE(MeshEncode);
    EncodedMeshResource result;
    if (cancellation != nullptr && cancellation->IsCancelled())
    {
        result.status = {StatusCode::Cancelled, "Mesh compression cancelled"};
    }
    if (result.status.Succeeded() && enabled && (type == ResourceType::VertexStream || type == ResourceType::IndexStream))
    {
        ZENASSET_MEASURE_ADD(MeshEncodeAttempts, 1);
        const uint8_t* source        = reinterpret_cast<const uint8_t*>(canonical.data());
        const uint32_t stride        = type == ResourceType::VertexStream ? MeshVertexStride : MeshIndexStride;
        const uint32_t blockElements = type == ResourceType::VertexStream ? MeshVertexBlockElements : MeshIndexBlockElements;
        const uint64_t count         = canonical.size() >= 8 ? Read64(source) : UINT64_MAX;
        if (canonical.size() < 8 || count > (SIZE_MAX - 8) / stride || 8 + count * stride != canonical.size())
        {
            result.status = {StatusCode::InvalidData, "Canonical mesh stream size is inconsistent"};
        }
        const uint64_t scratchElements = type == ResourceType::IndexStream ? std::min<uint64_t>(count, blockElements) : 0;
        const uint64_t scratchBytes    = scratchElements * sizeof(uint32_t);
        if (result.status.Succeeded() && canonical.size() > MeshEnvelopeBytes + MeshBlockHeaderBytes)
        {
            if (scratchBytes > maxEncoderBytes || scratchBytes > availableBytes
                || canonical.size() > availableBytes - scratchBytes)
            {
                result.status = {StatusCode::ResourceLimit, "Mesh encoder exceeds its allocation budget"};
            }
            else
            {
                result.bytes.resize(canonical.size());
                std::vector<uint32_t> indices(static_cast<size_t>(scratchElements));
                uint8_t*              output = reinterpret_cast<uint8_t*>(result.bytes.data());
                Write32(output, 1);
                Write32(output + 4, stride);
                Write64(output + 8, count);
                Write64(output + 16, canonical.size());
                size_t   position = MeshEnvelopeBytes;
                uint64_t consumed = 0;
                bool     useful   = true;
                while (result.status.Succeeded() && useful && consumed < count)
                {
                    if (cancellation != nullptr && cancellation->IsCancelled())
                    {
                        result.status = {StatusCode::Cancelled, "Mesh compression cancelled"};
                    }
                    else if (result.bytes.size() - position <= MeshBlockHeaderBytes)
                    {
                        useful = false;
                    }
                    else
                    {
                        const uint32_t elements = static_cast<uint32_t>(std::min<uint64_t>(count - consumed, blockElements));
                        const size_t   written  = EncodeBlock(type, output + position + MeshBlockHeaderBytes,
                                                              result.bytes.size() - position - MeshBlockHeaderBytes,
                                                              source + 8 + consumed * stride, elements, indices);
                        if (written == 0)
                        {
                            useful = false;
                        }
                        else
                        {
                            Write32(output + position, elements);
                            Write32(output + position + 4, static_cast<uint32_t>(written));
                            position += MeshBlockHeaderBytes + written;
                            consumed += elements;
                        }
                    }
                }
                if (result.status.Succeeded() && useful && position < canonical.size())
                {
                    result.bytes.resize(position);
                    result.encoding = static_cast<uint32_t>(type == ResourceType::VertexStream ? MeshEncoding::Vertex
                                                                                               : MeshEncoding::IndexSequence);
                }
                else
                {
                    std::vector<std::byte>().swap(result.bytes);
                }
            }
        }
    }
    ZENASSET_MEASURE_ADD(MeshEncodedResources, result.status.Succeeded() && result.encoding != 0 ? 1 : 0);
    return result;
}
} // namespace zenasset::detail
