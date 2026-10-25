#pragma once
#include "ZenAsset/Format/Status.h"
#include "ZenAsset/Format/Compression.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace zenasset
{
enum class ResourceType : uint32_t
{
    Scene = 1,
    Mesh,
    Material,
    Texture,
    Animation,
    Skin,
    Morph,
    Environment,
    Metadata,
    VertexStream,
    IndexStream,
    CustomAttributes,
    Image,
    Node,
    Sampler,
    Camera,
    Light,
    MaterialVariants,
    MaterialVariant,
    TextureCoordinates,
    SourceDocument
};

inline constexpr uint32_t RequiredResource = 1;

struct ResourceRecord
{
    uint64_t       id{0};
    ResourceType   type{ResourceType::Scene};
    uint32_t       version{1};
    uint32_t       flags{RequiredResource};
    TransportCodec codec{TransportCodec::None};
    uint64_t       offset{0};
    uint64_t       storedBytes{0};
    uint64_t       decodedBytes{0};
    uint64_t       checksum{0};
    uint32_t       alignment{16};
    uint32_t       encoding{0};
};

struct ReadLimits
{
    uint64_t maxFileBytes{16ull << 30};
    uint64_t maxDecodedBytes{16ull << 30};
    uint64_t maxChunkBytes{4ull << 30};
    uint64_t maxResources{1ull << 20};
    uint64_t maxDirectoryBytes{128ull << 20};
    // Decoder workspace and encoded input staging, excluding returned bytes.
    uint64_t maxCodecBytes{64ull << 20};
};

struct ResourceData
{
    Status                 status;
    std::vector<std::byte> bytes;
};

class Package;
struct PackageResult
{
    Status                         status;
    std::shared_ptr<const Package> package;
};

// Retains the open file, so later pathname replacement cannot substitute a new
// directory/payload. Resource directory views live as long as this object.
class Package
{
public:
    ~Package();
    Package(const Package&)            = delete;
    Package& operator=(const Package&) = delete;

    std::span<const ResourceRecord> Resources() const;
    const ResourceRecord*           Find(uint64_t id) const;
    ResourceData Read(uint64_t id, const Cancellation* cancellation = nullptr, uint64_t maxScratchBytes = UINT64_MAX) const;
    Status       ValidatePayloads(const Cancellation* cancellation = nullptr) const;

private:
    struct State;
    explicit Package(std::unique_ptr<State> state);
    std::unique_ptr<State> m_state;

    friend PackageResult OpenPackage(const std::string&, const ReadLimits&, const Cancellation*);
};

// Validates the header and directory before returning. Payload integrity is
// checked on Read or ValidatePayloads; semantic scene validation is a separate
// layer. Limits bound declared package data and each returned resource buffer.
PackageResult OpenPackage(const std::string& path, const ReadLimits& limits = {}, const Cancellation* cancellation = nullptr);
} // namespace zenasset
