#pragma once
#include "ZenAsset/Processor/Build.h"
#include <filesystem>
#include <mutex>
namespace zenasset::detail
{
std::timed_mutex& AssemblyBuildGate();
Status            ValidateBuildJob(const SceneBuildJob& job);
std::string       BuildSettingsKey(const SceneBuildJob& job, std::string_view format, uint32_t importerVersion);
bool              Cancelled(const Cancellation* cancellation);
bool              PollImport(void* user);
bool              Alias(const std::filesystem::path& a, const std::filesystem::path& b);
Status            Resolve(const std::string& text, std::filesystem::path& path);
bool              Within(const std::filesystem::path& child, const std::filesystem::path& root);
uint64_t          InputBytes(const std::vector<SceneDependency>& inputs);
std::string       ContentKey(const std::string& settings, const std::vector<SceneDependency>& inputs);
Status            CheckInputs(std::vector<SceneDependency>& inputs, bool fill, bool exactStamp, SceneImportControl& control);
} // namespace zenasset::detail
