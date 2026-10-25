#pragma once

#include <cstddef>

// Private fixed-v1 sequence encoder; independent of upstream's mutable version.
extern "C"
{
    size_t zenasset_meshopt_encodeIndexSequenceV1(unsigned char*      buffer,
                                                  size_t              buffer_size,
                                                  const unsigned int* indices,
                                                  size_t              index_count);
    size_t zenasset_meshopt_encodeIndexSequenceV1Bound(size_t index_count, size_t vertex_count);
}
