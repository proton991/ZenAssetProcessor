#pragma once

#include <cstddef>

// Private ABI for the generated decoder-only meshoptimizer v1.1 objects.
// Prefixes isolate them from a full meshoptimizer linked by source importers.
extern "C"
{
    int zenasset_meshopt_decodeIndexVersion(const unsigned char* buffer, size_t buffer_size);
    int zenasset_meshopt_decodeIndexBuffer(void*                destination,
                                           size_t               index_count,
                                           size_t               index_size,
                                           const unsigned char* buffer,
                                           size_t               buffer_size);
    int zenasset_meshopt_decodeIndexSequence(void*                destination,
                                             size_t               index_count,
                                             size_t               index_size,
                                             const unsigned char* buffer,
                                             size_t               buffer_size);
    int zenasset_meshopt_decodeVertexVersion(const unsigned char* buffer, size_t buffer_size);
    int zenasset_meshopt_decodeVertexBuffer(void*                destination,
                                            size_t               vertex_count,
                                            size_t               vertex_size,
                                            const unsigned char* buffer,
                                            size_t               buffer_size);
}
