#pragma once
#include "ZenAsset/Processor/Build.h"
namespace zenasset::detail
{
Status      ValidateBuildJob(const SceneBuildJob& job);
std::string BuildSettingsKey(const SceneBuildJob& job, std::string_view format, uint32_t importerVersion);
} // namespace zenasset::detail
