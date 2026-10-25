#pragma once
#include "Importers/ImportErrors.h"
#include <tiny_obj_loader.h>
#include <filesystem>
#include <map>

namespace zenasset
{
struct ObjMaterialFields
{
    bool roughness{false};
    bool metalness{false};
    bool diffuse{false};
    bool specular{false};
    bool emission{false};
    bool illumination{false};
};
// Shared OBJ/MTL state is private to one synchronous import.
struct ObjImportState
{
    const SceneImportRequest&          request;
    SceneImportResult&                 report;
    Scene&                             scene;
    ImportErrors                       errors;
    uint64_t                           remainingSourceBytes{0};
    uint64_t                           remainingGeometryBytes{0};
    std::vector<std::filesystem::path> materialDirectories;
    std::vector<ObjMaterialFields>     materialFields;

    ObjImportState(const SceneImportRequest& source, const BackendContext& context, SceneImportResult& result, Scene& asset);
    bool Charge(uint64_t count, uint64_t stride);
    void Warn(const std::string& object, const std::string& message);
    void CompleteDependency(const std::filesystem::path& path);
    bool ReadFile(const std::filesystem::path& path, std::string& bytes, bool optional = false);
};

class ObjMaterialReader final : public tinyobj::MaterialReader
{
public:
    explicit ObjMaterialReader(ObjImportState& state) : m_state(state) {}
    bool operator()(const std::string&                materialFile,
                    std::vector<tinyobj::material_t>* materials,
                    std::map<std::string, int>*       materialMap,
                    std::string*                      warning,
                    std::string*                      error) override;

private:
    ObjImportState& m_state;
};

void ReadObjMaterials(ObjImportState& state, const std::vector<tinyobj::material_t>& materials);
bool ParseObjNumbers(const std::string& text, std::vector<float>& values);
} // namespace zenasset
