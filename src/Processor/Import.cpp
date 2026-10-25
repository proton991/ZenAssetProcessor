#include "Format/FileIO.h"
#include "Processor/Backend.h"
#include "Importers/GltfAssetReader.h"
#include "Importers/FbxSceneImporter.h"
#include "Importers/ObjSceneImporter.h"
#include "Importers/AssetPaths.h"
#include "Processing/SceneAssetProcessing.h"
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace zenasset
{
namespace
{
class GltfSceneImporter final : public ISceneImporter
{
public:
    SceneImportResult Import(const SceneImportRequest& request, const BackendContext& context) const override
    {
        SceneImportResult      result;
        GltfAssetReader        importer(context);
        std::unique_ptr<Scene> candidate = importer.LoadAsset(request.path, request.settings, request.control.get());
        if (candidate && ProcessImportedAsset(*candidate, result, request.path, context.geometry))
        {
            result.asset  = std::move(candidate);
            result.status = SceneImportStatus::Success;
        }
        else if (!candidate)
        {
            const SceneImportStatus status =
                request.control && request.control->IsCancelled() ? SceneImportStatus::Cancelled : importer.GetStatus();
            SetSceneImportFailure(result, status, request.path, importer.GetError());
        }
        return result;
    }
};
const std::string_view      gltfExtensions[]{".gltf", ".glb"};
const std::string_view      fbxExtensions[]{".fbx"};
const std::string_view      objExtensions[]{".obj"};
const SceneFormatDescriptor formats[]{{"gltf", "glTF scenes", gltfExtensions, 1},
                                      {"fbx", "FBX scenes", fbxExtensions, 1},
                                      {"obj", "Wavefront OBJ scenes", objExtensions, 1}};
const GltfSceneImporter     gltfImporter;
const FbxSceneImporter      fbxImporter;
const ObjSceneImporter      objImporter;
const ISceneImporter* const importers[]{&gltfImporter, &fbxImporter, &objImporter};
} // namespace
std::span<const SceneFormatDescriptor> GetSceneImportFormats()
{
    return formats;
}
const SceneFormatDescriptor* FindSceneImportFormat(const std::string& path)
{
    const SceneFormatDescriptor* result = nullptr;
    std::filesystem::path        source;
    if (detail::MakePath(path, source).Succeeded())
    {
        std::string extension = PathToUtf8(source.extension());
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        for (const SceneFormatDescriptor& format : formats)
        {
            for (std::string_view supported : format.extensions)
            {
                if (extension == supported)
                {
                    result = &format;
                }
            }
        }
    }
    return result;
}
void SetSceneImportFailure(SceneImportResult& result,
                           SceneImportStatus  status,
                           const std::string& source,
                           std::string        message,
                           std::string        object)
{
    result.asset.reset();
    result.status = status;
    result.error  = std::move(message);
    result.diagnostics.push_back({SceneDiagnosticSeverity::Error, status, source, std::move(object), result.error});
}
SceneImportResult ImportScene(const SceneImportRequest& request, const SceneImportContext& context)
{
    SceneImportResult            result;
    const SceneFormatDescriptor* format = FindSceneImportFormat(request.path);
    std::error_code              error;
    std::filesystem::path        source;
    const Status                 pathStatus = detail::MakePath(request.path, source);
    if (request.control && request.control->IsCancelled())
    {
        SetSceneImportFailure(result, SceneImportStatus::Cancelled, request.path, "Scene import cancelled");
    }
    else if (!pathStatus.Succeeded())
    {
        SetSceneImportFailure(result, pathStatus.code, request.path, pathStatus.message);
    }
    else if (format == nullptr)
    {
        SetSceneImportFailure(result, SceneImportStatus::UnsupportedFormat, request.path,
                              "Unsupported scene format: " + request.path);
    }
    else if (!std::filesystem::is_regular_file(source, error))
    {
        SetSceneImportFailure(result, SceneImportStatus::SourceUnavailable, request.path,
                              "Scene source is unavailable: " + request.path);
    }
    else
    {
        const uintmax_t size = std::filesystem::file_size(source, error);
        if (error)
        {
            SetSceneImportFailure(result, SceneImportStatus::IOError, request.path, "Cannot inspect scene source size");
        }
        else if (size > context.maxSourceBytes)
        {
            SetSceneImportFailure(result, SceneImportStatus::ResourceLimit, request.path,
                                  "Scene source exceeds the configured byte limit");
        }
        else
        {
            if (request.control)
            {
                request.control->RegisterFile(request.path);
                request.control->SetStage(SceneImportStage::Reading);
            }
            GeometryBudget geometry(context.maxGeometryBytes);
            WorkerPool     workers(context.workerCount);
            ImageBudget    images(context.maxDecodedImageBytes);
            result = importers[format - formats]->Import(
                request, BackendContext{&workers, &images, context.maxSourceBytes, context.maxObjGeometryBytes, &geometry});
            result.format          = std::string(format->id);
            result.importerVersion = format->importerVersion;
        }
    }
    if (request.control && request.control->IsCancelled() && result.status != SceneImportStatus::Cancelled)
    {
        SetSceneImportFailure(result, SceneImportStatus::Cancelled, request.path, "Scene import cancelled");
    }
    if (request.control)
    {
        if (result.status == SceneImportStatus::Success && result.asset)
        {
            request.control->FinishFileDiscovery();
            request.control->CompleteFile(request.path);
        }
        request.control->SetStage(result.status == SceneImportStatus::Success     ? SceneImportStage::AssetReady
                                  : result.status == SceneImportStatus::Cancelled ? SceneImportStage::Cancelled
                                                                                  : SceneImportStage::Failed);
    }
    return result;
}
} // namespace zenasset
