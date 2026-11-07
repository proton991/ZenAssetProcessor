#include "ZenAsset/Processor/Import.h"
#include "ZenAsset/Processor/Cook.h"
#include "ZenAsset/Processor/Write.h"
#include "ZenAsset/Reader/Scene.h"
#include <chrono>
#include <iostream>
#include <future>
#include <filesystem>
#include <fstream>

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
        const zenasset::Status written  = zenasset::WriteScene(output.string(), *deferred.asset);
        valid                          &= Check(written.Succeeded(), "Encoded images must serialize through temporary leases");
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
