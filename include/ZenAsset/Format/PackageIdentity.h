#pragma once
#include <cstdint>

namespace zenasset
{
// Identity of the validated header/directory, not a cryptographic content hash.
// Payload checksums remain checked when resources are read.
struct PackageIdentity
{
    uint64_t       fileBytes{0};
    uint64_t       headerHash{0};
    constexpr bool operator==(const PackageIdentity&) const = default;
};
} // namespace zenasset
