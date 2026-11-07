#include "ZenAsset/Processor/Import.h"
#include "ZenAsset/Processor/Cook.h"
#include "ZenAsset/Processor/Write.h"
#include "ZenAsset/Reader/Scene.h"
#include "SceneValueComparison.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <future>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
bool Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
    }
    return condition;
}
bool CheckControlFlow()
{
    zenasset::SceneImportRequest request;
    bool valid = Check(zenasset::ImportScene(request).status == zenasset::StatusCode::InvalidArgument, "Empty path must fail");
    request.path  = "\xed\xa0\x80.gltf";
    valid        &= Check(zenasset::ImportScene(request).status == zenasset::StatusCode::InvalidArgument,
                          "Invalid UTF-8 path must fail without throwing");
    request.path  = "unsupported.file";
    valid &=
        Check(zenasset::ImportScene(request).status == zenasset::StatusCode::UnsupportedFormat, "Unknown format must fail");
    request.path = "does-not-exist.gltf";
    valid &=
        Check(zenasset::ImportScene(request).status == zenasset::StatusCode::SourceUnavailable, "Missing source must fail");
    request.control = std::make_shared<zenasset::SceneImportControl>();
    request.control->Cancel();
    const zenasset::SceneImportResult cancelled = zenasset::ImportScene(request);
    valid &= Check(cancelled.status == zenasset::StatusCode::Cancelled && !cancelled.asset,
                   "Pre-cancelled request must publish no asset");
    valid &= Check(request.control->GetStage() == zenasset::SceneImportStage::Cancelled, "Cancelled stage must be visible");
    valid &= Check(zenasset::GetSceneImportFormats().size() == 3, "All source backends must be registered");
    return valid;
}
bool Inspect(const std::string& path)
{
    zenasset::SceneImportRequest request;
    request.path    = path;
    request.control = std::make_shared<zenasset::SceneImportControl>();
    zenasset::SceneImportContext context;
    context.workerCount                      = 4;
    const zenasset::SceneImportResult result = zenasset::ImportScene(request, context);
    const bool                        valid  = result.status == zenasset::StatusCode::Success && result.asset;
    if (valid)
    {
        const zenasset::Scene& scene = *result.asset;
        std::vector<uint32_t>  pendingNodes;
        std::vector<bool>      selected(scene.nodes.size(), false);
        for (zenasset::NodeId root : scene.roots)
        {
            pendingNodes.push_back(root.value);
        }
        size_t selectedNodes = 0;
        while (!pendingNodes.empty())
        {
            const uint32_t node = pendingNodes.back();
            pendingNodes.pop_back();
            if (!selected[node])
            {
                selected[node] = true;
                ++selectedNodes;
                for (zenasset::NodeId child : scene.nodes[node].children)
                {
                    pendingNodes.push_back(child.value);
                }
            }
        }
        size_t primitives = 0;
        size_t channels   = 0;
        for (const zenasset::MeshAsset& mesh : scene.meshes)
        {
            primitives += mesh.primitives.size();
        }
        for (const zenasset::AnimationAsset& animation : scene.animations)
        {
            channels += animation.channels.size();
        }
        std::cout << "{\"vertices\":" << scene.vertices.size() << ",\"indices\":" << scene.indices.size()
                  << ",\"nodes\":" << scene.nodes.size() << ",\"selected_nodes\":" << selectedNodes
                  << ",\"meshes\":" << scene.meshes.size() << ",\"submeshes\":" << primitives
                  << ",\"materials\":" << scene.materials.size() << ",\"textures\":" << scene.textures.size()
                  << ",\"skins\":" << scene.skins.size() << ",\"animations\":" << scene.animations.size()
                  << ",\"animation_channels\":" << channels << ",\"morph_primitives\":" << scene.morphPrimitives.size()
                  << ",\"material_variants\":" << scene.materialVariants.size()
                  << ",\"variant_primitives\":" << scene.variantPrimitives.size()
                  << ",\"extra_vertex_attributes\":" << scene.extraVertexAttributes.size()
                  << ",\"images\":" << scene.images.size() << ",\"cameras\":" << scene.cameras.size()
                  << ",\"lights\":" << scene.lights.size() << ",\"image_based_lights\":" << scene.imageBasedLights.size()
                  << ",\"diagnostics\":" << result.diagnostics.size() << "}\n";
    }
    else
    {
        std::cerr << static_cast<uint32_t>(result.status) << ": " << result.error << '\n';
    }
    return valid;
}

struct CancelAtDecode
{
    std::atomic<bool> cancelled{false};
};
bool ReadCancellation(void* user)
{
    return static_cast<CancelAtDecode*>(user)->cancelled.load();
}
void TrackStage(void* user, zenasset::SceneImportStage stage)
{
    if (stage == zenasset::SceneImportStage::DecodingTextures)
    {
        static_cast<CancelAtDecode*>(user)->cancelled.store(true);
    }
}
struct WriteProgress
{
    uint64_t                completed{0};
    uint64_t                total{0};
    bool                    valid{true};
    zenasset::Cancellation* cancellation{nullptr};
};
void TrackWriteProgress(void* user, uint64_t completed, uint64_t total)
{
    WriteProgress& progress  = *static_cast<WriteProgress*>(user);
    progress.valid          &= total != 0 && completed <= total
                            && (progress.total == 0 ? completed == 0 : total == progress.total && completed == progress.completed + 1);
    progress.completed       = completed;
    progress.total           = total;
    if (progress.cancellation != nullptr && completed != 0)
    {
        progress.cancellation->Cancel();
    }
}
bool CheckEncodedImages(const std::string& path)
{
    zenasset::SceneImportRequest request;
    request.path = path;
    zenasset::SceneImportContext context;
    context.workerCount                        = 2;
    const zenasset::SceneImportResult resident = zenasset::ImportScene(request, context);
    context.retainEncodedGltfImages            = true;
    const zenasset::SceneImportResult deferred = zenasset::ImportScene(request, context);
    bool valid = Check(resident.asset && deferred.asset, "Both image storage policies must import successfully");
    if (valid)
    {
        valid &= Check(!resident.asset->images.empty() && resident.asset->images.size() == deferred.asset->images.size(),
                       "Deferred import must preserve images");
        for (size_t index = 0; valid && index < resident.asset->images.size(); ++index)
        {
            const std::shared_ptr<const zenasset::ImagePixels>& descriptor = deferred.asset->images[index].pixels;
            const zenasset::ImagePixels&                        expected   = *resident.asset->images[index].pixels;
            valid &= Check(descriptor && descriptor->payloadSource && descriptor->bytes.empty(),
                           "Encoded image must not retain decoded base pixels");
            if (valid)
            {
                for (const std::vector<uint8_t>& mip : descriptor->mips)
                {
                    valid &= Check(mip.empty(), "Encoded image must not retain decoded mip pixels");
                }
                std::shared_ptr<const zenasset::ImagePixels> lease;
                const zenasset::Status                       decoded = zenasset::ResolveImagePixels(descriptor, lease);
                valid &= Check(decoded.Succeeded() && lease && lease->bytes == expected.bytes && lease->mips == expected.mips,
                               "Encoded lease must preserve every pixel and mip");
                std::weak_ptr<const zenasset::ImagePixels> released = lease;
                lease.reset();
                valid &= Check(released.expired(), "Releasing a lease must free decoded storage");
                zenasset::Cancellation cancelled;
                cancelled.Cancel();
                valid &=
                    Check(zenasset::ResolveImagePixels(descriptor, lease, &cancelled).code == zenasset::StatusCode::Cancelled
                              && !lease,
                          "Cancelled image resolution must publish no pixels");
            }
        }
        const zenasset::SceneCookResult cooked = zenasset::CookScene(deferred.asset);
        valid &= Check(cooked.status.Succeeded(), "Reference cooking must accept encoded images");
        const std::filesystem::path output =
            std::filesystem::temp_directory_path()
            / ("zen_encoded_image_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
               + ".zenasset");
        WriteProgress               progress;
        zenasset::SceneWriteOptions options;
        options.progressUser            = &progress;
        options.resourceProgress        = &TrackWriteProgress;
        const zenasset::Status written  = zenasset::WriteScene(output.string(), *deferred.asset, options);
        valid                          &= Check(written.Succeeded(), "Encoded images must serialize through temporary leases");
        valid                          &= Check(progress.valid && progress.total != 0 && progress.completed == progress.total,
                                                "Cache progress must count each written resource exactly once");
        zenasset::Cancellation cancellation;
        progress               = {};
        progress.cancellation  = &cancellation;
        valid                 &= Check(zenasset::WriteScene(output.string(), *deferred.asset, options, &cancellation).code
                                           == zenasset::StatusCode::Cancelled,
                                       "A progress callback may cancel writing without replacing the previous package");
        const zenasset::SceneReadResult loaded = zenasset::ReadScene(output.string());
        valid &= Check(loaded.status.Succeeded() && loaded.scene, "Serialized encoded images must load");
        if (loaded.scene)
        {
            valid &= Check(loaded.scene->images.size() == resident.asset->images.size(), "Serialization preserves image count");
            for (size_t index = 0; valid && index < resident.asset->images.size(); ++index)
            {
                valid &= Check(loaded.scene->images[index].pixels->bytes == resident.asset->images[index].pixels->bytes
                                   && loaded.scene->images[index].pixels->mips == resident.asset->images[index].pixels->mips,
                               "Serialization preserves image contents");
            }
        }
        std::error_code error;
        std::filesystem::remove(output, error);
        context.maxDecodedImageBytes = 0;
        valid &= Check(zenasset::ImportScene(request, context).status == zenasset::StatusCode::ResourceLimit,
                       "Encoded retention must still enforce decode budgets during validation");
    }
    return valid;
}

bool CheckLimits(const std::string& path)
{
    zenasset::SceneImportRequest request;
    request.path = path;
    zenasset::SceneImportContext context;
    context.workerCount                           = 2;
    context.maxSourceBytes                        = 0;
    const zenasset::SceneImportResult sourceLimit = zenasset::ImportScene(request, context);
    bool valid                   = Check(sourceLimit.status == zenasset::StatusCode::ResourceLimit && !sourceLimit.asset,
                                         "Source byte limit must reject without publishing data");
    context.maxSourceBytes       = UINT64_MAX;
    context.maxDecodedImageBytes = 0;
    const zenasset::SceneImportResult imageLimit = zenasset::ImportScene(request, context);
    valid &= Check(imageLimit.status == zenasset::StatusCode::ResourceLimit && !imageLimit.asset,
                   "Image byte limit must reject before allocating pixels");
    CancelAtDecode state;
    request.control =
        std::make_shared<zenasset::SceneImportControl>(zenasset::SceneImportCallbacks{&state, ReadCancellation, TrackStage});
    context.maxDecodedImageBytes                = UINT64_MAX;
    const zenasset::SceneImportResult cancelled = zenasset::ImportScene(request, context);
    valid &= Check(cancelled.status == zenasset::StatusCode::Cancelled && !cancelled.asset,
                   "Progress callback cancellation must publish no scene");
    request.control.reset();
    std::future<zenasset::SceneImportResult> first  = std::async(std::launch::async, zenasset::ImportScene, request, context);
    const zenasset::SceneImportResult        second = zenasset::ImportScene(request, context);
    const zenasset::SceneImportResult        concurrent = first.get();
    valid &= Check(second.status == zenasset::StatusCode::Success && concurrent.status == zenasset::StatusCode::Success
                       && second.asset != concurrent.asset,
                   "Concurrent imports must independently own valid scenes");
    return valid;
}

bool SaveBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(file);
}
void AppendBigEndian(std::vector<uint8_t>& bytes, uint32_t value)
{
    for (uint32_t shift = 32; shift != 0; shift -= 8)
    {
        bytes.push_back(static_cast<uint8_t>(value >> (shift - 8)));
    }
}
void AppendPngChunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& data)
{
    AppendBigEndian(png, static_cast<uint32_t>(data.size()));
    const size_t first = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t index = first; index < png.size(); ++index)
    {
        crc ^= png[index];
        for (uint32_t bit = 0; bit < 8; ++bit)
        {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    AppendBigEndian(png, crc ^ 0xFFFFFFFFu);
}
// A stored-deflate RGBA PNG whose width and first byte identify the image index.
std::vector<uint8_t> EncodeTestPng(uint32_t index)
{
    const uint32_t       width  = 1 + index % 4;
    const uint32_t       height = 2;
    std::vector<uint8_t> scanlines;
    for (uint32_t y = 0; y < height; ++y)
    {
        scanlines.push_back(0);
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint8_t pixel[]{static_cast<uint8_t>(index * 16 + x), static_cast<uint8_t>(y * 64),
                                  static_cast<uint8_t>(255 - index), 255};
            scanlines.insert(scanlines.end(), pixel, pixel + 4);
        }
    }
    uint32_t adlerLow  = 1;
    uint32_t adlerHigh = 0;
    for (uint8_t value : scanlines)
    {
        adlerLow  = (adlerLow + value) % 65521;
        adlerHigh = (adlerHigh + adlerLow) % 65521;
    }
    const uint16_t       length  = static_cast<uint16_t>(scanlines.size());
    const uint16_t       inverse = static_cast<uint16_t>(~length);
    std::vector<uint8_t> zlib{0x78,
                              0x01,
                              0x01,
                              static_cast<uint8_t>(length),
                              static_cast<uint8_t>(length >> 8),
                              static_cast<uint8_t>(inverse),
                              static_cast<uint8_t>(inverse >> 8)};
    zlib.insert(zlib.end(), scanlines.begin(), scanlines.end());
    AppendBigEndian(zlib, (adlerHigh << 16) | adlerLow);
    std::vector<uint8_t> header;
    AppendBigEndian(header, width);
    AppendBigEndian(header, height);
    header.insert(header.end(), {8, 6, 0, 0, 0});
    std::vector<uint8_t> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    AppendPngChunk(png, "IHDR", header);
    AppendPngChunk(png, "IDAT", zlib);
    AppendPngChunk(png, "IEND", {});
    return png;
}
// A textured triangle with one texture per distinct external PNG.
bool WriteDecodeFixture(const std::filesystem::path& directory, uint32_t imageCount)
{
    const float          positions[]{0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const float          texCoords[]{0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    const uint16_t       indices[]{0, 1, 2};
    std::vector<uint8_t> buffer(sizeof(positions) + sizeof(texCoords) + sizeof(indices));
    std::memcpy(buffer.data(), positions, sizeof(positions));
    std::memcpy(buffer.data() + sizeof(positions), texCoords, sizeof(texCoords));
    std::memcpy(buffer.data() + sizeof(positions) + sizeof(texCoords), indices, sizeof(indices));
    std::error_code error;
    bool            valid =
        std::filesystem::create_directories(directory, error) && !error && SaveBytes(directory / "triangle.bin", buffer);
    std::string images;
    std::string textures;
    for (uint32_t index = 0; valid && index < imageCount; ++index)
    {
        const std::string name   = "image" + std::to_string(index) + ".png";
        const std::string comma  = index == 0 ? "" : ",";
        valid                   &= SaveBytes(directory / name, EncodeTestPng(index));
        images                  += comma + "{\"uri\":\"" + name + "\"}";
        textures                += comma + "{\"source\":" + std::to_string(index) + "}";
    }
    const std::string json =
        R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"indices":2,"material":0}]}],)"
        R"("buffers":[{"byteLength":66,"uri":"triangle.bin"}],"bufferViews":[{"buffer":0,"byteLength":36},)"
        R"({"buffer":0,"byteOffset":36,"byteLength":24},{"buffer":0,"byteOffset":60,"byteLength":6}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
        R"({"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"},)"
        R"({"bufferView":2,"componentType":5123,"count":3,"type":"SCALAR"}],)"
        R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],"images":[)"
        + images + R"(],"textures":[)" + textures + "]}";
    valid &= SaveBytes(directory / "decode_limit.gltf", std::vector<uint8_t>(json.begin(), json.end()));
    return valid;
}
// A worker's decode is in flight from its first cancellation poll until its
// image file completes. That first poll stalls, so unbounded decodes overlap.
struct DecodeObservation
{
    std::thread::id              caller{std::this_thread::get_id()};
    std::atomic<bool>            decoding{false};
    std::mutex                   mutex;
    std::vector<std::thread::id> active;
    size_t                       peak{0};
    uint32_t                     completed{0};
};
bool ObserveDecodePoll(void* user)
{
    DecodeObservation&    observation = *static_cast<DecodeObservation*>(user);
    const std::thread::id thread      = std::this_thread::get_id();
    bool                  started     = false;
    if (observation.decoding.load() && thread != observation.caller)
    {
        std::lock_guard<std::mutex> lock(observation.mutex);
        if (std::find(observation.active.begin(), observation.active.end(), thread) == observation.active.end())
        {
            observation.active.push_back(thread);
            observation.peak = std::max(observation.peak, observation.active.size());
            started          = true;
        }
    }
    if (started)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}
void ObserveDecodeStage(void* user, zenasset::SceneImportStage stage)
{
    static_cast<DecodeObservation*>(user)->decoding.store(stage == zenasset::SceneImportStage::DecodingTextures);
}
void ObserveDecodeCompletion(void* user, zenasset::SceneImportFileProgress)
{
    DecodeObservation&    observation = *static_cast<DecodeObservation*>(user);
    const std::thread::id thread      = std::this_thread::get_id();
    if (observation.decoding.load() && thread != observation.caller)
    {
        std::lock_guard<std::mutex> lock(observation.mutex);
        observation.active.erase(std::remove(observation.active.begin(), observation.active.end(), thread),
                                 observation.active.end());
        ++observation.completed;
    }
}
zenasset::SceneImportResult ImportObserved(const std::string& path,
                                           uint32_t           decodeLimit,
                                           bool               retainEncoded,
                                           DecodeObservation& observation)
{
    zenasset::SceneImportRequest request;
    request.path    = path;
    request.control = std::make_shared<zenasset::SceneImportControl>(
        zenasset::SceneImportCallbacks{&observation, ObserveDecodePoll, ObserveDecodeStage, ObserveDecodeCompletion});
    zenasset::SceneImportContext context;
    context.workerCount               = 8;
    context.maxConcurrentImageDecodes = decodeLimit;
    context.retainEncodedGltfImages   = retainEncoded;
    return zenasset::ImportScene(request, context);
}
bool CheckDecodeResults(const zenasset::Scene& baseline,
                        const zenasset::Scene& bounded,
                        const zenasset::Scene& encoded,
                        uint32_t               imageCount)
{
    bool valid = Check(baseline.images.size() == imageCount && encoded.images.size() == imageCount,
                       "Every fixture image must be imported");
    for (uint32_t index = 0; valid && index < imageCount; ++index)
    {
        const zenasset::ImagePixels& expected = *baseline.images[index].pixels;
        valid &= Check(expected.width == 1 + index % 4 && !expected.bytes.empty() && expected.bytes[0] == index * 16,
                       "Fixture images must decode distinct pixels in source order");
        std::shared_ptr<const zenasset::ImagePixels> lease;
        const zenasset::Status resolved = zenasset::ResolveImagePixels(encoded.images[index].pixels, lease);
        valid &= Check(resolved.Succeeded() && lease && lease->width == expected.width && lease->height == expected.height
                           && lease->bytes == expected.bytes && lease->mips == expected.mips,
                       "A decode limit must preserve encoded image order and pixels");
    }
    valid &= Check(scene_test::Same(baseline, bounded), "A decode limit must not change imported scene values");
    return valid;
}
// Eight workers with a decode limit of two keep at most two decodes in flight,
// with resident or encoded image retention, and import the same scene values.
bool CheckDecodeLimit()
{
    const uint32_t              imageCount = 16;
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path()
        / ("zen_decode_limit_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::string path  = (directory / "decode_limit.gltf").string();
    bool              valid = Check(WriteDecodeFixture(directory, imageCount), "Decode-limit fixture must be written");
    if (valid)
    {
        DecodeObservation                 unlimited;
        const zenasset::SceneImportResult baseline = ImportObserved(path, 0, false, unlimited);
        DecodeObservation                 limited;
        const zenasset::SceneImportResult bounded = ImportObserved(path, 2, false, limited);
        DecodeObservation                 limitedEncoded;
        const zenasset::SceneImportResult encoded   = ImportObserved(path, 2, true, limitedEncoded);
        const uint32_t                    available = std::thread::hardware_concurrency();
        valid &= Check(baseline.asset && bounded.asset && encoded.asset, "Every decode-limit import must succeed");
        valid &= Check(unlimited.completed == imageCount && limited.completed == imageCount
                           && limitedEncoded.completed == imageCount,
                       "Every image decode must be observed");
        valid &= Check(available < 3 || unlimited.peak > 2, "Eight unlimited workers must overlap more than two decodes");
        valid &= Check(limited.peak <= 2 && limitedEncoded.peak <= 2,
                       "A decode limit of two must keep at most two decodes in flight");
        valid &= Check(available < 2 || (limited.peak == 2 && limitedEncoded.peak == 2),
                       "A decode limit of two must still overlap two decodes");
        if (valid)
        {
            valid &= CheckDecodeResults(*baseline.asset, *bounded.asset, *encoded.asset, imageCount);
        }
        std::cout << "decode peaks unlimited=" << unlimited.peak << " limited=" << limited.peak
                  << " limited-encoded=" << limitedEncoded.peak << '\n';
    }
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    return valid;
}
} // namespace
int RunMain(const std::vector<std::string>& arguments)
{
    bool valid = CheckControlFlow();
    if (arguments.size() == 3 && arguments[1] == "--encoded")
    {
        valid &= CheckEncodedImages(arguments[2]);
    }
    else if (arguments.size() == 3 && arguments[1] == "--limits")
    {
        valid &= CheckLimits(arguments[2]);
    }
    else if (arguments.size() == 2 && arguments[1] == "--decode-limit")
    {
        valid &= CheckDecodeLimit();
    }
    else
    {
        for (size_t index = 1; index < arguments.size(); ++index)
        {
            valid &= Inspect(arguments[index]);
        }
    }
    return valid ? 0 : 1;
}
#if defined(_WIN32)
int wmain(int argc, wchar_t** argv)
{
    std::vector<std::string> arguments;
    for (int index = 0; index < argc; ++index)
    {
        const std::u8string value = std::filesystem::path(argv[index]).u8string();
        arguments.emplace_back(reinterpret_cast<const char*>(value.data()), value.size());
    }
    return RunMain(arguments);
}
#else
int main(int argc, char** argv)
{
    const std::vector<std::string> arguments(argv, argv + argc);
    return RunMain(arguments);
}
#endif
