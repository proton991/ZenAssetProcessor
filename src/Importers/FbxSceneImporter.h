#pragma once
#include "Processor/Backend.h"

namespace zenasset
{
class FbxSceneImporter final : public ISceneImporter
{
public:
    SceneImportResult Import(const SceneImportRequest& request, const BackendContext& context) const override;
};
} // namespace zenasset
