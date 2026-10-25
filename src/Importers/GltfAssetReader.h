#pragma once
#include <atomic>
#include <filesystem>
#include <vector>
#include <map>
#include <mutex>
#include <string>
#include <fastgltf/core.hpp>
#include <fastgltf/types.hpp>
#include <fastgltf/tools.hpp>
#include "ZenAsset/Format/Scene.h"
#include "Processor/Backend.h"
#include "Importers/ImportErrors.h"
#include "Format/MathOps.h"
#include "Processor/WorkerPool.h"

namespace zenasset
{
// Per-import paths are frozen before texture workers start.
struct GltfFileProgress
{
    SceneImportControl*      control{nullptr};
    std::string              root;
    std::vector<std::string> imageFiles;
    void                     CompleteFile(const std::string& path) const;
    void                     CompleteImage(uint32_t index) const;
};

class GltfAssetReader
{
public:
    // Borrow a shared processing pool, or create a bounded private pool. The caller
    // must keep that pool running and unresized during import, and must not run
    // LoadAsset on one of its workers. One loader serves one caller at a time.
    explicit GltfAssetReader(const BackendContext& context);

    std::unique_ptr<Scene> LoadAsset(const std::string&         path,
                                     const SceneImportSettings& settings = {},
                                     SceneImportControl*        control  = nullptr);

    // Empty after a successful import.
    SceneImportStatus GetStatus() const
    {
        return m_errors.Status();
    }

    std::string GetError() const
    {
        return m_errors.Message();
    }

private:
    void ImportFile(const std::filesystem::path& filePath, bool binary, Scene* pScene);

    void                               ImportAsset(Scene* pScene);
    void                               LoadGltfSamplers(Scene* pScene);
    void                               LoadGltfTextures(Scene* pScene);
    void                               LoadGltfMaterials(Scene* pScene);
    void                               LoadGltfMeshes(Scene* pScene);
    void                               LoadGltfAssetData(Scene* pScene);
    void                               LoadGltfRenderableNodes(Scene* pScene);
    void                               LoadGltfInstances(uint32_t nodeIndex, Scene& scene);
    std::shared_ptr<const ImagePixels> DecodeGltfImage(uint32_t imageIndex);

    struct PrimitiveData;

    PrimitiveData ProcessPrimitive(const fastgltf::Primitive& primitive);

    const fastgltf::Accessor* GetVertexAccessor(const fastgltf::Primitive& primitive,
                                                const char*                name,
                                                fastgltf::AccessorType     type,
                                                size_t                     vertexCount) const;

    bool LoadPrimitive(const fastgltf::Primitive& primitive,
                       std::vector<Vertex>&       vertices,
                       std::vector<uint32_t>&     indices,
                       std::vector<uint32_t>&     sourceVertices,
                       bool&                      flatNormals);


    std::string       m_name;
    std::string       m_json;
    std::string       m_sourceJson;
    fastgltf::Options m_loadOptions;
    fastgltf::Parser  m_gltfParser;
    fastgltf::Asset   m_gltfAsset;

    std::vector<uint32_t>      m_linearTextureIndices;
    std::vector<uint32_t>      m_textureIndices;
    std::vector<uint32_t>      m_samplerIndices;
    std::vector<uint32_t>      m_uniqueTextureIndices;
    std::vector<PixelFormat>   m_textureFormats;
    std::map<size_t, uint32_t> m_texCoordIndices;
    GltfFileProgress           m_fileProgress;
    ImportErrors               m_errors;
    SceneImportSettings        m_settings;

    std::unique_ptr<WorkerPool> m_ownedWorkers;
    WorkerPool*                 m_workers{nullptr};
    ImageBudget*                m_imageBudget{nullptr};
    GeometryBudget*             m_geometryBudget{nullptr};
};
} // namespace zenasset
