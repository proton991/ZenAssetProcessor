#include "Processor/BuildInternal.h"
#include "Format/FileIO.h"
#include "Importers/AssetPaths.h"
#include "ZenAsset/Format/Utf8.h"
#include <simdjson.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace zenasset
{
namespace
{
enum class Kind
{
    Text,
    Boolean,
    U32,
    U64,
    Float,
    Double,
    Source,
    Bake,
    Processing,
    Geometry,
    Textures,
    Compression,
    Transport,
    Matrix
};
struct Field
{
    std::string_view name;
    Kind             kind;
    void*            value;
};
class ManifestReader
{
public:
    ManifestReader(const SceneBuildOptions& limits, const Cancellation* cancellation) :
        m_limits(limits), m_cancellation(cancellation)
    {}
    Status Read(simdjson::dom::element document, SceneBuildManifest& manifest)
    {
        simdjson::dom::object object;
        if (document.get_object().get(object) != simdjson::SUCCESS)
        {
            Fail("Manifest must be an object");
        }
        uint64_t version = 0;
        if (m_status.Succeeded()
            && (document["version"].get_uint64().get(version) != simdjson::SUCCESS || (version != 1 && version != 2)))
        {
            Fail("Manifest version must be integer 1 or 2");
        }
        manifest.version     = static_cast<uint32_t>(version);
        bool assemblySeen    = false;
        bool instancesSeen   = false;
        bool nameSeen        = false;
        bool cameraSeen      = false;
        bool environmentSeen = false;
        bool versionSeen     = false;
        bool directorySeen   = false;
        bool jobsSeen        = false;
        if (m_status.Succeeded())
        {
            for (simdjson::dom::key_value_pair entry : object)
            {
                Poll();
                if (!m_status.Succeeded())
                {
                    break;
                }
                if (entry.key == "version" && !versionSeen)
                {
                    versionSeen = true;
                }
                else if (entry.key == "outputDirectory" && !directorySeen)
                {
                    directorySeen = true;
                    Value(entry.value, {entry.key, Kind::Text, &manifest.outputDirectory});
                }
                else if (entry.key == "jobs" && !jobsSeen)
                {
                    jobsSeen = true;
                    Jobs(entry.value, manifest.jobs, manifest.version == 2 ? &manifest.assembly.models : nullptr);
                }
                else if (manifest.version == 2 && entry.key == "assembly" && !assemblySeen)
                {
                    assemblySeen = true;
                    Value(entry.value, {entry.key, Kind::Text, &manifest.assemblyOutput});
                }
                else if (manifest.version == 2 && entry.key == "name" && !nameSeen)
                {
                    nameSeen = true;
                    Value(entry.value, {entry.key, Kind::Text, &manifest.assembly.name});
                }
                else if (manifest.version == 2 && entry.key == "instances" && !instancesSeen)
                {
                    instancesSeen = true;
                    Instances(entry.value, manifest.assembly.instances);
                }
                else if (manifest.version == 2 && entry.key == "camera" && !cameraSeen)
                {
                    cameraSeen = true;
                    Selection(entry.value, manifest.assembly.camera);
                }
                else if (manifest.version == 2 && entry.key == "environment" && !environmentSeen)
                {
                    environmentSeen = true;
                    Selection(entry.value, manifest.assembly.environment);
                }
                else
                {
                    Fail("Unknown or repeated manifest field: " + std::string(entry.key));
                }
            }
        }
        if (!versionSeen || !directorySeen || !jobsSeen || manifest.jobs.empty())
        {
            Fail("Manifest requires version, outputDirectory and nonempty jobs");
        }
        if (manifest.version == 2 && (!assemblySeen || !instancesSeen || manifest.assembly.instances.empty()))
        {
            Fail("Assembly manifest requires assembly and nonempty instances");
        }
        return m_status;
    }

private:
    void Poll()
    {
        if (m_status.Succeeded() && m_cancellation != nullptr && m_cancellation->IsCancelled())
        {
            m_status = {StatusCode::Cancelled, "Manifest parsing cancelled"};
        }
    }
    void Fail(std::string text)
    {
        if (m_status.Succeeded())
        {
            m_status = {StatusCode::InvalidArgument, std::move(text)};
        }
    }
    void Object(simdjson::dom::element element, std::span<const Field> fields)
    {
        simdjson::dom::object object;
        if (element.get_object().get(object) != simdjson::SUCCESS)
        {
            Fail("Settings must be JSON objects");
        }
        std::vector<bool> seen(fields.size(), false);
        if (m_status.Succeeded())
        {
            for (simdjson::dom::key_value_pair entry : object)
            {
                Poll();
                if (!m_status.Succeeded())
                {
                    break;
                }
                size_t index = 0;
                while (index < fields.size() && fields[index].name != entry.key)
                {
                    ++index;
                }
                if (index == fields.size() || seen[index])
                {
                    Fail("Unknown or repeated setting: " + std::string(entry.key));
                }
                else if (m_status.Succeeded())
                {
                    seen[index] = true;
                    Value(entry.value, fields[index]);
                }
            }
        }
    }
    void Value(simdjson::dom::element value, const Field& field)
    {
        uint64_t         integer = 0;
        double           number  = 0;
        bool             boolean = false;
        std::string_view text;
        switch (field.kind)
        {
            case Kind::Text:
            case Kind::Transport:
                if (value.get_string().get(text) != simdjson::SUCCESS || text.empty() || text.size() > 4096
                    || text.find('\0') != std::string_view::npos || !IsValidUtf8(text))
                {
                    Fail("Expected nonempty UTF-8 text: " + std::string(field.name));
                }
                else if (field.kind == Kind::Transport)
                {
                    if (text != "none" && text != "zstd")
                    {
                        Fail("Compression transport must be none or zstd");
                    }
                    else
                    {
                        *static_cast<TransportCodec*>(field.value) =
                            text == "zstd" ? TransportCodec::Zstandard : TransportCodec::None;
                    }
                }
                else
                {
                    *static_cast<std::string*>(field.value) = text;
                }
                break;
            case Kind::Boolean:
                if (value.get_bool().get(boolean) != simdjson::SUCCESS)
                {
                    Fail("Expected boolean: " + std::string(field.name));
                }
                else
                {
                    *static_cast<bool*>(field.value) = boolean;
                }
                break;
            case Kind::U32:
            case Kind::U64:
                if (value.get_uint64().get(integer) != simdjson::SUCCESS || (field.kind == Kind::U32 && integer > UINT32_MAX))
                {
                    Fail("Expected bounded unsigned integer: " + std::string(field.name));
                }
                else if (field.kind == Kind::U32)
                {
                    *static_cast<uint32_t*>(field.value) = static_cast<uint32_t>(integer);
                }
                else
                {
                    *static_cast<uint64_t*>(field.value) = integer;
                }
                break;
            case Kind::Float:
            case Kind::Double:
                if (value.get_double().get(number) != simdjson::SUCCESS || !std::isfinite(number)
                    || (field.kind == Kind::Float && std::abs(number) > std::numeric_limits<float>::max()))
                {
                    Fail("Expected finite number: " + std::string(field.name));
                }
                else if (field.kind == Kind::Float)
                {
                    *static_cast<float*>(field.value) = static_cast<float>(number);
                }
                else
                {
                    *static_cast<double*>(field.value) = number;
                }
                break;
            case Kind::Matrix: Matrix(value, *static_cast<Mat4*>(field.value)); break;
            case Kind::Source: Source(value, *static_cast<SourceSettings*>(field.value)); break;
            case Kind::Bake: Bake(value, *static_cast<AnimationBakeSettings*>(field.value)); break;
            case Kind::Processing: Processing(value, *static_cast<ProcessingSettings*>(field.value)); break;
            case Kind::Geometry: Geometry(value, *static_cast<GeometrySettings*>(field.value)); break;
            case Kind::Textures: Textures(value, *static_cast<TextureSettings*>(field.value)); break;
            case Kind::Compression: Compression(value, *static_cast<CompressionSettings*>(field.value)); break;
        }
    }
    void Source(simdjson::dom::element value, SourceSettings& settings)
    {
        const Field fields[]{{"sceneIndex", Kind::U32, &settings.sceneIndex},
                             {"fallbackMissingNamedUVSets", Kind::Boolean, &settings.fallbackMissingNamedUVSets},
                             {"animationBake", Kind::Bake, &settings.animationBake}};
        Object(value, fields);
    }
    void Bake(simdjson::dom::element value, AnimationBakeSettings& settings)
    {
        const Field fields[]{{"sampleRate", Kind::Double, &settings.sampleRate},
                             {"translationTolerance", Kind::Float, &settings.translationTolerance},
                             {"rotationTolerance", Kind::Float, &settings.rotationTolerance},
                             {"scaleTolerance", Kind::Float, &settings.scaleTolerance},
                             {"valueTolerance", Kind::Float, &settings.valueTolerance},
                             {"maxSubdivisions", Kind::U32, &settings.maxSubdivisions},
                             {"maxValues", Kind::U64, &settings.maxValues}};
        Object(value, fields);
    }
    void Processing(simdjson::dom::element value, ProcessingSettings& settings)
    {
        const Field fields[]{{"geometry", Kind::Geometry, &settings.geometry},
                             {"textures", Kind::Textures, &settings.textures}};
        Object(value, fields);
    }
    void Geometry(simdjson::dom::element value, GeometrySettings& settings)
    {
        const Field fields[]{{"optimizeVertexFetch", Kind::Boolean, &settings.optimizeVertexFetch},
                             {"positionQuantizationBits", Kind::U32, &settings.positionQuantizationBits},
                             {"positionMaxRelativeError", Kind::Float, &settings.positionMaxRelativeError},
                             {"positionMaxAbsoluteError", Kind::Float, &settings.positionMaxAbsoluteError}};
        Object(value, fields);
    }
    void Textures(simdjson::dom::element value, TextureSettings& settings)
    {
        const Field fields[]{{"generateMipmaps", Kind::Boolean, &settings.generateMipmaps},
                             {"compressBC7", Kind::Boolean, &settings.compressBC7},
                             {"bc7Quality", Kind::U32, &settings.bc7Quality},
                             {"maxDimension", Kind::U32, &settings.maxDimension},
                             {"colorMinPsnr", Kind::Float, &settings.colorMinPsnr},
                             {"colorMaxRmse", Kind::Float, &settings.colorMaxRmse},
                             {"scalarMaxError", Kind::Float, &settings.scalarMaxError},
                             {"normalRmsAngleDegrees", Kind::Float, &settings.normalRmsAngleDegrees},
                             {"normalMaxAngleDegrees", Kind::Float, &settings.normalMaxAngleDegrees},
                             {"alphaCoverageMaxError", Kind::Float, &settings.alphaCoverageMaxError},
                             {"compressBC5Normals", Kind::Boolean, &settings.compressBC5Normals},
                             {"compressBC4Occlusion", Kind::Boolean, &settings.compressBC4Occlusion},
                             {"compressBC6HEnvironment", Kind::Boolean, &settings.compressBC6HEnvironment},
                             {"hdrMaxRelativeRmse", Kind::Float, &settings.hdrMaxRelativeRmse},
                             {"arrayPages", Kind::Boolean, &settings.arrayPages},
                             {"arrayMaxDimension", Kind::U32, &settings.arrayMaxDimension},
                             {"arrayMaxLayers", Kind::U32, &settings.arrayMaxLayers},
                             {"arrayMaxPages", Kind::U32, &settings.arrayMaxPages},
                             {"arrayMaxImages", Kind::U32, &settings.arrayMaxImages},
                             {"spatialPages", Kind::Boolean, &settings.spatialPages},
                             {"spatialMaxDimension", Kind::U32, &settings.spatialMaxDimension},
                             {"spatialMaxPages", Kind::U32, &settings.spatialMaxPages},
                             {"spatialMaxImages", Kind::U32, &settings.spatialMaxImages},
                             {"collectAlphaStatistics", Kind::Boolean, &settings.collectAlphaStatistics}};
        Object(value, fields);
    }
    void Compression(simdjson::dom::element value, CompressionSettings& settings)
    {
        uint32_t    level = static_cast<uint32_t>(settings.zstandardLevel);
        const Field fields[]{{"transport", Kind::Transport, &settings.transport},
                             {"zstandardLevel", Kind::U32, &level},
                             {"meshoptimizer", Kind::Boolean, &settings.meshoptimizer}};
        Object(value, fields);
        if (level < 1 || level > 19)
        {
            Fail("Zstandard level must be 1..19");
        }
        else
        {
            settings.zstandardLevel = static_cast<int32_t>(level);
        }
    }
    void Matrix(simdjson::dom::element value, Mat4& matrix)
    {
        simdjson::dom::array array;
        if (value.get_array().get(array) != simdjson::SUCCESS || array.size() != 16)
        {
            Fail("Transform must have 16 column-major numbers");
        }
        size_t index = 0;
        if (m_status.Succeeded())
        {
            for (simdjson::dom::element item : array)
            {
                Value(item, {"transform", Kind::Float, &matrix[index / 4][index % 4]});
                ++index;
            }
        }
    }
    void Selection(simdjson::dom::element value, AssemblySelection& selection)
    {
        const Field fields[]{{"instance", Kind::Text, &selection.instance}, {"resource", Kind::U32, &selection.resource}};
        Object(value, fields);
        if (selection.instance.empty() || selection.resource == UINT32_MAX)
        {
            Fail("Selection requires instance and resource");
        }
    }
    void Instances(simdjson::dom::element value, std::vector<AssemblyInstance>& instances)
    {
        simdjson::dom::array array;
        if (value.get_array().get(array) != simdjson::SUCCESS || array.size() > 65536
            || array.size() > m_limits.maxPlanningBytes / sizeof(AssemblyInstance))
        {
            Fail("Instances exceed the planning budget");
        }
        if (m_status.Succeeded())
        {
            for (simdjson::dom::element item : array)
            {
                Poll();
                AssemblyInstance instance;
                const Field      fields[]{{"key", Kind::Text, &instance.key},
                                          {"model", Kind::Text, &instance.model},
                                          {"transform", Kind::Matrix, &instance.transform},
                                          {"submitLights", Kind::Boolean, &instance.submitLights}};
                Object(item, fields);
                const uint64_t bytes = sizeof(AssemblyInstance) + instance.key.size() + instance.model.size();
                if (bytes > m_limits.maxPlanningBytes - m_jobBytes)
                {
                    Fail("Instances exceed the planning budget");
                }
                if (instance.key.empty() || instance.model.empty())
                {
                    Fail("Instances require key and model");
                }
                if (!m_status.Succeeded())
                {
                    break;
                }
                m_jobBytes += bytes;
                instances.push_back(std::move(instance));
            }
        }
    }
    void Jobs(simdjson::dom::element value, std::vector<SceneBuildJob>& jobs, std::vector<AssemblyModel>* models)
    {
        simdjson::dom::array array;
        if (value.get_array().get(array) != simdjson::SUCCESS || array.size() > m_limits.maxJobs)
        {
            Fail("Jobs must be a bounded JSON array");
        }
        if (m_status.Succeeded())
        {
            for (simdjson::dom::element item : array)
            {
                Poll();
                if (!m_status.Succeeded())
                {
                    break;
                }
                if (m_status.Succeeded())
                {
                    SceneBuildJob job;
                    AssemblyModel model;
                    const Field   fields[]{
                        {"source", Kind::Text, &job.source},
                        {models == nullptr ? "output" : "key", Kind::Text, models == nullptr ? &job.output : &model.key},
                        {"profile", Kind::Text, &job.profile},
                        {"sourceSettings", Kind::Source, &job.sourceSettings},
                        {"processing", Kind::Processing, &job.processing},
                        {"compression", Kind::Compression, &job.compression}};
                    Object(item, fields);
                    if (models != nullptr && !model.key.empty())
                    {
                        job.output = model.key + ".zenasset";
                    }
                    if (job.source.empty() || job.output.empty())
                    {
                        Fail("Every job requires source and output");
                    }
                    if (m_status.Succeeded())
                    {
                        m_status = detail::ValidateBuildJob(job);
                    }
                    if (m_status.Succeeded())
                    {
                        const uint64_t bytes = sizeof(SceneBuildJob) + job.source.size() + job.output.size()
                                             + job.profile.size()
                                             + (models == nullptr ? 0 : sizeof(AssemblyModel) + model.key.size());
                        if (bytes > m_limits.maxPlanningBytes - m_jobBytes)
                        {
                            m_status = {StatusCode::ResourceLimit, "Manifest jobs exceed the planning budget"};
                        }
                        else
                        {
                            m_jobBytes += bytes;
                            jobs.push_back(std::move(job));
                            if (models != nullptr)
                            {
                                models->push_back(std::move(model));
                            }
                        }
                    }
                }
            }
        }
    }
    const SceneBuildOptions& m_limits;
    const Cancellation*      m_cancellation;
    Status                   m_status;
    uint64_t                 m_jobBytes{0};
};
} // namespace
SceneBuildManifest ReadSceneBuildManifest(const std::string&       path,
                                          const SceneBuildOptions& limits,
                                          const Cancellation*      cancellation)
{
    SceneBuildManifest    manifest;
    std::filesystem::path source;
    manifest.status = detail::MakePath(path, source);
    detail::FileOpenResult opened;
    uint64_t               bytes = 0;
    if (manifest.status.Succeeded())
    {
        opened          = detail::OpenFile(source);
        manifest.status = opened.status;
    }
    std::vector<char> text;
    if (manifest.status.Succeeded())
    {
        if (!detail::FileLength(opened.file, bytes) || bytes > limits.maxManifestBytes
            || bytes > SIZE_MAX - simdjson::SIMDJSON_PADDING || limits.maxManifestBytes > (64ull << 20)
            || limits.maxJobs > 65536)
        {
            manifest.status = {StatusCode::ResourceLimit, "Manifest exceeds the configured byte/job limit"};
        }
        else
        {
            text.resize(static_cast<size_t>(bytes) + simdjson::SIMDJSON_PADDING);
            manifest.status = detail::ReadFileBytes(opened.file, text.data(), bytes, cancellation);
        }
    }
    if (opened.file != nullptr)
    {
        std::fclose(opened.file);
    }
    if (manifest.status.Succeeded())
    {
        simdjson::dom::parser      parser;
        simdjson::dom::element     document;
        const simdjson::error_code error = parser.parse(text.data(), static_cast<size_t>(bytes), false).get(document);
        if (error != simdjson::SUCCESS)
        {
            manifest.status = {StatusCode::InvalidArgument,
                               "Invalid manifest JSON: " + std::string(simdjson::error_message(error))};
        }
        else
        {
            ManifestReader reader(limits, cancellation);
            manifest.status = reader.Read(document, manifest);
        }
    }
    if (manifest.status.Succeeded())
    {
        std::error_code             error;
        const std::filesystem::path parent = std::filesystem::absolute(source, error).parent_path();
        if (error)
        {
            manifest.status = {StatusCode::IOError, "Cannot resolve manifest directory"};
        }
        else
        {
            manifest.outputDirectory =
                PathToUtf8((parent / std::filesystem::u8path(manifest.outputDirectory)).lexically_normal());
            for (SceneBuildJob& job : manifest.jobs)
            {
                if (manifest.status.Succeeded() && cancellation != nullptr && cancellation->IsCancelled())
                {
                    manifest.status = {StatusCode::Cancelled, "Manifest path resolution cancelled"};
                }
                if (manifest.status.Succeeded())
                {
                    job.source = PathToUtf8((parent / std::filesystem::u8path(job.source)).lexically_normal());
                }
            }
        }
    }
    if (!manifest.status.Succeeded())
    {
        manifest.jobs.clear();
    }
    return manifest;
}
} // namespace zenasset
