#pragma once
#include "ZenAsset/Format/BuildMetadata.h"
#include "ZenAsset/Format/Status.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zenasset
{
using SceneImportStatus = StatusCode;
enum class SceneImportStage : uint32_t
{
    Queued,
    Reading,
    DecodingTextures,
    ProcessingMeshes,
    BakingAnimations,
    BuildingScene,
    AssetReady,
    Complete,
    Failed,
    Cancelled
};
using SceneDiagnosticSeverity = DiagnosticSeverity;
using SceneImportDiagnostic   = Diagnostic;
struct SceneImportFileProgress
{
    uint64_t processedFiles{0};
    uint64_t totalFiles{0};
    bool     totalKnown{false};
};
// Callbacks may run on processing workers and must be thread-safe. The caller
// retains user data until synchronous ImportScene has returned.
struct SceneImportCallbacks
{
    void* user{nullptr};
    bool  (*cancelled)(void*){nullptr};
    void  (*stageChanged)(void*, SceneImportStage){nullptr};
    void  (*fileProgressChanged)(void*, SceneImportFileProgress){nullptr};
};
class SceneImportControl : public Cancellation
{
public:
    explicit SceneImportControl(SceneImportCallbacks callbacks = {}) :
        Cancellation(callbacks.cancelled, callbacks.user), m_callbacks(callbacks)
    {}

    SceneImportStage GetStage() const
    {
        return m_stage.load(std::memory_order_acquire);
    }
    void SetStage(SceneImportStage stage)
    {
        m_stage.store(stage, std::memory_order_release);
        if (m_callbacks.stageChanged != nullptr)
        {
            m_callbacks.stageChanged(m_callbacks.user, stage);
        }
    }

    // Count distinct physical inputs, not embedded images or derived resources.
    // File callbacks are serialized, may poll or cancel, and must not mutate progress.
    SceneImportFileProgress GetFileProgress() const;
    void                    RegisterFile(const std::string& path);
    void                    CompleteFile(const std::string& path);
    void                    FinishFileDiscovery();

private:
    enum class FileProgressAction
    {
        Register,
        Complete,
        FinishDiscovery
    };
    void                          UpdateFileProgress(const std::string& path, FileProgressAction action);
    mutable std::mutex            m_fileMutex;
    std::mutex                    m_fileCallbackMutex;
    std::map<std::string, bool>   m_files;
    SceneImportFileProgress       m_fileProgress;
    std::atomic<SceneImportStage> m_stage{SceneImportStage::Queued};
    SceneImportCallbacks          m_callbacks;
};
using SceneImportSettings = SourceSettings;
struct SceneImportRequest
{
    std::string                         path;
    SceneImportSettings                 settings;
    std::shared_ptr<SceneImportControl> control;
};
struct SceneImportContext
{
    // Zero selects the available CPU count. Workers are private to one import.
    uint32_t workerCount{0};
    uint64_t maxSourceBytes{16ull * 1024 * 1024 * 1024};
    // Cumulative decoded/converted image buffer bytes, including temporary
    // copies. Parser/codec-private allocations and geometry have separate limits.
    // Allow large scenes with several GiB of textures plus decoder copies.
    uint64_t maxDecodedImageBytes{16ull * 1024 * 1024 * 1024};
    // OBJ source normalization and expanded geometry storage. Parser-private
    // allocator overhead is excluded; source/count preflight also bounds it.
    uint64_t maxObjGeometryBytes{16ull * 1024 * 1024 * 1024};
    // glTF/FBX requested geometry, skin/morph/custom/animation buffers and
    // decoder-conversion/expansion scratch. Conservative capacity-growth charges
    // include simultaneous worker-result and destination storage. Parser/codec-
    // private storage, graph metadata and allocator overhead are excluded; this
    // is a requested-storage admission limit, not an RSS or final-size limit.
    // OBJ retains the separate maxObjGeometryBytes policy above.
    uint64_t maxGeometryBytes{16ull * 1024 * 1024 * 1024};
};
struct SceneImportReport
{
    std::string                        error;
    SceneImportStatus                  status{SceneImportStatus::InvalidData};
    std::vector<SceneImportDiagnostic> diagnostics;
    std::string                        format;
    uint32_t                           importerVersion{0};
};
struct SceneImportResult : SceneImportReport
{
    // Owns all canonical data. Images are immutable shared allocations; parser
    // memory and workers are released before ImportScene returns.
    std::shared_ptr<const Scene> asset;
};
struct SceneFormatDescriptor
{
    std::string_view                  id;
    std::string_view                  displayName;
    std::span<const std::string_view> extensions;
    uint32_t                          importerVersion{1};
};
std::span<const SceneFormatDescriptor> GetSceneImportFormats();
const SceneFormatDescriptor*           FindSceneImportFormat(const std::string& path);
SceneImportResult                      ImportScene(const SceneImportRequest& request, const SceneImportContext& context = {});
} // namespace zenasset
