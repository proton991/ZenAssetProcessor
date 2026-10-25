#pragma once
#include <cstdint>
#include <string_view>

namespace zenasset
{
inline constexpr uint32_t PackageSchemaVersion = 3;
inline constexpr uint32_t ProcessorVersion     = 1;

std::string_view LibraryVersion();
// Source and pinned dependency configuration identity, stable across locations.
std::string_view LibraryBuildFingerprint();
} // namespace zenasset
