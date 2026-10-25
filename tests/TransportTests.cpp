#include "ZenAsset/Reader/Package.h"
#include "Format/PackageLayout.h"
#include <zstd.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace
{
using namespace zenasset;
uint32_t checks{0};
bool     CancelAfterEightPolls(void* user)
{
    uint32_t& polls = *static_cast<uint32_t*>(user);
    return ++polls >= 8;
}
bool Check(bool valid, const char* message)
{
    ++checks;
    if (!valid)
    {
        std::fprintf(stderr, "%s\n", message);
    }
    return valid;
}

std::vector<uint8_t> Compress(const std::vector<uint8_t>& source, bool contentSize = true)
{
    std::vector<uint8_t> bytes(ZSTD_compressBound(source.size()));
    ZSTD_CCtx*           encoder = ZSTD_createCCtx();
    size_t               result  = SIZE_MAX;
    if (encoder != nullptr)
    {
        result = ZSTD_CCtx_setParameter(encoder, ZSTD_c_contentSizeFlag, contentSize ? 1 : 0);
        if (!ZSTD_isError(result))
        {
            result = ZSTD_compress2(encoder, bytes.data(), bytes.size(), source.data(), source.size());
        }
        ZSTD_freeCCtx(encoder);
    }
    if (ZSTD_isError(result))
    {
        bytes.clear();
    }
    else
    {
        bytes.resize(result);
    }
    return bytes;
}

bool Save(const std::filesystem::path& path, const std::vector<uint8_t>& payload, uint64_t decoded)
{
    // A valid container around arbitrary codec bytes reaches the decoder's own
    // validation, rather than stopping at an outer checksum mismatch.
    std::vector<uint8_t> bytes(128 + payload.size(), 0);
    std::copy(detail::Magic.begin(), detail::Magic.end(), bytes.begin());
    detail::Write32(bytes.data() + 8, PackageSchemaVersion);
    detail::Write32(bytes.data() + 12, detail::ByteOrder);
    detail::Write32(bytes.data() + 16, 64);
    detail::Write32(bytes.data() + 20, 64);
    detail::Write64(bytes.data() + 24, 64);
    detail::Write64(bytes.data() + 32, 1);
    detail::Write64(bytes.data() + 40, bytes.size());
    uint8_t* entry = bytes.data() + 64;
    detail::Write64(entry, 1);
    detail::Write32(entry + 8, static_cast<uint32_t>(ResourceType::VertexStream));
    detail::Write32(entry + 12, 1);
    detail::Write32(entry + 16, RequiredResource);
    detail::Write32(entry + 20, static_cast<uint32_t>(TransportCodec::Zstandard));
    detail::Write64(entry + 24, 128);
    detail::Write64(entry + 32, payload.size());
    detail::Write64(entry + 40, decoded);
    detail::Write64(entry + 48, detail::HashBytes(payload.data(), payload.size()));
    detail::Write32(entry + 56, 16);
    detail::Write64(bytes.data() + 48, detail::HashBytes(entry, 64));
    detail::Write64(bytes.data() + 56, detail::HashBytes(bytes.data(), 56));
    std::copy(payload.begin(), payload.end(), bytes.begin() + 128);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.close();
    return bool(file);
}

ResourceData Read(const std::filesystem::path& path, const ReadLimits& limits = {}, uint64_t scratch = UINT64_MAX)
{
    ResourceData        result;
    const std::u8string utf8    = path.u8string();
    const PackageResult package = OpenPackage(std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size()), limits);
    result.status               = package.status;
    if (package.package)
    {
        result = package.package->Read(1, nullptr, scratch);
    }
    return result;
}

bool Reject(const std::filesystem::path& path,
            const std::vector<uint8_t>&  payload,
            uint64_t                     decoded,
            StatusCode                   expected,
            const char*                  message)
{
    bool               valid  = Check(Save(path, payload, decoded), "Cannot save codec fixture");
    const ResourceData read   = Read(path);
    valid                    &= Check(read.status.code == expected && read.bytes.empty(), message);
    return valid;
}
} // namespace

int main()
{
    const int64_t               tick = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path path = std::filesystem::current_path() / ("transport-" + std::to_string(tick) + ".zenasset");
    std::vector<uint8_t>        source(200000);
    uint32_t                    state = 0x12345678;
    for (uint8_t& byte : source)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        byte   = static_cast<uint8_t>(state);
    }
    const std::vector<uint8_t> encoded = Compress(source);
    bool valid = Check(encoded.size() > 65536 && Save(path, encoded, source.size()), "Prepare multi-buffer compressed fixture");
    const ResourceData decoded = Read(path);
    valid &=
        Check(decoded.status.Succeeded() && decoded.bytes.size() == source.size()
                  && std::equal(decoded.bytes.begin(), decoded.bytes.end(), reinterpret_cast<const std::byte*>(source.data())),
              "Streaming Zstandard decode must reproduce every byte");
    ReadLimits limits;
    limits.maxCodecBytes = 1;
    valid &= Check(Read(path, limits).status.code == StatusCode::ResourceLimit, "Retained codec workspace limit must apply");
    valid &= Check(Read(path, {}, 1).status.code == StatusCode::ResourceLimit, "Per-read scratch limit must apply");
    valid &= Reject(path, encoded, source.size() + 1, StatusCode::InvalidData, "Frame content size must match directory");
    std::vector<uint8_t> changed = encoded;
    changed.pop_back();
    valid   &= Reject(path, changed, source.size(), StatusCode::InvalidData, "Truncated frame must fail");
    changed  = encoded;
    changed.push_back(0);
    valid   &= Reject(path, changed, source.size(), StatusCode::InvalidData, "Trailing compressed bytes must fail");
    changed  = encoded;
    changed.insert(changed.end(), encoded.begin(), encoded.end());
    valid   &= Reject(path, changed, source.size(), StatusCode::InvalidData, "Concatenated frames must fail");
    valid   &= Reject(path, Compress(source, false), source.size(), StatusCode::InvalidData, "Unknown content size must fail");
    changed  = encoded;
    const size_t dictionaryOffset  = (changed[4] & 0x20) != 0 ? 5 : 6;
    changed[4]                    |= 1;
    changed.insert(changed.begin() + dictionaryOffset, 37);
    valid &= Reject(path, changed, source.size(), StatusCode::InvalidData, "Dictionary frames are unsupported");
    valid &= Reject(path, {0x50, 0x2a, 0x4d, 0x18, 0, 0, 0, 0}, 0, StatusCode::InvalidData, "Skippable frames must fail");
    valid &= Reject(path, {0x28, 0xb5, 0x2f, 0xfd, 0x80, 0xa8, 1, 0, 0, 0}, 1, StatusCode::ResourceLimit,
                    "Huge advertised windows must fail before workspace allocation");
    valid &=
        Check(Save(path, Compress({}), 0) && Read(path).status.Succeeded(), "Empty frame must decode within its declared size");
    const std::vector<uint8_t> repeated(4u << 20, 73);
    valid &= Check(Save(path, Compress(repeated), repeated.size()), "Prepare highly compressible cancellation fixture");
    const std::u8string utf8    = path.u8string();
    const PackageResult package = OpenPackage(std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size()));
    if (package.package)
    {
        uint32_t           polls = 0;
        const Cancellation cancellation(&CancelAfterEightPolls, &polls);
        const ResourceData cancelled = package.package->Read(1, &cancellation);
        valid &= Check(cancelled.status.code == StatusCode::Cancelled && cancelled.bytes.empty() && polls == 8,
                       "Large expansion must poll cancellation between bounded output slices");
    }
    else
    {
        valid &= Check(false, "Open cancellation fixture");
    }
    std::error_code error;
    std::filesystem::remove(path, error);
    valid &= Check(!error, "Remove transport test fixture");
    std::printf("%u transport checks\n", checks);
    return valid ? 0 : 1;
}
