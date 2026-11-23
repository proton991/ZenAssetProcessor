#pragma once
#include <filesystem>
#include <string>

namespace zenasset
{
// UTF-8 text of a path with forward slashes: the form stored in assets and caches.
inline std::string PathToUtf8(const std::filesystem::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

inline bool HasPathExtension(const std::filesystem::path& path, const char* wanted)
{
    std::string extension = PathToUtf8(path.extension());
    for (char& c : extension)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c + ('a' - 'A'));
        }
    }
    return extension == wanted;
}

// Absolute, lexically normalized UTF-8 path; empty when it cannot be resolved.
inline std::string AbsoluteUtf8Path(const std::filesystem::path& path)
{
    std::error_code             error;
    const std::filesystem::path absolute = std::filesystem::absolute(path, error).lexically_normal();
    return error ? std::string() : PathToUtf8(absolute);
}
} // namespace zenasset
