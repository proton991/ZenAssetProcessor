#include "Processor/BuildInternal.h"
#include "ZenAsset/Processor/Cook.h"
#include "Importers/AssetHash.h"
#include "Importers/AssetPaths.h"
#include "Format/FileIO.h"
#include "ZenAsset/Format/Utf8.h"
#include "Writer/Compression.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>
#if defined(_WIN32)
#    define NOMINMAX
#    include <Windows.h>
#endif

namespace zenasset
{
namespace
{
constexpr uint64_t      HashScratch = 4ull << 20;
std::mutex              batchMutex;
std::condition_variable batchChanged;
bool                    batchActive = false;

bool Cancelled(const Cancellation* cancellation)
{
    return cancellation != nullptr && cancellation->IsCancelled();
}
void HashNumber(AssetHash& hash, uint64_t value)
{
    uint8_t bytes[8]{};
    for (uint32_t i = 0; i < 8; ++i)
    {
        bytes[i] = static_cast<uint8_t>(value >> (i * 8));
    }
    hash.Update(bytes, sizeof(bytes));
}
void HashText(AssetHash& hash, std::string_view value)
{
    HashNumber(hash, value.size());
    hash.Update(value.data(), value.size());
}
bool SamePathText(const std::filesystem::path& a, const std::filesystem::path& b)
{
#if defined(_WIN32)
    const bool same = CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
    const bool same = a == b;
#endif
    return same;
}
bool Alias(const std::filesystem::path& a, const std::filesystem::path& b)
{
    std::error_code error;
    return SamePathText(a, b) || std::filesystem::equivalent(a, b, error);
}
Status Resolve(const std::string& text, std::filesystem::path& path)
{
    Status status = detail::MakePath(text, path);
    if (status.Succeeded())
    {
        std::error_code error;
        path = std::filesystem::weakly_canonical(path, error);
        if (error)
        {
            status = {StatusCode::IOError, "Cannot resolve build path: " + text};
        }
    }
    return status;
}
bool Within(const std::filesystem::path& child, const std::filesystem::path& root)
{
    std::filesystem::path::const_iterator a      = child.begin();
    std::filesystem::path::const_iterator b      = root.begin();
    bool                                  inside = true;
    while (inside && b != root.end())
    {
        inside = a != child.end() && SamePathText(*a, *b);
        if (inside)
        {
            ++a;
            ++b;
        }
    }
    return inside && a != child.end();
}
bool SafeOutput(const std::string& text)
{
    std::filesystem::path path;
    bool                  valid = detail::MakePath(text, path).Succeeded() && !path.is_absolute() && !path.has_root_name()
                               && path.extension() == ".zenasset" && text.find(':') == std::string::npos;
    for (const std::filesystem::path& part : path)
    {
        valid = valid && part != ".." && part != "." && !part.empty();
    }
    return valid;
}
uint64_t InputBytes(const std::vector<SceneDependency>& inputs)
{
    uint64_t bytes = inputs.size() * sizeof(SceneDependency);
    for (const SceneDependency& input : inputs)
    {
        bytes += input.path.size();
    }
    return bytes;
}
std::string ContentKey(const std::string& settings, const std::vector<SceneDependency>& inputs)
{
    AssetHash hash;
    HashText(hash, settings);
    HashNumber(hash, inputs.size());
    const std::filesystem::path parent = std::filesystem::u8path(inputs.front().path).parent_path();
    for (size_t index = 0; index < inputs.size(); ++index)
    {
        const SceneDependency& input = inputs[index];
        HashText(hash, index == 0 ? std::string("$source")
                                  : PathToUtf8(std::filesystem::u8path(input.path).lexically_relative(parent)));
        HashNumber(hash, input.exists);
        HashNumber(hash, input.size);
        HashNumber(hash, input.contentHash);
    }
    return DigestText(hash.Finish());
}
bool PollImport(void* user)
{
    return Cancelled(static_cast<const Cancellation*>(user));
}
Status CheckInputs(std::vector<SceneDependency>& inputs, bool fill, bool exactStamp, SceneImportControl& control)
{
    Status status;
    for (SceneDependency& input : inputs)
    {
        if (status.Succeeded() && control.IsCancelled())
        {
            status = {StatusCode::Cancelled, "Build input verification cancelled"};
        }
        if (status.Succeeded() && input.exists)
        {
            AssetFileStamp before;
            AssetFileStamp after;
            AssetDigest    hash = 0;
            const bool valid    = StampAssetFile(input.path, before) && before.size == input.size
                               && (!exactStamp || before.modified == input.modified) && HashAssetFile(input.path, hash, &control)
                               && StampAssetFile(input.path, after) && before == after && (fill || input.contentHash == hash);
            if (!valid)
            {
                status = {control.IsCancelled() ? StatusCode::Cancelled : StatusCode::SourceUnavailable,
                          "Build input changed or cannot be read: " + input.path};
            }
            else if (fill)
            {
                input.contentHash = hash;
            }
        }
        else if (status.Succeeded())
        {
            std::error_code error;
            const bool      exists = std::filesystem::exists(std::filesystem::u8path(input.path), error);
            if (error || exists)
            {
                status = {StatusCode::SourceUnavailable,
                          "Missing preferred input appeared or cannot be checked: " + input.path};
            }
        }
    }
    return status;
}
struct Plan
{
    SceneBuildJob                job;
    std::filesystem::path        source;
    std::filesystem::path        output;
    std::string                  settingsKey;
    std::vector<SceneDependency> inputs;
    bool                         reuse{false};
};

class Batch
{
public:
    Batch(std::span<const SceneBuildJob> jobs, const SceneBuildOptions& options, const Cancellation* cancellation) :
        m_jobs(jobs), m_options(options), m_cancellation(cancellation)
    {}
    SceneBuildResult Run()
    {
        m_result.status = ValidateOptions();
        if (m_result.status.Succeeded())
        {
            if (Charge(m_jobs.size() * (sizeof(Plan) + sizeof(SceneBuildJobResult))))
            {
                m_result.jobs.resize(m_jobs.size());
                m_plans.resize(m_jobs.size());
                ResolveJobs();
            }
        }
        if (m_result.status.Succeeded())
        {
            Preflight();
        }
        if (m_result.status.Succeeded())
        {
            Execute();
        }
        for (SceneBuildJobResult& job : m_result.jobs)
        {
            if (job.status.message.size() > 1024)
            {
                job.status.message.resize(1024);
            }
        }
        return std::move(m_result);
    }

private:
    void Progress(uint32_t index, SceneBuildStage stage) const
    {
        if (m_options.callbacks.progress != nullptr)
        {
            m_options.callbacks.progress(m_options.callbacks.user, index, stage);
        }
    }
    Status ValidateOptions()
    {
        Status         status;
        const uint64_t reservation = SceneBuildReservationBytes(m_options);
        if (m_jobs.empty() || m_jobs.size() > m_options.maxJobs || m_options.maxJobs > 65536 || m_options.workerCount == 0
            || m_options.workerCount > 64 || m_options.maxDiagnosticsPerJob > 4096 || m_options.maxJobMetadataBytes < 1024
            || m_options.maxPlanningBytes < 1024 || m_options.outputDirectory.size() > 4096 || reservation == UINT64_MAX
            || reservation > m_options.maxReservedMemoryBytes)
        {
            status = {StatusCode::ResourceLimit, "Batch job count, worker count or reservation exceeds configured bounds"};
        }
        if (status.Succeeded())
        {
            const uint64_t pathReservation = m_options.outputDirectory.size() * 4 + 16384;
            if (pathReservation > m_options.maxPlanningBytes)
            {
                status = {StatusCode::ResourceLimit, "Output directory exceeds the planning budget"};
            }
            else
            {
                m_planningBytes = pathReservation;
                status          = Resolve(m_options.outputDirectory, m_root);
            }
        }
        const uint64_t maximumWorkers = reservation == 0 ? 0 : m_options.maxReservedMemoryBytes / reservation;
        m_workers                     = static_cast<uint32_t>(std::min<uint64_t>(
            {m_options.workerCount, maximumWorkers, m_jobs.size(), std::max(1u, std::thread::hardware_concurrency())}));
        return status;
    }
    bool Charge(uint64_t bytes)
    {
        const bool fits = bytes <= m_options.maxPlanningBytes - m_planningBytes;
        if (fits)
        {
            m_planningBytes += bytes;
        }
        else
        {
            m_result.status = {StatusCode::ResourceLimit, "Batch planning/results exceed the metadata budget"};
        }
        return fits;
    }
    void ResolveJobs()
    {
        for (size_t index = 0; m_result.status.Succeeded() && index < m_jobs.size(); ++index)
        {
            Plan& plan      = m_plans[index];
            m_result.status = Cancelled(m_cancellation) ? Status{StatusCode::Cancelled, "Batch path preflight cancelled"}
                                                        : detail::ValidateBuildJob(m_jobs[index]);
            if (m_result.status.Succeeded() && !SafeOutput(m_jobs[index].output))
            {
                m_result.status = {StatusCode::InvalidArgument,
                                   "Build outputs must be rooted relative .zenasset paths without traversal"};
            }
            if (m_result.status.Succeeded())
            {
                Charge(m_jobs[index].source.size() * 4 + m_jobs[index].output.size() * 4 + m_jobs[index].profile.size()
                       + PathToUtf8(m_root).size() * 4 + 16384);
            }
            if (m_result.status.Succeeded())
            {
                m_result.status = Resolve(m_jobs[index].source, plan.source);
            }
            if (m_result.status.Succeeded())
            {
                m_result.status = Resolve(PathToUtf8(m_root / std::filesystem::u8path(m_jobs[index].output)), plan.output);
            }
            if (m_result.status.Succeeded() && !Within(plan.output, m_root))
            {
                m_result.status = {StatusCode::InvalidArgument, "Output resolves outside the output directory"};
            }
            if (m_result.status.Succeeded())
            {
                plan.job                            = m_jobs[index];
                plan.job.source                     = PathToUtf8(plan.source);
                m_result.jobs[index].output         = PathToUtf8(plan.output);
                const SceneFormatDescriptor* format = FindSceneImportFormat(plan.job.source);
                if (format == nullptr)
                {
                    m_result.status = {StatusCode::UnsupportedFormat, "Unsupported batch source format"};
                }
                else
                {
                    plan.settingsKey = detail::BuildSettingsKey(plan.job, format->id, format->importerVersion);
                }
            }
            if (!m_result.status.Succeeded())
            {
                m_result.jobs[index].status = m_result.status;
            }
        }
        for (size_t a = 0; m_result.status.Succeeded() && a < m_plans.size(); ++a)
        {
            for (size_t b = 0; m_result.status.Succeeded() && b < m_plans.size(); ++b)
            {
                if (Cancelled(m_cancellation))
                {
                    m_result.status         = {StatusCode::Cancelled, "Batch collision preflight cancelled"};
                    m_result.jobs[a].status = m_result.status;
                }
                else if ((a != b && Alias(m_plans[a].output, m_plans[b].output)) || Alias(m_plans[a].output, m_plans[b].source))
                {
                    m_result.status         = {StatusCode::InvalidArgument, "Batch output aliases another output or source"};
                    m_result.jobs[a].status = m_result.status;
                }
            }
        }
    }
    SceneImportContext ImportContext() const
    {
        SceneImportContext context   = m_options.importContext;
        const uint32_t     available = std::max(1u, std::thread::hardware_concurrency()) / std::max(1u, m_workers);
        context.workerCount =
            context.workerCount == 0 ? std::max(1u, available) : std::min(context.workerCount, std::max(1u, available));
        return context;
    }
    Status Import(Plan& plan, SceneImportControl& control, SceneImportResult& imported, std::vector<SceneDependency>& inputs)
    {
        AssetFileStamp rootStamp;
        Status         status;
        if (!StampAssetFile(plan.job.source, rootStamp))
        {
            status = {StatusCode::SourceUnavailable, "Cannot inspect batch source"};
        }
        if (status.Succeeded())
        {
            SceneImportRequest request;
            request.path     = plan.job.source;
            request.settings = plan.job.sourceSettings;
            request.control  = std::make_shared<SceneImportControl>(
                SceneImportCallbacks{static_cast<Cancellation*>(&control), &PollImport, nullptr});
            imported = ImportScene(request, ImportContext());
            status   = {imported.status, imported.error};
        }
        if (status.Succeeded())
        {
            if (InputBytes(imported.asset->dependencies) + plan.job.source.size() + sizeof(SceneDependency)
                > m_options.maxJobMetadataBytes)
            {
                status = {StatusCode::ResourceLimit, "Build input records exceed the per-job metadata limit"};
            }
            else
            {
                inputs.push_back({plan.job.source, true, rootStamp.size, rootStamp.modified, 0});
                inputs.insert(inputs.end(), imported.asset->dependencies.begin(), imported.asset->dependencies.end());
                status = CheckInputs(inputs, true, true, control);
            }
        }
        return status;
    }
    bool Reuse(Plan& plan, SceneImportControl& control)
    {
        bool            reusable = false;
        std::error_code error;
        if (!m_options.force && std::filesystem::is_regular_file(plan.output, error))
        {
            const SceneReadResult read = ReadScene(PathToUtf8(plan.output), m_options.limits, &control);
            if (read.status.Succeeded() && !read.metadata.inputs.empty()
                && InputBytes(read.metadata.inputs) <= m_options.maxJobMetadataBytes
                && read.metadata.processorVersion == ProcessorVersion
                && read.metadata.buildFingerprint == LibraryBuildFingerprint()
                && read.metadata.inputs.front().path == plan.job.source)
            {
                SceneBuildJob identity  = plan.job;
                identity.sourceSettings = read.metadata.settings;
                identity.processing     = read.metadata.processing;
                identity.compression    = read.metadata.compression;
                identity.profile        = read.metadata.profile;
                const std::string key =
                    detail::BuildSettingsKey(identity, read.metadata.sourceFormat, read.metadata.importerVersion);
                if (key == plan.settingsKey && read.metadata.cacheKey == ContentKey(key, read.metadata.inputs))
                {
                    plan.inputs = read.metadata.inputs;
                    reusable    = CheckInputs(plan.inputs, false, false, control).Succeeded();
                    if (!reusable)
                    {
                        plan.inputs.clear();
                    }
                }
            }
        }
        return reusable;
    }
    Status Collisions(const std::vector<SceneDependency>& inputs) const
    {
        Status status;
        for (size_t inputIndex = 0; status.Succeeded() && inputIndex < inputs.size(); ++inputIndex)
        {
            const SceneDependency& input = inputs[inputIndex];
            std::filesystem::path  path;
            if (status.Succeeded())
            {
                status = Resolve(input.path, path);
            }
            for (size_t planIndex = 0; status.Succeeded() && planIndex < m_plans.size(); ++planIndex)
            {
                const Plan& plan = m_plans[planIndex];
                if (status.Succeeded() && Cancelled(m_cancellation))
                {
                    status = {StatusCode::Cancelled, "Batch dependency collision check cancelled"};
                }
                if (status.Succeeded() && Alias(path, plan.output))
                {
                    status = {StatusCode::InvalidArgument, "Batch output aliases an input dependency: " + input.path};
                }
            }
        }
        return status;
    }
    void Preflight()
    {
        SceneImportControl control({const_cast<Cancellation*>(m_cancellation), &PollImport, nullptr});
        for (uint32_t index = 0; m_result.status.Succeeded() && index < m_plans.size(); ++index)
        {
            Progress(index, SceneBuildStage::Preflight);
            Plan& plan = m_plans[index];
            if (control.IsCancelled())
            {
                m_result.status = {StatusCode::Cancelled, "Batch preflight cancelled"};
            }
            if (m_result.status.Succeeded())
            {
                plan.reuse = Reuse(plan, control);
                if (!plan.reuse)
                {
                    SceneImportResult imported;
                    m_result.status = Import(plan, control, imported, plan.inputs);
                }
            }
            if (m_result.status.Succeeded())
            {
                m_result.status = Collisions(plan.inputs);
            }
            if (m_result.status.Succeeded() && Charge(InputBytes(plan.inputs) + m_options.maxJobMetadataBytes))
            {
                m_result.jobs[index].cacheKey = ContentKey(plan.settingsKey, plan.inputs);
            }
            if (!m_result.status.Succeeded())
            {
                m_result.jobs[index].status = m_result.status;
            }
        }
    }
    void Diagnostics(uint32_t index, const std::vector<Diagnostic>& values)
    {
        SceneBuildJobResult& result = m_result.jobs[index];
        uint64_t             bytes  = 0;
        for (const Diagnostic& value : result.diagnostics)
        {
            bytes += sizeof(Diagnostic) + value.source.size() + value.object.size() + value.message.size();
        }
        for (const Diagnostic& value : values)
        {
            const uint64_t size = sizeof(Diagnostic) + value.source.size() + value.object.size() + value.message.size();
            if (result.diagnostics.size() < m_options.maxDiagnosticsPerJob && size <= m_options.maxJobMetadataBytes - bytes)
            {
                result.diagnostics.push_back(value);
                bytes += size;
            }
            else if (result.diagnosticsOmitted != UINT32_MAX)
            {
                ++result.diagnosticsOmitted;
            }
        }
    }
    struct Publication
    {
        Batch*                        batch;
        uint32_t                      index;
        std::vector<SceneDependency>* inputs;
        SceneImportControl*           control;
    };
    static Status BeforePublish(void* user)
    {
        Publication& publication = *static_cast<Publication*>(user);
        Batch&       batch       = *publication.batch;
        batch.Progress(publication.index, SceneBuildStage::Publishing);
        Status                status;
        std::filesystem::path output;
        std::filesystem::path root;
        if (status.Succeeded())
        {
            status = Resolve(batch.m_options.outputDirectory, root);
            if (status.Succeeded() && !SamePathText(root, batch.m_root))
            {
                status = {StatusCode::InvalidArgument, "Output directory resolution changed during the build"};
            }
        }
        if (status.Succeeded())
        {
            status = Resolve(PathToUtf8(batch.m_root / std::filesystem::u8path(batch.m_plans[publication.index].job.output)),
                             output);
        }
        if (status.Succeeded()
            && (!SamePathText(output, batch.m_plans[publication.index].output) || !Within(output, batch.m_root)))
        {
            status = {StatusCode::InvalidArgument, "Output resolution changed during the build"};
        }
        if (status.Succeeded())
        {
            status = batch.Collisions(*publication.inputs);
        }
        if (status.Succeeded())
        {
            status = CheckInputs(*publication.inputs, false, true, *publication.control);
        }
        return status;
    }
    void RunJob(uint32_t index)
    {
        Plan&                plan   = m_plans[index];
        SceneBuildJobResult& result = m_result.jobs[index];
        SceneImportControl   control({const_cast<Cancellation*>(m_cancellation), &PollImport, nullptr});
        result.status = CheckInputs(plan.inputs, false, false, control);
        if (result.status.Succeeded() && plan.reuse)
        {
            Progress(index, SceneBuildStage::Reused);
            result.status = CheckInputs(plan.inputs, false, false, control);
            if (result.status.Succeeded())
            {
                result.disposition = SceneBuildDisposition::Reused;
            }
        }
        else if (result.status.Succeeded())
        {
            Progress(index, SceneBuildStage::Importing);
            SceneImportResult            imported;
            std::vector<SceneDependency> inputs;
            result.status = Import(plan, control, imported, inputs);
            Diagnostics(index, imported.diagnostics);
            if (result.status.Succeeded()
                && (detail::BuildSettingsKey(plan.job, imported.format, imported.importerVersion) != plan.settingsKey
                    || ContentKey(plan.settingsKey, inputs) != result.cacheKey))
            {
                result.status = {StatusCode::SourceUnavailable, "Inputs changed after batch preflight"};
            }
            if (result.status.Succeeded())
            {
                result.status = Collisions(inputs);
            }
            SceneCookResult cooked;
            if (result.status.Succeeded())
            {
                Progress(index, SceneBuildStage::Cooking);
                cooked        = CookScene(imported.asset, SceneCookOptions{plan.job.processing, m_options.limits}, &control);
                result.status = cooked.status;
                Diagnostics(index, cooked.diagnostics);
            }
            if (result.status.Succeeded())
            {
                Progress(index, SceneBuildStage::Writing);
                std::error_code error;
                std::filesystem::create_directories(plan.output.parent_path(), error);
                if (error)
                {
                    result.status = {StatusCode::IOError, "Cannot create build output directory"};
                }
            }
            if (result.status.Succeeded())
            {
                SceneWriteOptions write;
                write.limits                    = m_options.limits;
                write.compression               = plan.job.compression;
                write.maxEncoderBytes           = m_options.maxEncoderBytes;
                write.durable                   = m_options.durable;
                write.metadata.sourceFormat     = imported.format;
                write.metadata.importerVersion  = imported.importerVersion;
                write.metadata.buildFingerprint = LibraryBuildFingerprint();
                write.metadata.profile          = plan.job.profile;
                write.metadata.settings         = plan.job.sourceSettings;
                write.metadata.processing       = plan.job.processing;
                write.metadata.cacheKey         = result.cacheKey;
                write.metadata.inputs           = inputs;
                write.metadata.diagnostics      = result.diagnostics;
                Publication publication{this, index, &inputs, &control};
                write.publicationUser = &publication;
                write.beforePublish   = &BeforePublish;
                write.published       = &result.published;
                result.status         = WriteScene(PathToUtf8(plan.output), *cooked.scene, write, &control);
                if (result.published)
                {
                    result.disposition = SceneBuildDisposition::Built;
                }
            }
        }
        Progress(index, SceneBuildStage::Complete);
    }
    void Worker()
    {
        uint32_t index = m_next.fetch_add(1);
        while (index < m_plans.size())
        {
            RunJob(index);
            index = m_next.fetch_add(1);
        }
    }
    void Execute()
    {
        std::vector<std::thread> workers;
        for (uint32_t index = 1; index < m_workers; ++index)
        {
            workers.emplace_back(&Batch::Worker, this);
        }
        Worker();
        for (std::thread& worker : workers)
        {
            worker.join();
        }
        for (const SceneBuildJobResult& job : m_result.jobs)
        {
            if (m_result.status.Succeeded() && !job.status.Succeeded())
            {
                m_result.status = job.status;
            }
        }
    }
    std::span<const SceneBuildJob> m_jobs;
    const SceneBuildOptions&       m_options;
    const Cancellation*            m_cancellation;
    SceneBuildResult               m_result;
    std::filesystem::path          m_root;
    std::vector<Plan>              m_plans;
    uint64_t                       m_planningBytes{0};
    uint32_t                       m_workers{0};
    std::atomic<uint32_t>          m_next{0};
};
} // namespace

namespace detail
{
Status ValidateBuildJob(const SceneBuildJob& job)
{
    Status status = ValidateProcessingSettings(job.processing);
    if (status.Succeeded()
        && (job.source.size() > 4096 || job.output.size() > 4096 || job.profile.size() > 64 || !IsValidUtf8(job.source)
            || !IsValidUtf8(job.output) || !IsValidUtf8(job.profile)))
    {
        status = {StatusCode::InvalidArgument, "Build job paths/profile exceed text bounds or are not UTF-8"};
    }
    if (status.Succeeded())
    {
        status = ValidateCompressionSettings(job.compression);
    }
    const AnimationBakeSettings& bake = job.sourceSettings.animationBake;
    if (status.Succeeded() && (job.profile != "reference" && job.profile != "desktop"))
    {
        status = {StatusCode::InvalidArgument, "Build profile must be reference or desktop"};
    }
    if (status.Succeeded() && job.profile == "reference" && HasProcessingOperations(job.processing))
    {
        status = {StatusCode::InvalidArgument, "Reference profile cannot enable processing operations"};
    }
    if (status.Succeeded()
        && (!std::isfinite(bake.sampleRate) || bake.sampleRate <= 0 || !std::isfinite(bake.translationTolerance)
            || bake.translationTolerance < 0 || !std::isfinite(bake.rotationTolerance) || bake.rotationTolerance < 0
            || !std::isfinite(bake.scaleTolerance) || bake.scaleTolerance < 0 || !std::isfinite(bake.valueTolerance)
            || bake.valueTolerance < 0 || bake.maxSubdivisions > 30 || bake.maxValues == 0))
    {
        status = {StatusCode::InvalidArgument, "Invalid animation bake settings"};
    }
    return status;
}
std::string BuildSettingsKey(const SceneBuildJob& job, std::string_view format, uint32_t importerVersion)
{
    AssetHash hash;
    HashText(hash, "ZenAsset batch identity 1");
    HashNumber(hash, PackageSchemaVersion);
    HashNumber(hash, ProcessorVersion);
    HashText(hash, LibraryVersion());
    HashText(hash, LibraryBuildFingerprint());
    HashText(hash, format);
    HashNumber(hash, importerVersion);
    HashText(hash, job.profile);
    HashNumber(hash, job.sourceSettings.sceneIndex);
    HashNumber(hash, job.sourceSettings.fallbackMissingNamedUVSets);
    const AnimationBakeSettings& bake = job.sourceSettings.animationBake;
    HashNumber(hash, std::bit_cast<uint64_t>(bake.sampleRate));
    HashNumber(hash, std::bit_cast<uint32_t>(bake.translationTolerance));
    HashNumber(hash, std::bit_cast<uint32_t>(bake.rotationTolerance));
    HashNumber(hash, std::bit_cast<uint32_t>(bake.scaleTolerance));
    HashNumber(hash, std::bit_cast<uint32_t>(bake.valueTolerance));
    HashNumber(hash, bake.maxSubdivisions);
    HashNumber(hash, bake.maxValues);
    HashNumber(hash, ProcessingSettingsHash(job.processing));
    HashNumber(hash, static_cast<uint32_t>(job.compression.transport));
    HashNumber(hash, static_cast<uint32_t>(job.compression.zstandardLevel));
    HashNumber(hash, job.compression.meshoptimizer);
    return DigestText(hash.Finish());
}
} // namespace detail
uint64_t SceneBuildReservationBytes(const SceneBuildOptions& options)
{
    uint64_t       bytes = 0;
    const uint64_t parts[]{options.importContext.maxSourceBytes,
                           options.importContext.maxDecodedImageBytes,
                           std::max(options.importContext.maxGeometryBytes, options.importContext.maxObjGeometryBytes),
                           options.limits.maxAllocationBytes,
                           options.limits.maxAllocationBytes,
                           HashScratch,
                           options.maxJobMetadataBytes,
                           options.maxJobMetadataBytes,
                           options.maxJobMetadataBytes};
    for (uint64_t part : parts)
    {
        bytes = part > UINT64_MAX - bytes ? UINT64_MAX : bytes + part;
    }
    return bytes;
}
SceneBuildResult BuildSceneBatch(std::span<const SceneBuildJob> jobs,
                                 const SceneBuildOptions&       options,
                                 const Cancellation*            cancellation)
{
    SceneBuildResult result;
    if (options.callbacks.progress != nullptr)
    {
        options.callbacks.progress(options.callbacks.user, UINT32_MAX, SceneBuildStage::Waiting);
    }
    bool acquired = false;
    {
        std::unique_lock<std::mutex> lock(batchMutex);
        while (batchActive && !Cancelled(cancellation))
        {
            batchChanged.wait_for(lock, std::chrono::milliseconds(25));
        }
        acquired = !Cancelled(cancellation);
        if (acquired)
        {
            batchActive = true;
        }
    }
    if (acquired)
    {
        Batch batch(jobs, options, cancellation);
        result = batch.Run();
        {
            std::lock_guard<std::mutex> lock(batchMutex);
            batchActive = false;
        }
        batchChanged.notify_all();
    }
    else
    {
        result.status = {StatusCode::Cancelled, "Batch cancelled while waiting for another build"};
    }
    return result;
}
} // namespace zenasset
