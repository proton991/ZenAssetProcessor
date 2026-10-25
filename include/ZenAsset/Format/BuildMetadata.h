#pragma once
#include "ZenAsset/Format/Scene.h"
#include "ZenAsset/Format/Compression.h"
#include "ZenAsset/Format/Processing.h"
#include "ZenAsset/Format/Status.h"
#include "ZenAsset/Format/Version.h"

namespace zenasset
{
struct SourceSettings
{
    uint32_t              sceneIndex{UINT32_MAX};
    AnimationBakeSettings animationBake;
    bool                  fallbackMissingNamedUVSets{true};
};
enum class DiagnosticSeverity : uint32_t
{
    Warning,
    Error
};
struct Diagnostic
{
    DiagnosticSeverity severity{DiagnosticSeverity::Error};
    StatusCode         status{StatusCode::InvalidData};
    std::string        source;
    std::string        object;
    std::string        message;
};
// Provenance is separate from runtime chunks. Incidental paths/timestamps affect
// this record only; source-independent scene loading never consults these files.
struct BuildMetadata
{
    std::string    sourceFormat;
    uint32_t       importerVersion{0};
    uint32_t       processorVersion{ProcessorVersion};
    std::string    buildFingerprint;
    std::string    profile{"reference"};
    SourceSettings settings;
    std::string    cacheKey;
    // Build-time root source and dependency revisions, including content hashes.
    // Kept separate from the canonical scene's import-time dependency stamps.
    std::vector<SceneDependency> inputs;
    std::vector<Diagnostic>      diagnostics;
    // Requested policy; each resource directory entry records its actual codec.
    CompressionSettings compression;
    // Policy already applied to the scene by CookScene. The writer records it
    // without reprocessing geometry or images.
    ProcessingSettings processing;
};
} // namespace zenasset
