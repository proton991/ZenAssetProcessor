#include "Processor/BuildInternal.h"
#include "Processor/CookInternal.h"
#include "ZenAsset/Processor/Assembly.h"
#include "ZenAsset/Reader/Assembly.h"
#include "Importers/AssetHash.h"
#include "Importers/AssetPaths.h"
#include "Format/FileIO.h"
#include "Format/AssemblySchema.h"
#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>

namespace zenasset
{
namespace
{
std::timed_mutex assemblyBuildMutex;

bool SafeKey(const std::string& key)
{
    bool valid = !key.empty() && key.size() <= 128;
    for (unsigned char c : key)
    {
        valid = valid && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-');
    }
    return valid;
}

struct AssemblyJob
{
    SceneBuildJob                job;
    std::string                  settingsKey;
    std::vector<SceneDependency> inputs;
    std::filesystem::path        staged;
    std::filesystem::path        output;
};

class AssemblyBuilder
{
public:
    AssemblyBuilder(const SceneBuildManifest& manifest, const SceneBuildOptions& options, const Cancellation* cancellation) :
        m_manifest(manifest),
        m_options(options),
        m_cancellation(cancellation),
        m_control({const_cast<Cancellation*>(cancellation), &detail::PollImport, nullptr})
    {}

    SceneBuildResult Run()
    {
        m_result.sourceFreshnessChecked = !m_options.assemblyOnly;
        m_result.status                 = Initialize();
        for (uint32_t index = 0; m_result.status.Succeeded() && index < m_jobs.size(); ++index)
        {
            Progress(index, SceneBuildStage::Preflight);
            m_result.status             = Process(index);
            m_result.jobs[index].status = m_result.status;
            Progress(index, SceneBuildStage::Complete);
        }
        if (m_result.status.Succeeded())
        {
            if (!detail::AssemblyValidationFits(m_assembly, m_options.maxPlanningBytes - m_planningBytes))
            {
                m_result.status = {StatusCode::ResourceLimit, "Assembly validation exceeds remaining planning budget"};
            }
            else
            {
                m_result.status = FinalizeAssembly(m_assembly);
            }
        }
        if (m_result.status.Succeeded())
        {
            m_result.status = Publish();
        }
        // Only paths created in this build's exclusive staging directory are removed.
        for (const AssemblyJob& job : m_jobs)
        {
            if (!job.staged.empty())
            {
                std::error_code ignored;
                std::filesystem::remove(job.staged, ignored);
            }
        }
        if (!m_stage.empty())
        {
            std::error_code ignored;
            std::filesystem::remove(m_stage, ignored);
        }
        return std::move(m_result);
    }

private:
    void Progress(uint32_t index, SceneBuildStage stage)
    {
        if (m_options.callbacks.progress != nullptr)
        {
            m_options.callbacks.progress(m_options.callbacks.user, index, stage);
        }
    }

    Status Initialize()
    {
        std::filesystem::path assemblyName;
        Status                status      = detail::MakePath(m_manifest.assemblyOutput, assemblyName);
        const uint64_t        reservation = SceneBuildReservationBytes(m_options);
        if (m_manifest.version != 2 || m_manifest.jobs.empty() || m_manifest.jobs.size() != m_manifest.assembly.models.size()
            || m_manifest.jobs.size() > m_options.maxJobs || m_options.maxJobs > 65536 || m_options.maxPlanningBytes < 16384
            || m_options.maxJobMetadataBytes < 1024 || m_options.maxDiagnosticsPerJob > 4096 || m_options.workerCount == 0
            || m_options.workerCount > 64
            || (!m_options.assemblyOnly && (reservation == UINT64_MAX || reservation > m_options.maxReservedMemoryBytes)))
        {
            status = {StatusCode::ResourceLimit, "Assembly jobs or reservation exceed configured limits"};
        }
        if (status.Succeeded()
            && (assemblyName.has_parent_path() || assemblyName.extension() != ".zenscene"
                || !SafeKey(PathToUtf8(assemblyName.stem()))))
        {
            status = {StatusCode::InvalidArgument, "Assembly output must be a .zenscene basename with a simple key"};
        }
        if (status.Succeeded())
        {
            status = detail::Resolve(m_options.outputDirectory, m_root);
        }
        if (status.Succeeded())
        {
            status = detail::Resolve(PathToUtf8(m_root / assemblyName), m_output);
            if (status.Succeeded() && !detail::Within(m_output, m_root))
            {
                status = {StatusCode::InvalidArgument, "Assembly output escapes its directory"};
            }
        }
        uint64_t planned = 16384;
        for (size_t i = 0; status.Succeeded() && i < m_manifest.jobs.size(); ++i)
        {
            const SceneBuildJob& job = m_manifest.jobs[i];
            const std::string&   key = m_manifest.assembly.models[i].key;
            status                   = detail::ValidateBuildJob(job);
            if (status.Succeeded() && !SafeKey(key))
            {
                status = {StatusCode::InvalidArgument, "Model key must use ASCII letters, numbers, dash or underscore"};
            }
            for (size_t j = 0; status.Succeeded() && j < i; ++j)
            {
                if (key == m_manifest.assembly.models[j].key)
                {
                    status = {StatusCode::InvalidArgument, "Repeated assembly model key"};
                }
            }
            const uint64_t charge = sizeof(AssemblyJob) + sizeof(AssemblyModel) + sizeof(SceneBuildJobResult)
                                  + job.source.size() * 4 + job.output.size() + key.size() * 4 + 16384;
            if (status.Succeeded() && charge > m_options.maxPlanningBytes - planned)
            {
                status = {StatusCode::ResourceLimit, "Assembly planning budget exceeded"};
            }
            if (status.Succeeded())
            {
                planned += charge;
            }
        }
        for (const AssemblyInstance& instance : m_manifest.assembly.instances)
        {
            const uint64_t bytes = sizeof(AssemblyInstance) + instance.key.size() + instance.model.size();
            if (status.Succeeded() && bytes > m_options.maxPlanningBytes - planned)
            {
                status = {StatusCode::ResourceLimit, "Assembly instances exceed planning budget"};
            }
            if (status.Succeeded())
            {
                planned += bytes;
            }
        }
        if (status.Succeeded())
        {
            m_planningBytes = planned;
            m_assembly      = m_manifest.assembly;
            m_jobs.resize(m_manifest.jobs.size());
            m_result.jobs.resize(m_jobs.size());
            m_result.assemblyOutput        = PathToUtf8(m_output);
            SceneReadLimits previousLimits = m_options.limits;
            previousLimits.maxAllocationBytes =
                std::min(previousLimits.maxAllocationBytes, m_options.maxPlanningBytes - m_planningBytes);
            const AssemblyReadResult previous = ReadAssembly(m_result.assemblyOutput, previousLimits, m_cancellation);
            if (previous.status.Succeeded())
            {
                m_previous        = previous.assembly;
                uint64_t retained = sizeof(SceneAssembly) + m_previous->name.size() + m_previous->camera.instance.size()
                                  + m_previous->environment.instance.size()
                                  + m_previous->texCoordSets.size() * sizeof(uint32_t);
                for (const AssemblyModel& model : m_previous->models)
                {
                    retained += sizeof(AssemblyModel) + model.key.size() + model.path.size()
                              + model.validationFingerprint.size() + model.texCoordSets.size() * sizeof(uint32_t);
                }
                for (const AssemblyInstance& instance : m_previous->instances)
                {
                    retained += sizeof(AssemblyInstance) + instance.key.size() + instance.model.size();
                }
                if (retained > m_options.maxPlanningBytes - m_planningBytes)
                {
                    status = {StatusCode::ResourceLimit, "Previous assembly exceeds planning budget"};
                }
                else
                {
                    m_planningBytes += retained;
                }
            }
            else if (m_options.assemblyOnly)
            {
                status = previous.status;
            }
        }
        if (status.Succeeded() && !m_options.assemblyOnly)
        {
            std::error_code error;
            std::filesystem::create_directories(m_root, error);
            if (error)
            {
                status = {StatusCode::IOError, "Cannot create assembly output directory"};
            }
            if (status.Succeeded())
            {
                const uint64_t nonce = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
                const std::filesystem::path stage = m_root / (".assembly-stage-" + DigestText(nonce));
                if (!std::filesystem::create_directory(stage, error) || error)
                {
                    status = {StatusCode::IOError, "Cannot create exclusive assembly staging directory"};
                }
                else
                {
                    m_stage = stage;
                }
            }
        }
        return status;
    }

    const AssemblyModel* Previous(const std::string& key) const
    {
        const AssemblyModel* found = nullptr;
        if (m_previous != nullptr)
        {
            for (const AssemblyModel& model : m_previous->models)
            {
                if (model.key == key)
                {
                    found = &model;
                }
            }
        }
        return found;
    }

    Status Reuse(uint32_t index, const AssemblyModel& old, bool& reused)
    {
        AssemblyJob&                plan   = m_jobs[index];
        const std::filesystem::path path   = m_root / std::filesystem::u8path(old.path);
        PackageResult               opened = OpenPackage(PathToUtf8(path), m_options.limits.package, m_cancellation);
        Status                      status = opened.status;
        if (status.Succeeded() && opened.package->Identity() != old.identity)
        {
            status = {StatusCode::InvalidData, "Assembly model identity changed"};
        }
        SceneMetadataResult metadata;
        if (status.Succeeded())
        {
            metadata = ReadSceneMetadata(*opened.package, m_options.limits, m_cancellation);
            status   = metadata.status;
        }
        if (status.Succeeded() && !m_options.assemblyOnly)
        {
            SceneBuildJob recorded  = plan.job;
            recorded.sourceSettings = metadata.metadata.settings;
            recorded.processing     = metadata.metadata.processing;
            recorded.compression    = metadata.metadata.compression;
            recorded.profile        = metadata.metadata.profile;
            if (metadata.metadata.inputs.empty() || metadata.metadata.inputs.front().path != plan.job.source
                || metadata.metadata.processorVersion != ProcessorVersion
                || detail::BuildSettingsKey(recorded, metadata.metadata.sourceFormat, metadata.metadata.importerVersion)
                       != plan.settingsKey
                || metadata.metadata.cacheKey != detail::ContentKey(plan.settingsKey, metadata.metadata.inputs))
            {
                status = {StatusCode::SourceUnavailable, "Assembly model settings or source identity changed"};
            }
        }
        if (status.Succeeded() && detail::InputBytes(metadata.metadata.inputs) > m_options.maxJobMetadataBytes)
        {
            status = {StatusCode::ResourceLimit, "Assembly input metadata exceeds job budget"};
        }
        if (status.Succeeded() && !m_options.assemblyOnly)
        {
            status = opened.package->ValidatePayloads(m_cancellation);
        }
        AssemblyModel model = old;
        if (status.Succeeded() && !m_options.assemblyOnly && old.validationFingerprint != LibraryBuildFingerprint())
        {
            const SceneReadResult validated = ReadScene(*opened.package, m_options.limits, m_cancellation);
            status                          = validated.status;
            if (status.Succeeded())
            {
                status =
                    SummarizeAssemblyModel(*validated.scene, model, m_options.maxPlanningBytes - m_planningBytes, &m_control);
                model.validationFingerprint = LibraryBuildFingerprint();
            }
        }
        if (status.Succeeded() && !m_options.assemblyOnly)
        {
            status = detail::CheckInputs(metadata.metadata.inputs, false, false, m_control);
        }
        if (status.Succeeded())
        {
            if (!m_options.assemblyOnly)
            {
                plan.inputs = std::move(metadata.metadata.inputs);
            }
            plan.output                      = path;
            m_assembly.models[index]         = std::move(model);
            m_result.jobs[index].cacheKey    = metadata.metadata.cacheKey;
            m_result.jobs[index].output      = PathToUtf8(path);
            m_result.jobs[index].disposition = SceneBuildDisposition::Reused;
            reused                           = true;
            Progress(index, SceneBuildStage::Reused);
        }
        return status;
    }

    void Diagnostics(uint32_t index, const std::vector<Diagnostic>& diagnostics)
    {
        SceneBuildJobResult& result = m_result.jobs[index];
        uint64_t             used   = 0;
        for (const Diagnostic& value : result.diagnostics)
        {
            used += sizeof(Diagnostic) + value.message.size() + value.source.size() + value.object.size();
        }
        for (const Diagnostic& diagnostic : diagnostics)
        {
            const uint64_t bytes =
                sizeof(Diagnostic) + diagnostic.message.size() + diagnostic.source.size() + diagnostic.object.size();
            if (result.diagnostics.size() < m_options.maxDiagnosticsPerJob && used <= m_options.maxJobMetadataBytes
                && bytes <= m_options.maxJobMetadataBytes - used)
            {
                result.diagnostics.push_back(diagnostic);
                used += bytes;
            }
            else if (result.diagnosticsOmitted != UINT32_MAX)
            {
                ++result.diagnosticsOmitted;
            }
        }
    }

    Status Cook(uint32_t index)
    {
        AssemblyJob&         plan   = m_jobs[index];
        SceneBuildJobResult& result = m_result.jobs[index];
        AssetFileStamp       stamp;
        Status               status;
        if (!StampAssetFile(plan.job.source, stamp))
        {
            status = {StatusCode::SourceUnavailable, "Cannot inspect assembly source"};
        }
        SceneImportResult imported;
        if (status.Succeeded())
        {
            Progress(index, SceneBuildStage::Importing);
            SceneImportRequest request;
            request.path     = plan.job.source;
            request.settings = plan.job.sourceSettings;
            request.control =
                std::make_shared<SceneImportControl>(SceneImportCallbacks{&m_control, &detail::PollImport, nullptr});
            SceneImportContext context = m_options.importContext;
            context.workerCount        = std::max(1u, context.workerCount);
            imported                   = ImportScene(request, context);
            status                     = {imported.status, imported.error};
            Diagnostics(index, imported.diagnostics);
        }
        if (status.Succeeded())
        {
            if (detail::InputBytes(imported.asset->dependencies) + sizeof(SceneDependency) + plan.job.source.size()
                > m_options.maxJobMetadataBytes)
            {
                status = {StatusCode::ResourceLimit, "Assembly input metadata exceeds job budget"};
            }
            else
            {
                plan.inputs.push_back({plan.job.source, true, stamp.size, stamp.modified, 0});
                plan.inputs.insert(plan.inputs.end(), imported.asset->dependencies.begin(), imported.asset->dependencies.end());
                status = detail::CheckInputs(plan.inputs, true, true, m_control);
            }
        }
        SceneCookResult cooked;
        if (status.Succeeded())
        {
            Progress(index, SceneBuildStage::Cooking);
            cooked = detail::ConsumeCookScene(std::move(imported.asset), {plan.job.processing, m_options.limits}, &m_control);
            status = cooked.status;
            Diagnostics(index, cooked.diagnostics);
        }
        if (status.Succeeded())
        {
            status = SummarizeAssemblyModel(*cooked.scene, m_assembly.models[index],
                                            m_options.maxPlanningBytes - m_planningBytes, &m_control);
        }
        if (status.Succeeded())
        {
            result.cacheKey = detail::ContentKey(plan.settingsKey, plan.inputs);
            plan.staged     = m_stage / (std::to_string(index) + ".zenasset");
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
            write.metadata.inputs           = plan.inputs;
            write.metadata.diagnostics      = result.diagnostics;
            Progress(index, SceneBuildStage::Writing);
            status = WriteScene(PathToUtf8(plan.staged), *cooked.scene, write, &m_control);
        }
        cooked.scene.reset();
        if (status.Succeeded())
        {
            // Establish the receipt from a semantic read of the exact serialized output.
            // The cooked owner is released first so validation does not retain two models.
            const SceneReadResult validated = ReadScene(PathToUtf8(plan.staged), m_options.limits, &m_control);
            status                          = validated.status;
        }
        if (status.Succeeded())
        {
            const PackageResult package = OpenPackage(PathToUtf8(plan.staged), m_options.limits.package, &m_control);
            status                      = package.status;
            if (status.Succeeded())
            {
                AssemblyModel& model        = m_assembly.models[index];
                model.identity              = package.package->Identity();
                model.validationFingerprint = LibraryBuildFingerprint();
                model.path =
                    "models/" + model.key + "-" + result.cacheKey + "-" + DigestText(model.identity.headerHash) + ".zenasset";
                status = detail::Resolve(PathToUtf8(m_root / std::filesystem::u8path(model.path)), plan.output);
                std::error_code error;
                if (status.Succeeded() && std::filesystem::exists(plan.output, error))
                {
                    const PackageResult existing = OpenPackage(PathToUtf8(plan.output), m_options.limits.package, &m_control);
                    if (!existing.status.Succeeded() || existing.package->Identity() != model.identity
                        || !existing.package->ValidatePayloads(&m_control).Succeeded())
                    {
                        const uint64_t nonce =
                            static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
                        model.path = "models/" + model.key + "-" + result.cacheKey + "-" + DigestText(model.identity.headerHash)
                                   + "-" + DigestText(nonce) + ".zenasset";
                        status     = detail::Resolve(PathToUtf8(m_root / std::filesystem::u8path(model.path)), plan.output);
                    }
                }
                result.output      = PathToUtf8(plan.output);
                result.disposition = SceneBuildDisposition::Built;
            }
        }
        return status;
    }

    Status Process(uint32_t index)
    {
        AssemblyJob& plan = m_jobs[index];
        plan.job          = m_manifest.jobs[index];
        Status status;
        if (m_control.IsCancelled())
        {
            status = {StatusCode::Cancelled, "Assembly build cancelled"};
        }
        if (status.Succeeded() && !m_options.assemblyOnly)
        {
            std::filesystem::path source;
            status          = detail::Resolve(plan.job.source, source);
            plan.job.source = PathToUtf8(source);
            if (status.Succeeded() && (detail::Alias(source, m_output) || detail::Within(source, m_stage)))
            {
                status = {StatusCode::InvalidArgument, "Assembly output/staging aliases a source"};
            }
            const SceneFormatDescriptor* format = FindSceneImportFormat(plan.job.source);
            if (status.Succeeded() && format == nullptr)
            {
                status = {StatusCode::UnsupportedFormat, "Unsupported assembly source format"};
            }
            if (status.Succeeded())
            {
                plan.settingsKey = detail::BuildSettingsKey(plan.job, format->id, format->importerVersion);
            }
        }
        bool                 reused   = false;
        const AssemblyModel* previous = Previous(m_assembly.models[index].key);
        if (status.Succeeded() && previous != nullptr && (!m_options.force || m_options.assemblyOnly))
        {
            const Status reuse = Reuse(index, *previous, reused);
            if (m_options.assemblyOnly || reuse.code == StatusCode::Cancelled)
            {
                status = reuse;
            }
        }
        if (status.Succeeded() && !reused)
        {
            if (m_options.assemblyOnly)
            {
                status = {StatusCode::SourceUnavailable, "Assembly-only requires every model in the previous assembly"};
            }
            else
            {
                status = Cook(index);
            }
        }
        if (status.Succeeded())
        {
            const uint64_t bytes = detail::InputBytes(plan.inputs) + m_options.maxJobMetadataBytes;
            if (bytes > m_options.maxPlanningBytes - m_planningBytes)
            {
                status = {StatusCode::ResourceLimit, "Assembly dependency planning budget exceeded"};
            }
            else
            {
                m_planningBytes += bytes;
            }
        }
        return status;
    }

    Status CheckPublication(bool hashInputs)
    {
        Status                status;
        std::filesystem::path root;
        status = detail::Resolve(m_options.outputDirectory, root);
        if (status.Succeeded() && root != m_root)
        {
            status = {StatusCode::InvalidArgument, "Assembly output root resolution changed"};
        }
        for (AssemblyJob& job : m_jobs)
        {
            if (status.Succeeded() && !m_options.assemblyOnly && hashInputs)
            {
                status = detail::CheckInputs(job.inputs, false, false, m_control);
            }
            for (const SceneDependency& input : job.inputs)
            {
                std::filesystem::path path;
                if (status.Succeeded())
                {
                    status = detail::Resolve(input.path, path);
                }
                if (status.Succeeded() && detail::Alias(path, m_output))
                {
                    status = {StatusCode::InvalidArgument, "Assembly output aliases an input dependency"};
                }
                for (const AssemblyJob& output : m_jobs)
                {
                    if (status.Succeeded() && detail::Alias(path, output.output))
                    {
                        status = {StatusCode::InvalidArgument, "Model output aliases an input dependency"};
                    }
                }
            }
        }
        if (status.Succeeded())
        {
            status = VerifyAssemblyDependencies(PathToUtf8(m_output), m_assembly, m_options.limits.package, m_cancellation);
        }
        if (status.Succeeded() && m_control.IsCancelled())
        {
            status = {StatusCode::Cancelled, "Assembly publication cancelled"};
        }
        return status;
    }

    static Status BeforePublish(void* user)
    {
        AssemblyBuilder& builder = *static_cast<AssemblyBuilder*>(user);
        builder.Progress(UINT32_MAX, SceneBuildStage::Publishing);
        return builder.CheckPublication(true);
    }

    Status Publish()
    {
        Status status;
        // Discover every dependency before exposing any new model package.
        for (const AssemblyJob& inputJob : m_jobs)
        {
            for (const SceneDependency& input : inputJob.inputs)
            {
                std::filesystem::path path;
                if (status.Succeeded())
                {
                    status = detail::Resolve(input.path, path);
                }
                if (status.Succeeded() && detail::Alias(path, m_output))
                {
                    status = {StatusCode::InvalidArgument, "Assembly aliases an input"};
                }
                for (const AssemblyJob& outputJob : m_jobs)
                {
                    if (status.Succeeded() && detail::Alias(path, outputJob.output))
                    {
                        status = {StatusCode::InvalidArgument, "Model output aliases an input"};
                    }
                }
            }
        }
        for (size_t index = 0; status.Succeeded() && index < m_jobs.size(); ++index)
        {
            AssemblyJob& job = m_jobs[index];
            if (!job.staged.empty())
            {
                std::filesystem::path resolved;
                status = detail::Resolve(PathToUtf8(m_root / std::filesystem::u8path(m_assembly.models[index].path)), resolved);
                if (status.Succeeded() && (resolved != job.output || !detail::Within(resolved, m_root)))
                {
                    status = {StatusCode::InvalidArgument, "Model output resolution changed or escapes root"};
                }
                std::error_code error;
                if (status.Succeeded())
                {
                    std::filesystem::create_directories(job.output.parent_path(), error);
                    if (error)
                    {
                        status = {StatusCode::IOError, "Cannot create model output directory"};
                    }
                }
                if (status.Succeeded() && std::filesystem::exists(job.output, error))
                {
                    const PackageResult existing =
                        OpenPackage(PathToUtf8(job.output), m_options.limits.package, m_cancellation);
                    status = existing.status;
                    if (status.Succeeded() && existing.package->Identity() != m_assembly.models[index].identity)
                    {
                        status = {StatusCode::InvalidData, "Immutable model path has different content"};
                    }
                    if (status.Succeeded())
                    {
                        status = existing.package->ValidatePayloads(m_cancellation);
                    }
                }
                else if (status.Succeeded())
                {
                    // rename never replaces a destination on Windows; cross-process builders require external coordination.
                    std::filesystem::rename(job.staged, job.output, error);
                    if (error)
                    {
                        status = {StatusCode::IOError, "Cannot publish immutable model package"};
                    }
                }
            }
        }
        if (status.Succeeded() && m_previous != nullptr && *m_previous == m_assembly)
        {
            status = CheckPublication(false);
        }
        else if (status.Succeeded())
        {
            SceneWriteOptions write;
            write.limits          = m_options.limits;
            write.durable         = m_options.durable;
            write.publicationUser = this;
            write.beforePublish   = &BeforePublish;
            write.published       = &m_result.assemblyPublished;
            status                = WriteAssembly(PathToUtf8(m_output), m_assembly, write, m_cancellation);
        }
        if (status.Succeeded() || m_result.assemblyPublished)
        {
            for (SceneBuildJobResult& job : m_result.jobs)
            {
                job.published = job.disposition == SceneBuildDisposition::Built;
            }
        }
        return status;
    }

    const SceneBuildManifest&            m_manifest;
    const SceneBuildOptions&             m_options;
    const Cancellation*                  m_cancellation;
    SceneImportControl                   m_control;
    SceneBuildResult                     m_result;
    SceneAssembly                        m_assembly;
    std::shared_ptr<const SceneAssembly> m_previous;
    std::vector<AssemblyJob>             m_jobs;
    std::filesystem::path                m_root, m_output, m_stage;
    uint64_t                             m_planningBytes{0};
};
} // namespace

namespace detail
{
std::timed_mutex& AssemblyBuildGate()
{
    return assemblyBuildMutex;
}
} // namespace detail

SceneBuildResult BuildSceneAssembly(const SceneBuildManifest& manifest,
                                    const SceneBuildOptions&  options,
                                    const Cancellation*       cancellation)
{
    SceneBuildResult result;
    bool             acquired = false;
    while (!acquired && !detail::Cancelled(cancellation))
    {
        acquired = assemblyBuildMutex.try_lock_for(std::chrono::milliseconds(25));
    }
    if (acquired)
    {
        AssemblyBuilder builder(manifest, options, cancellation);
        result = builder.Run();
        assemblyBuildMutex.unlock();
    }
    else
    {
        result.status = {StatusCode::Cancelled, "Assembly cancelled waiting for another build"};
    }
    return result;
}
} // namespace zenasset
