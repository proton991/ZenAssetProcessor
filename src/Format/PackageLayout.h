#pragma once
#include "ZenAsset/Format/Version.h"
#include "ZenAsset/Reader/Package.h"
#include "Format/AssetKernels.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace zenasset::detail
{
inline constexpr uint32_t               HeaderBytes         = 64;
inline constexpr uint32_t               DirectoryEntryBytes = 64;
inline constexpr uint32_t               ByteOrder           = 0x01020304;
inline constexpr std::array<uint8_t, 8> Magic{'Z', 'E', 'N', 'A', 'S', 'S', 'E', 'T'};

inline bool SupportedResourceVersion(ResourceType type, uint32_t version)
{
    return version == 1 || (type == ResourceType::Metadata && version >= 2 && version <= 8)
        || (type == ResourceType::Texture && version >= 2 && version <= 4)
        || ((type == ResourceType::Image || type == ResourceType::Environment) && version == 2);
}

inline uint32_t Read32(const uint8_t* bytes)
{
    uint32_t result = 0;
    for (uint32_t index = 0; index < 4; ++index)
    {
        result |= static_cast<uint32_t>(bytes[index]) << (index * 8);
    }
    return result;
}

inline uint64_t Read64(const uint8_t* bytes)
{
    uint64_t result = 0;
    for (uint32_t index = 0; index < 8; ++index)
    {
        result |= static_cast<uint64_t>(bytes[index]) << (index * 8);
    }
    return result;
}

inline void Write32(uint8_t* bytes, uint32_t value)
{
    for (uint32_t index = 0; index < 4; ++index)
    {
        bytes[index] = static_cast<uint8_t>(value >> (index * 8));
    }
}

inline void Write64(uint8_t* bytes, uint64_t value)
{
    for (uint32_t index = 0; index < 8; ++index)
    {
        bytes[index] = static_cast<uint8_t>(value >> (index * 8));
    }
}

inline uint64_t HashBytes(const void* bytes, size_t size)
{
    kernels::HashState hash{};
    kernels::ResetHash(hash, 0);
    kernels::UpdateHash(hash, bytes, size);
    return kernels::FinishHash(hash);
}
} // namespace zenasset::detail
