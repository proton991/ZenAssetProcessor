#include "Codecs/MeshoptDecoder.h"
#include "Codecs/MeshoptEncoder.h"

#include <meshoptimizer.h>
#include <zstd.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
bool CheckVertexCodecs()
{
    bool                        success = true;
    const std::array<size_t, 5> strides{4, 8, 16, 48, 256};
    const std::array<size_t, 6> counts{0, 1, 15, 16, 257, 1024};
    for (size_t stride : strides)
    {
        for (size_t count : counts)
        {
            std::vector<unsigned char> source(count * stride);
            for (size_t index = 0; index < source.size(); ++index)
            {
                source[index] = static_cast<unsigned char>((index * 193u) ^ (index >> 3));
            }
            for (int version = 0; version <= 1; ++version)
            {
                std::vector<unsigned char> encoded(meshopt_encodeVertexBufferBound(count, stride));
                const size_t               encodedSize =
                    meshopt_encodeVertexBufferLevel(encoded.data(), encoded.size(), source.data(), count, stride, 2, version);
                std::vector<unsigned char> decoded(source.size());
                const int                  result =
                    zenasset_meshopt_decodeVertexBuffer(decoded.data(), count, stride, encoded.data(), encodedSize);
                success    = success && encodedSize != 0 && result == 0 && decoded == source
                          && zenasset_meshopt_decodeVertexVersion(encoded.data(), encodedSize) == version;
                encoded[0] = 0;
                const int generatedFailure =
                    zenasset_meshopt_decodeVertexBuffer(decoded.data(), count, stride, encoded.data(), encodedSize);
                const int upstreamFailure =
                    meshopt_decodeVertexBuffer(decoded.data(), count, stride, encoded.data(), encodedSize);
                success = success && generatedFailure < 0 && generatedFailure == upstreamFailure;
            }
        }
    }
    return success;
}

bool CheckIndexCodecs()
{
    bool                            success = true;
    const std::vector<unsigned int> source{0, 1, 2, 2, 1, 3, 65537, 12, 4, 12, 65537, 4};
    for (int version = 0; version <= 1; ++version)
    {
        meshopt_encodeIndexVersion(version);
        std::vector<unsigned char> encoded(meshopt_encodeIndexBufferBound(source.size(), 65538));
        const size_t encodedSize = meshopt_encodeIndexBuffer(encoded.data(), encoded.size(), source.data(), source.size());
        std::vector<unsigned int> decoded(source.size());
        std::vector<unsigned int> upstream(source.size());
        const int result = zenasset_meshopt_decodeIndexBuffer(decoded.data(), decoded.size(), sizeof(unsigned int),
                                                              encoded.data(), encodedSize);
        const int upstreamResult =
            meshopt_decodeIndexBuffer(upstream.data(), upstream.size(), sizeof(unsigned int), encoded.data(), encodedSize);
        success    = success && encodedSize != 0 && result == 0 && upstreamResult == 0 && decoded == upstream
                  && zenasset_meshopt_decodeIndexVersion(encoded.data(), encodedSize) == version;
        encoded[0] = 0;
        success    = success
                  && zenasset_meshopt_decodeIndexBuffer(decoded.data(), decoded.size(), sizeof(unsigned int), encoded.data(),
                                                        encodedSize)
                         < 0;
    }
    meshopt_encodeIndexVersion(0);
    std::vector<unsigned char> sequence(zenasset_meshopt_encodeIndexSequenceV1Bound(source.size(), 65538));
    const size_t               sequenceSize =
        zenasset_meshopt_encodeIndexSequenceV1(sequence.data(), sequence.size(), source.data(), source.size());
    std::vector<unsigned int> decoded(source.size());
    const int sequenceResult = zenasset_meshopt_decodeIndexSequence(decoded.data(), decoded.size(), sizeof(unsigned int),
                                                                    sequence.data(), sequenceSize);
    success                  = success && sequenceSize != 0 && sequenceResult == 0 && decoded == source
                            && zenasset_meshopt_decodeIndexVersion(sequence.data(), sequenceSize) == 1;
    meshopt_encodeIndexVersion(1);
    std::vector<unsigned char> upstreamSequence(meshopt_encodeIndexSequenceBound(source.size(), 65538));
    const size_t               upstreamSequenceSize =
        meshopt_encodeIndexSequence(upstreamSequence.data(), upstreamSequence.size(), source.data(), source.size());
    success = success && sequenceSize == upstreamSequenceSize && sequence == upstreamSequence;
    return success;
}

bool CheckZstdCodecs()
{
    const std::vector<unsigned char> source(8192, 57);
    std::vector<unsigned char>       encoded(ZSTD_compressBound(source.size()));
    const size_t encodedSize = ZSTD_compress(encoded.data(), encoded.size(), source.data(), source.size(), 3);
    bool         success     = !ZSTD_isError(encodedSize);
    if (success)
    {
        std::vector<unsigned char> decoded(source.size());
        const size_t               decodedSize = ZSTD_decompress(decoded.data(), decoded.size(), encoded.data(), encodedSize);
        success = !ZSTD_isError(decodedSize) && decodedSize == source.size() && decoded == source;
    }
    return success;
}
} // namespace

int main()
{
    const bool vertex = CheckVertexCodecs();
    const bool index  = CheckIndexCodecs();
    const bool zstd   = CheckZstdCodecs();
    std::printf("Codec boundary: vertex=%s index=%s zstd=%s\n", vertex ? "PASS" : "FAIL", index ? "PASS" : "FAIL",
                zstd ? "PASS" : "FAIL");
    return vertex && index && zstd ? 0 : 1;
}
