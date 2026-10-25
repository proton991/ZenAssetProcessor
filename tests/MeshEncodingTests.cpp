#include "Format/MeshEncoding.h"
#include "Format/PackageLayout.h"
#include "ZenAsset/Processor/Import.h"
#include "ZenAsset/Processor/Write.h"
#include "SceneValueComparison.h"
#include <cstdio>
#include <chrono>
#include <filesystem>

namespace
{
using namespace zenasset;
uint32_t checks = 0;
bool     Check(bool condition, const char* message)
{
    ++checks;
    if (!condition)
    {
        std::fprintf(stderr, "%s\n", message);
    }
    return condition;
}

bool CancelAfterTwoBlocks(void* user)
{
    uint32_t& polls = *static_cast<uint32_t*>(user);
    return ++polls >= 3;
}

std::vector<std::byte> Canonical(ResourceType type)
{
    const size_t           count  = type == ResourceType::VertexStream ? 9000 : 70000;
    const uint32_t         stride = type == ResourceType::VertexStream ? detail::MeshVertexStride : detail::MeshIndexStride;
    std::vector<std::byte> bytes(8 + count * stride);
    uint8_t*               output = reinterpret_cast<uint8_t*>(bytes.data());
    detail::Write64(output, count);
    for (size_t index = 0; index < count; ++index)
    {
        for (uint32_t component = 0; component < stride / 4; ++component)
        {
            // Include signed zero, NaN payload bits, denormals and unsigned
            // maxima; semantic coding must retain bytes without float math.
            const uint32_t patterns[] = {0x80000000, 0x7fc01234, 0x00000001, 0xffffffff, 0x3f800000, 0x807fffff};
            const uint32_t value      = type == ResourceType::VertexStream
                                          ? patterns[(index + component) % 6]
                                          : (index % 37 == 0 ? UINT32_MAX : uint32_t((index * 7) % 11));
            detail::Write32(output + 8 + index * stride + component * 4, value);
        }
    }
    return bytes;
}

ResourceRecord Record(ResourceType type, const detail::EncodedMeshResource& encoded)
{
    ResourceRecord record;
    record.type         = type;
    record.encoding     = encoded.encoding;
    record.decodedBytes = encoded.bytes.size();
    return record;
}

bool ExpectInvalid(const ResourceRecord& record, const std::vector<std::byte>& bytes, const char* message)
{
    const ResourceData result = detail::DecodeMeshResource(record, bytes, {}, 32ull << 20, nullptr);
    return Check(!result.status.Succeeded() && result.bytes.empty(), message);
}

bool TestStream(ResourceType type)
{
    bool                              valid     = true;
    const std::vector<std::byte>      canonical = Canonical(type);
    const detail::EncodedMeshResource encoded =
        detail::EncodeMeshResource(type, canonical, true, 32ull << 20, 1ull << 20, nullptr);
    valid &= Check(encoded.status.Succeeded() && encoded.encoding != 0 && encoded.bytes.size() < canonical.size(),
                   "mesh coding saves bytes over canonical data");
    if (encoded.status.Succeeded() && encoded.encoding != 0)
    {
        const ResourceRecord record   = Record(type, encoded);
        const ResourceData   decoded  = detail::DecodeMeshResource(record, encoded.bytes, {}, 32ull << 20, nullptr);
        valid                        &= Check(decoded.status.Succeeded() && decoded.bytes == canonical,
                                              "block codec preserves every vertex bit and arbitrary index order");
        for (uint32_t offset : {0u, 4u, 8u, 16u, 24u, 28u})
        {
            std::vector<std::byte> corrupted = encoded.bytes;
            detail::Write32(reinterpret_cast<uint8_t*>(corrupted.data()) + offset, UINT32_MAX);
            valid &= ExpectInvalid(record, corrupted, "malformed mesh envelope or block count rejected");
        }
        std::vector<std::byte> corrupted                                    = encoded.bytes;
        corrupted[detail::MeshEnvelopeBytes + detail::MeshBlockHeaderBytes] = std::byte{0xff};
        valid     &= ExpectInvalid(record, corrupted, "unknown mesh bitstream version rejected");
        corrupted  = encoded.bytes;
        corrupted.pop_back();
        valid     &= ExpectInvalid(record, corrupted, "truncated mesh bitstream rejected");
        corrupted  = encoded.bytes;
        corrupted.push_back(std::byte{0});
        valid     &= ExpectInvalid(record, corrupted, "trailing mesh payload rejected");
        corrupted  = encoded.bytes;
        detail::Write32(reinterpret_cast<uint8_t*>(corrupted.data()) + detail::MeshEnvelopeBytes, 0);
        valid                &= ExpectInvalid(record, corrupted, "empty intermediate mesh block rejected");
        ResourceRecord wrong  = record;
        wrong.type            = ResourceType::Material;
        valid                &= ExpectInvalid(wrong, encoded.bytes, "mesh encoding cannot be applied to other resource types");
        wrong                 = record;
        wrong.encoding        = type == ResourceType::VertexStream ? 2 : 1;
        valid                &= ExpectInvalid(wrong, encoded.bytes, "vertex and index encoding identities cannot be swapped");
        SceneReadLimits limits;
        ResourceData    limited  = detail::DecodeMeshResource(record, encoded.bytes, limits, canonical.size() - 1, nullptr);
        valid                   &= Check(limited.status.code == StatusCode::ResourceLimit && limited.bytes.empty(),
                                         "expanded output respects allocation budget before allocation");
        limits.maxRuntimeGeometryBytes  = canonical.size() - 9;
        limited                         = detail::DecodeMeshResource(record, encoded.bytes, limits, 32ull << 20, nullptr);
        valid                          &= Check(limited.status.code == StatusCode::ResourceLimit && limited.bytes.empty(),
                                                "expanded geometry limit enforced before allocation");
        limits                          = {};
        limits.maxArrayElements         = 1;
        limited                         = detail::DecodeMeshResource(record, encoded.bytes, limits, 32ull << 20, nullptr);
        valid                          &= Check(limited.status.code == StatusCode::ResourceLimit && limited.bytes.empty(),
                                                "expanded array limit enforced before allocation");
        uint32_t     polls              = 0;
        Cancellation cancelled(CancelAfterTwoBlocks, &polls);
        limited  = detail::DecodeMeshResource(record, encoded.bytes, {}, 32ull << 20, &cancelled);
        valid   &= Check(limited.status.code == StatusCode::Cancelled && limited.bytes.empty(),
                         "mesh decode cancellation checked between blocks");
        if (type == ResourceType::IndexStream)
        {
            limits                        = {};
            limits.package.maxCodecBytes  = 1;
            limited                       = detail::DecodeMeshResource(record, encoded.bytes, limits, 32ull << 20, nullptr);
            valid                        &= Check(limited.status.code == StatusCode::ResourceLimit && limited.bytes.empty(),
                                                  "native index scratch respects codec memory budget");
        }
    }
    const detail::EncodedMeshResource noBudget = detail::EncodeMeshResource(type, canonical, true, 1, 1ull << 20, nullptr);
    valid &= Check(noBudget.status.code == StatusCode::ResourceLimit && noBudget.bytes.empty(),
                   "mesh encoder rejects inadequate budget");
    uint32_t                          polls = 0;
    Cancellation                      cancelled(CancelAfterTwoBlocks, &polls);
    const detail::EncodedMeshResource stopped =
        detail::EncodeMeshResource(type, canonical, true, 32ull << 20, 1ull << 20, &cancelled);
    valid                                 &= Check(stopped.status.code == StatusCode::Cancelled && stopped.bytes.empty(),
                                                   "mesh encoder cancellation checked between blocks");
    const detail::EncodedMeshResource raw  = detail::EncodeMeshResource(type, canonical, false, 0, 0, nullptr);
    valid &= Check(raw.status.Succeeded() && raw.encoding == 0 && raw.bytes.empty(), "disabled mesh coding requires no memory");
    std::vector<std::byte> noise  = canonical;
    uint32_t               random = 0x7c01f3b9;
    for (size_t index = 8; index < noise.size(); ++index)
    {
        random       ^= random << 13;
        random       ^= random >> 17;
        random       ^= random << 5;
        noise[index]  = std::byte(random & 255);
    }
    const detail::EncodedMeshResource fallback =
        detail::EncodeMeshResource(type, noise, true, 32ull << 20, 1ull << 20, nullptr);
    valid &= Check(fallback.status.Succeeded() && fallback.encoding == 0 && fallback.bytes.empty(),
                   "incompressible semantic streams use raw fallback without retaining scratch");
    return valid;
}

bool TestScene(const std::filesystem::path& path, const Scene& scene)
{
    bool valid = true;
    for (TransportCodec transport : {TransportCodec::None, TransportCodec::Zstandard})
    {
        SceneWriteOptions options;
        options.compression.meshoptimizer  = true;
        options.compression.transport      = transport;
        const Status written               = WriteScene(path.string(), scene, options);
        valid                             &= Check(written.Succeeded(), written.message.c_str());
        if (written.Succeeded())
        {
            const SceneReadResult read  = ReadScene(path.string());
            valid                      &= Check(read.status.Succeeded() && read.scene && scene_test::Same(scene, *read.scene),
                                                "mesh coding with either transport retains all canonical scene records");
            valid &= Check(read.metadata.compression.meshoptimizer && read.metadata.compression.transport == transport,
                           "metadata v3 records semantic and transport compression settings");
            if (scene.vertices.size() == 9000 && scene.indices.size() == 70000)
            {
                const PackageResult package  = OpenPackage(path.string());
                valid                       &= Check(package.status.Succeeded(), "open semantically encoded package");
                if (package.package)
                {
                    for (const ResourceRecord& record : package.package->Resources())
                    {
                        if (record.type == ResourceType::VertexStream || record.type == ResourceType::IndexStream)
                        {
                            valid &= Check(record.encoding != 0, "full writer publishes actual mesh encoded directory entries");
                        }
                    }
                }
                SceneReadLimits limited;
                limited.maxAllocationBytes =
                    scene.vertices.size() * sizeof(Vertex) + scene.indices.size() * sizeof(uint32_t) + 1024;
                const SceneReadResult rejected = ReadScene(path.string(), limited);
                valid &= Check(rejected.status.code == StatusCode::ResourceLimit && !rejected.scene,
                               "scene allocation budget includes expanded buffers plus retained canonical records");
            }
        }
    }
    return valid;
}
} // namespace

int main(int argc, char** argv)
{
    bool valid                        = TestStream(ResourceType::VertexStream);
    valid                            &= TestStream(ResourceType::IndexStream);
    const int64_t               tick  = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("zenasset-mesh-" + std::to_string(tick) + ".zenasset");
    Scene scene;
    scene.name = "mesh coding";
    scene.vertices.resize(9000);
    scene.indices.resize(70000);
    for (size_t index = 0; index < scene.indices.size(); ++index)
    {
        scene.indices[index] = static_cast<uint32_t>((index * 7) % scene.vertices.size());
    }
    valid &= TestScene(path, scene);
    for (int index = 1; index < argc; ++index)
    {
        SceneImportContext context;
        context.workerCount               = 2;
        const SceneImportResult imported  = ImportScene({argv[index]}, context);
        valid                            &= Check(bool(imported.asset), imported.error.c_str());
        if (imported.asset)
        {
            valid &= TestScene(path, *imported.asset);
        }
    }
    std::error_code error;
    std::filesystem::remove(path, error);
    std::printf("Mesh encoding: %u checks, %s\n", checks, valid ? "passed" : "failed");
    return valid ? 0 : 1;
}
