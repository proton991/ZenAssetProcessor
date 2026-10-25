#include "ZenAsset/Reader/Package.h"
#include "Format/PackageLayout.h"
#include "Format/FileIO.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using namespace zenasset;

class Checks
{
public:
    void Expect(bool condition, const char* description)
    {
        ++m_checks;
        if (!condition)
        {
            ++m_failed;
            std::cerr << "FAIL: " << description << '\n';
        }
    }

    int Finish() const
    {
        std::cout << m_checks << " checks, " << m_failed << " failures\n";
        return m_failed == 0 ? 0 : 1;
    }

private:
    uint32_t m_checks{0};
    uint32_t m_failed{0};
};

void RefreshDirectory(std::vector<uint8_t>& bytes)
{
    detail::Write64(bytes.data() + 48, detail::HashBytes(bytes.data() + 64, 128));
    detail::Write64(bytes.data() + 56, detail::HashBytes(bytes.data(), 56));
}

std::vector<uint8_t> Fixture()
{
    std::vector<uint8_t> bytes(212, 0);
    std::copy(detail::Magic.begin(), detail::Magic.end(), bytes.begin());
    detail::Write32(bytes.data() + 8, PackageSchemaVersion);
    detail::Write32(bytes.data() + 12, detail::ByteOrder);
    detail::Write32(bytes.data() + 16, 64);
    detail::Write32(bytes.data() + 20, 64);
    detail::Write64(bytes.data() + 24, 64);
    detail::Write64(bytes.data() + 32, 2);
    detail::Write64(bytes.data() + 40, bytes.size());
    for (uint32_t index = 0; index < 2; ++index)
    {
        uint8_t*       entry  = bytes.data() + 64 + 64 * index;
        const uint64_t offset = 192 + index * 16;
        detail::Write64(entry, index + 1);
        detail::Write32(entry + 8, static_cast<uint32_t>(ResourceType::Metadata));
        detail::Write32(entry + 12, 1);
        detail::Write32(entry + 16, RequiredResource);
        detail::Write64(entry + 24, offset);
        detail::Write64(entry + 32, 4);
        detail::Write64(entry + 40, 4);
        detail::Write32(entry + 56, 16);
        for (size_t byte = 0; byte < 4; ++byte)
        {
            bytes[static_cast<size_t>(offset) + byte] = static_cast<uint8_t>(index * 4 + byte);
        }
        detail::Write64(entry + 48, detail::HashBytes(bytes.data() + offset, 4));
    }
    RefreshDirectory(bytes);
    return bytes;
}

bool Write(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    return !output.fail();
}

std::string Utf8(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

ResourceData ReadSecond(std::shared_ptr<const Package> package)
{
    return package->Read(2);
}

void VerifyValid(Checks& checks, const std::filesystem::path& file)
{
    checks.Expect(Write(file, Fixture()), "write fixture");
    const PackageResult opened = OpenPackage(Utf8(file));
    checks.Expect(opened.status.Succeeded() && opened.package != nullptr, "valid package opens");
    if (opened.package)
    {
        checks.Expect(opened.package->Resources().size() == 2, "directory resource count");
        checks.Expect(opened.package->Find(0) == nullptr && opened.package->Find(3) == nullptr, "missing resource lookup");
        const ResourceData first = opened.package->Read(1);
        checks.Expect(first.status.Succeeded() && first.bytes.size() == 4 && first.bytes[3] == std::byte{3},
                      "raw resource read");
        checks.Expect(opened.package->ValidatePayloads().Succeeded(), "streaming checksum validation");
        std::future<ResourceData> concurrent = std::async(std::launch::async, &ReadSecond, opened.package);
        const ResourceData        repeated   = opened.package->Read(1);
        const ResourceData        second     = concurrent.get();
        checks.Expect(repeated.status.Succeeded() && second.status.Succeeded() && second.bytes[0] == std::byte{4},
                      "concurrent resource reads retain independent offsets");
        const ResourceData missing = opened.package->Read(3);
        checks.Expect(missing.status.code == StatusCode::InvalidArgument && missing.bytes.empty(),
                      "missing read has no payload");
        Cancellation cancelled;
        cancelled.Cancel();
        checks.Expect(opened.package->Read(1, &cancelled).status.code == StatusCode::Cancelled, "cancelled resource read");
        checks.Expect(opened.package->ValidatePayloads(&cancelled).code == StatusCode::Cancelled, "cancelled validation");
    }
    Cancellation cancelled;
    cancelled.Cancel();
    checks.Expect(OpenPackage(Utf8(file), {}, &cancelled).status.code == StatusCode::Cancelled, "cancelled open");
    checks.Expect(OpenPackage(Utf8(file) + ".missing").status.code == StatusCode::SourceUnavailable, "missing source");
    checks.Expect(OpenPackage("").status.code == StatusCode::InvalidArgument, "empty path");
    checks.Expect(OpenPackage("\xed\xa0\x80").status.code == StatusCode::InvalidArgument, "malformed UTF-8 path");
    checks.Expect(OpenPackage(std::string("path\0suffix", 11)).status.code == StatusCode::InvalidArgument,
                  "embedded null path");
}

void VerifyReplacement(Checks& checks, const std::filesystem::path& file)
{
    checks.Expect(Write(file, Fixture()), "write original publication");
    const PackageResult  original    = OpenPackage(Utf8(file));
    std::vector<uint8_t> replacement = Fixture();
    replacement.back()               = 42;
    detail::Write64(replacement.data() + 128 + 48, detail::HashBytes(replacement.data() + 208, 4));
    RefreshDirectory(replacement);
    std::filesystem::path temporary  = file;
    temporary                       += ".replacement";
    checks.Expect(Write(temporary, replacement), "write replacement publication");
    const Status published = detail::ReplaceFileAtomically(temporary, file, true);
    if (!published.Succeeded())
    {
        std::cerr << published.message << '\n';
    }
    checks.Expect(published.Succeeded(), "replace pathname with old reader alive");
    const PackageResult current = OpenPackage(Utf8(file));
    if (original.package && current.package)
    {
        const ResourceData oldBytes = original.package->Read(2);
        const ResourceData newBytes = current.package->Read(2);
        checks.Expect(oldBytes.status.Succeeded() && oldBytes.bytes.back() == std::byte{7},
                      "old reader retains original file bytes");
        checks.Expect(newBytes.status.Succeeded() && newBytes.bytes.back() == std::byte{42},
                      "new reader sees completed publication");
    }
    else
    {
        checks.Expect(false, "both package publications open");
    }
    std::error_code error;
    std::filesystem::remove(temporary, error);
}

void VerifyCorruption(Checks& checks, const std::filesystem::path& file)
{
    const std::vector<uint8_t> original = Fixture();
    for (size_t byte : {size_t(0), size_t(12), size_t(24), size_t(40), size_t(48), size_t(56), size_t(120)})
    {
        std::vector<uint8_t> corrupt  = original;
        corrupt[byte]                ^= 0x80;
        checks.Expect(Write(file, corrupt), "write header/directory corruption");
        checks.Expect(!OpenPackage(Utf8(file)).package, "reject corrupt header/directory");
    }
    for (size_t size : {size_t(0), size_t(7), size_t(63), size_t(64), size_t(150), size_t(211)})
    {
        const std::vector<uint8_t> truncated(original.begin(), original.begin() + size);
        checks.Expect(Write(file, truncated), "write truncated fixture");
        checks.Expect(!OpenPackage(Utf8(file)).package, "reject truncated package");
    }
    std::vector<uint8_t> corrupt  = original;
    corrupt.back()               ^= 1;
    checks.Expect(Write(file, corrupt), "write corrupt payload");
    const PackageResult opened = OpenPackage(Utf8(file));
    checks.Expect(opened.status.Succeeded(), "metadata access does not read stale payload");
    if (opened.package)
    {
        checks.Expect(opened.package->Read(2).status.code == StatusCode::InvalidData, "payload checksum is checked on read");
        checks.Expect(opened.package->ValidatePayloads().code == StatusCode::InvalidData,
                      "streaming validator rejects corrupt payload");
    }
}

void VerifyDirectoryRules(Checks& checks, const std::filesystem::path& file)
{
    const std::vector<uint8_t>                       original = Fixture();
    const std::array<std::pair<size_t, uint64_t>, 6> invalid{
        {{128, 1}, {152, 192}, {152, UINT64_MAX}, {160, UINT64_MAX}, {168, UINT64_MAX}, {96, 3}}};
    for (const std::pair<size_t, uint64_t>& change : invalid)
    {
        std::vector<uint8_t> bytes = original;
        detail::Write64(bytes.data() + change.first, change.second);
        RefreshDirectory(bytes);
        checks.Expect(Write(file, bytes), "write semantically invalid directory");
        checks.Expect(!OpenPackage(Utf8(file)).package, "reject malformed directory even with valid checksum");
    }
    std::vector<uint8_t> bytes = original;
    detail::Write32(bytes.data() + 8, 99);
    RefreshDirectory(bytes);
    checks.Expect(Write(file, bytes), "write unsupported version");
    checks.Expect(OpenPackage(Utf8(file)).status.code == StatusCode::UnsupportedVersion, "unsupported schema diagnostic");
    bytes = original;
    detail::Write32(bytes.data() + 84, 99);
    RefreshDirectory(bytes);
    checks.Expect(Write(file, bytes), "write unsupported codec");
    checks.Expect(OpenPackage(Utf8(file)).status.code == StatusCode::UnsupportedCodec, "required unknown codec rejected");
    bytes = original;
    detail::Write32(bytes.data() + 72, 999);
    detail::Write32(bytes.data() + 80, 0);
    RefreshDirectory(bytes);
    checks.Expect(Write(file, bytes), "write unknown optional resource");
    const PackageResult optional = OpenPackage(Utf8(file));
    checks.Expect(optional.status.Succeeded(), "unknown optional resource can be skipped");
    if (optional.package)
    {
        checks.Expect(optional.package->Read(1).status.code == StatusCode::UnsupportedVersion,
                      "unknown optional resource cannot be consumed");
        checks.Expect(optional.package->Read(2).status.Succeeded(), "known resource remains available");
    }
}

void VerifyLimits(Checks& checks, const std::filesystem::path& file)
{
    checks.Expect(Write(file, Fixture()), "write limit fixture");
    ReadLimits limits;
    limits.maxFileBytes = 100;
    checks.Expect(OpenPackage(Utf8(file), limits).status.code == StatusCode::ResourceLimit, "file budget");
    limits                   = {};
    limits.maxDirectoryBytes = 1;
    checks.Expect(OpenPackage(Utf8(file), limits).status.code == StatusCode::ResourceLimit, "directory allocation budget");
    limits              = {};
    limits.maxResources = 1;
    checks.Expect(OpenPackage(Utf8(file), limits).status.code == StatusCode::ResourceLimit, "resource count budget");
    limits                 = {};
    limits.maxDecodedBytes = 7;
    checks.Expect(OpenPackage(Utf8(file), limits).status.code == StatusCode::ResourceLimit, "aggregate decoded budget");
    limits               = {};
    limits.maxChunkBytes = 3;
    checks.Expect(OpenPackage(Utf8(file), limits).status.code == StatusCode::ResourceLimit, "individual chunk budget");
    limits.maxChunkBytes   = 4;
    limits.maxDecodedBytes = 8;
    checks.Expect(OpenPackage(Utf8(file), limits).status.Succeeded(), "exact decoded limits accepted");
}
} // namespace

int main()
{
    Checks checks;
    checks.Expect(detail::HashBytes(nullptr, 0) == 0xef46db3751d8e999ull, "XXH64 empty known answer");
    checks.Expect(detail::HashBytes("abc", 3) == 0x44bc2cf5ad770999ull, "XXH64 abc known answer");
    std::error_code             error;
    const std::filesystem::path directory = std::filesystem::temp_directory_path(error);
    const std::filesystem::path file =
        directory
        / ("zenasset-package-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".zenasset");
    checks.Expect(!error, "temporary directory available");
    if (!error)
    {
        VerifyValid(checks, file);
        VerifyReplacement(checks, file);
        VerifyCorruption(checks, file);
        VerifyDirectoryRules(checks, file);
        VerifyLimits(checks, file);
        std::filesystem::remove(file, error);
        checks.Expect(!error, "temporary file removed");
    }
    return checks.Finish();
}
