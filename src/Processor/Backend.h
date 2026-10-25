#pragma once
#include "ZenAsset/Processor/Import.h"
#include "ZenAsset/Format/Validation.h"
#include "Processor/WorkerPool.h"
#include "Processor/ImageBudget.h"
#include "Processor/GeometryBudget.h"
namespace zenasset
{
struct BackendContext
{
    WorkerPool*     processing{nullptr};
    ImageBudget*    images{nullptr};
    uint64_t        maxSourceBytes{16ull * 1024 * 1024 * 1024};
    uint64_t        maxObjGeometryBytes{16ull * 1024 * 1024 * 1024};
    GeometryBudget* geometry{nullptr};
};
class ISceneImporter
{
public:
    virtual ~ISceneImporter()                                                                                = default;
    virtual SceneImportResult Import(const SceneImportRequest& request, const BackendContext& context) const = 0;
};
void SetSceneImportFailure(SceneImportResult& result,
                           SceneImportStatus  status,
                           const std::string& source,
                           std::string        message,
                           std::string        object = {});
} // namespace zenasset
