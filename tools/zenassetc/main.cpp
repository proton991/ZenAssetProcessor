#include "ZenAsset/Format/Utf8.h"
#include "ZenAsset/Format/Version.h"
#include "ZenAsset/Processor/Import.h"
#include "ZenAsset/Processor/Cook.h"
#include "ZenAsset/Processor/Build.h"
#include "ZenAsset/Processor/Write.h"
#include "ZenAsset/Reader/Scene.h"
#include <atomic>
#include <array>
#include <algorithm>
#include <charconv>
#include <csignal>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#if defined(_WIN32)
#    define NOMINMAX
#    include <Windows.h>
#endif

namespace
{
using namespace zenasset;
static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> interrupted{false};

void Interrupt(int)
{
    interrupted.store(true, std::memory_order_relaxed);
}

bool Interrupted(void*)
{
    return interrupted.load(std::memory_order_relaxed);
}

void JsonString(std::ostream& stream, std::string_view value)
{
    constexpr char hex[] = "0123456789abcdef";
    stream << '"';
    for (unsigned char character : value)
    {
        if (character == '"' || character == '\\')
        {
            stream << '\\' << character;
        }
        else if (character < 0x20)
        {
            stream << "\\u00" << hex[character >> 4] << hex[character & 15];
        }
        else
        {
            stream << character;
        }
    }
    stream << '"';
}

void PrintFailure(const Status& status)
{
    std::cerr << "{\"status\":" << static_cast<uint32_t>(status.code) << ",\"message\":";
    JsonString(std::cerr, status.message);
    std::cerr << "}\n";
}

struct Arguments
{
    std::string         command;
    std::string         input;
    std::string         output;
    std::string         profile{"reference"};
    SceneImportContext  context;
    SourceSettings      settings;
    SceneReadLimits     limits;
    CompressionSettings compression;
    ProcessingSettings  processing;
    uint64_t            maxEncoderBytes{256ull << 20};
};

Status ParseUnsigned(std::string_view value, uint64_t& number)
{
    Status                       status;
    const std::from_chars_result parsed = std::from_chars(value.data(), value.data() + value.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
    {
        status = {StatusCode::InvalidArgument, "Expected an unsigned integer: " + std::string(value)};
    }
    return status;
}

float* FloatOption(Arguments& arguments, std::string_view name)
{
    struct Entry
    {
        std::string_view name;
        float*           value;
    };
    const Entry entries[]{
        {"--position-max-error", &arguments.processing.geometry.positionMaxAbsoluteError},
        {"--position-relative-error", &arguments.processing.geometry.positionMaxRelativeError},
        {"--color-min-psnr", &arguments.processing.textures.colorMinPsnr},
        {"--color-max-rmse", &arguments.processing.textures.colorMaxRmse},
        {"--scalar-max-error", &arguments.processing.textures.scalarMaxError},
        {"--normal-rms-degrees", &arguments.processing.textures.normalRmsAngleDegrees},
        {"--normal-max-degrees", &arguments.processing.textures.normalMaxAngleDegrees},
        {"--alpha-coverage-error", &arguments.processing.textures.alphaCoverageMaxError},
        {"--hdr-relative-rmse", &arguments.processing.textures.hdrMaxRelativeRmse},
    };
    float* result = nullptr;
    for (const Entry& entry : entries)
    {
        if (entry.name == name)
        {
            result = entry.value;
        }
    }
    return result;
}

bool* ToggleOption(Arguments& arguments, std::string_view name)
{
    struct Entry
    {
        std::string_view name;
        bool*            value;
    };
    const Entry entries[]{
        {"--vertex-fetch", &arguments.processing.geometry.optimizeVertexFetch},
        {"--texture-mipmaps", &arguments.processing.textures.generateMipmaps},
        {"--normal-bc5", &arguments.processing.textures.compressBC5Normals},
        {"--occlusion-bc4", &arguments.processing.textures.compressBC4Occlusion},
        {"--environment-bc6h", &arguments.processing.textures.compressBC6HEnvironment},
    };
    bool* result = nullptr;
    for (const Entry& entry : entries)
    {
        if (entry.name == name)
        {
            result = entry.value;
        }
    }
    return result;
}

Status ParseArguments(const std::vector<std::string>& values, Arguments& arguments)
{
    Status                  status;
    std::array<uint32_t, 3> pageLimits{};
    if (values.size() < 3)
    {
        status = {StatusCode::InvalidArgument, "Expected cook, inspect, or validate and an input path; use --help"};
    }
    else
    {
        arguments.command = values[1];
        if (arguments.command != "cook" && arguments.command != "inspect" && arguments.command != "validate")
        {
            status = {StatusCode::InvalidArgument, "Unknown command: " + arguments.command};
        }
    }
    for (size_t index = 2; status.Succeeded() && index < values.size(); ++index)
    {
        const std::string& option = values[index];
        if (!IsValidUtf8(option) || option.find('\0') != std::string::npos || option.empty())
        {
            status = {StatusCode::InvalidArgument, "Arguments must be nonempty UTF-8 text without null bytes"};
        }
        else if (option[0] != '-')
        {
            if (arguments.input.empty())
            {
                arguments.input = option;
            }
            else
            {
                status = {StatusCode::InvalidArgument, "Only one input path is accepted"};
            }
        }
        else if (++index == values.size())
        {
            status = {StatusCode::InvalidArgument, "Missing option value: " + option};
        }
        else
        {
            const std::string& value       = values[index];
            const bool         cooking     = arguments.command == "cook";
            float* const       floatValue  = FloatOption(arguments, option);
            bool* const        toggleValue = ToggleOption(arguments, option);
            if (cooking && (option == "-o" || option == "--output"))
            {
                arguments.output = value;
            }
            else if (cooking && option == "--profile")
            {
                arguments.profile = value;
            }
            else if (cooking && option == "--compression")
            {
                if (value == "none")
                {
                    arguments.compression.transport = TransportCodec::None;
                }
                else if (value == "zstd")
                {
                    arguments.compression.transport = TransportCodec::Zstandard;
                }
                else
                {
                    status = {StatusCode::InvalidArgument, "Compression must be none or zstd"};
                }
            }
            else if (cooking && option == "--mesh-compression")
            {
                if (value == "none" || value == "meshopt")
                {
                    arguments.compression.meshoptimizer = value == "meshopt";
                }
                else
                {
                    status = {StatusCode::InvalidArgument, "Mesh compression must be none or meshopt"};
                }
            }
            else if (cooking && option == "--texture-compression")
            {
                if (value == "none" || value == "bc7")
                {
                    arguments.processing.textures.compressBC7 = value == "bc7";
                }
                else
                {
                    status = {StatusCode::InvalidArgument, "Texture compression must be none or bc7"};
                }
            }
            else if (cooking && option == "--atlas")
            {
                if (value == "off" || value == "array" || value == "spatial")
                {
                    arguments.processing.textures.arrayPages   = value == "array";
                    arguments.processing.textures.spatialPages = value == "spatial";
                }
                else
                {
                    status = {StatusCode::InvalidArgument, "Atlas mode must be off, array or spatial"};
                }
            }
            else if (cooking && toggleValue != nullptr)
            {
                if (value == "on" || value == "off")
                {
                    *toggleValue = value == "on";
                }
                else
                {
                    status = {StatusCode::InvalidArgument, option + " must be on or off"};
                }
            }
            else if (cooking && floatValue != nullptr)
            {
                float                        number = 0;
                const std::from_chars_result parsed = std::from_chars(value.data(), value.data() + value.size(), number);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !std::isfinite(number)
                    || number < 0)
                {
                    status = {StatusCode::InvalidArgument, "Quality bounds must be finite nonnegative numbers"};
                }
                else
                {
                    *floatValue = number;
                }
            }
            else if (option == "--allocation-budget" || option == "--max-file-bytes" || option == "--codec-budget"
                     || (cooking
                         && (option == "--workers" || option == "--scene-index" || option == "--image-budget"
                             || option == "--geometry-budget" || option == "--zstd-level" || option == "--encoder-budget"
                             || option == "--position-bits" || option == "--texture-quality"
                             || option == "--texture-max-dimension" || option == "--atlas-max-dimension"
                             || option == "--atlas-max-layers" || option == "--atlas-max-pages"
                             || option == "--atlas-max-images")))
            {
                uint64_t number = 0;
                status          = ParseUnsigned(value, number);
                if (status.Succeeded())
                {
                    if (option == "--allocation-budget")
                    {
                        arguments.limits.maxAllocationBytes = number;
                    }
                    else if (option == "--max-file-bytes")
                    {
                        arguments.limits.package.maxFileBytes = number;
                        arguments.context.maxSourceBytes      = number;
                    }
                    else if (option == "--image-budget")
                    {
                        arguments.context.maxDecodedImageBytes = number;
                    }
                    else if (option == "--geometry-budget")
                    {
                        arguments.context.maxGeometryBytes = number;
                    }
                    else if (option == "--codec-budget")
                    {
                        arguments.limits.package.maxCodecBytes = number;
                    }
                    else if (option == "--encoder-budget")
                    {
                        arguments.maxEncoderBytes = number;
                    }
                    else if (option == "--zstd-level")
                    {
                        if (number < 1 || number > 19)
                        {
                            status = {StatusCode::InvalidArgument, "Zstandard level must be between 1 and 19"};
                        }
                        else
                        {
                            arguments.compression.zstandardLevel = static_cast<int32_t>(number);
                        }
                    }
                    else if (number > UINT32_MAX)
                    {
                        status = {StatusCode::InvalidArgument, "Option value exceeds 32-bit range: " + option};
                    }
                    else if (option == "--workers")
                    {
                        arguments.context.workerCount = static_cast<uint32_t>(number);
                    }
                    else if (option == "--position-bits")
                    {
                        arguments.processing.geometry.positionQuantizationBits = static_cast<uint32_t>(number);
                    }
                    else if (option == "--texture-quality")
                    {
                        arguments.processing.textures.bc7Quality = static_cast<uint32_t>(number);
                    }
                    else if (option == "--texture-max-dimension")
                    {
                        arguments.processing.textures.maxDimension = static_cast<uint32_t>(number);
                    }
                    else if (option == "--atlas-max-dimension")
                    {
                        if (number == 0)
                        {
                            status = {StatusCode::InvalidArgument, "Atlas limits must be positive"};
                        }
                        else
                        {
                            pageLimits[0] = static_cast<uint32_t>(number);
                        }
                    }
                    else if (option == "--atlas-max-layers")
                    {
                        arguments.processing.textures.arrayMaxLayers = static_cast<uint32_t>(number);
                    }
                    else if (option == "--atlas-max-pages")
                    {
                        if (number == 0)
                        {
                            status = {StatusCode::InvalidArgument, "Atlas limits must be positive"};
                        }
                        else
                        {
                            pageLimits[1] = static_cast<uint32_t>(number);
                        }
                    }
                    else if (option == "--atlas-max-images")
                    {
                        if (number == 0)
                        {
                            status = {StatusCode::InvalidArgument, "Atlas limits must be positive"};
                        }
                        else
                        {
                            pageLimits[2] = static_cast<uint32_t>(number);
                        }
                    }
                    else
                    {
                        arguments.settings.sceneIndex = static_cast<uint32_t>(number);
                    }
                }
            }
            else
            {
                status = {StatusCode::InvalidArgument, "Unknown option for " + arguments.command + ": " + option};
            }
        }
    }
    if (status.Succeeded() && (arguments.input.empty() || (arguments.command == "cook" && arguments.output.empty())))
    {
        status = {StatusCode::InvalidArgument, "An input path is required; cook also requires --output"};
    }
    if (status.Succeeded() && arguments.command == "cook" && arguments.profile != "reference" && arguments.profile != "desktop")
    {
        status = {StatusCode::UnsupportedFeature, "Available development profiles are reference and desktop"};
    }
    if (status.Succeeded() && arguments.command == "cook")
    {
        TextureSettings& textures = arguments.processing.textures;
        uint32_t* const  limits[]{textures.spatialPages ? &textures.spatialMaxDimension : &textures.arrayMaxDimension,
                                  textures.spatialPages ? &textures.spatialMaxPages : &textures.arrayMaxPages,
                                  textures.spatialPages ? &textures.spatialMaxImages : &textures.arrayMaxImages};
        for (size_t index = 0; index < pageLimits.size(); ++index)
        {
            if (pageLimits[index] != 0)
            {
                *limits[index] = pageLimits[index];
            }
        }
        status = ValidateProcessingSettings(arguments.processing);
        if (status.Succeeded() && arguments.profile == "reference" && HasProcessingOperations(arguments.processing))
        {
            status = {StatusCode::InvalidArgument,
                      "Geometry/texture processing requires --profile desktop; reference preserves original records"};
        }
    }
    if (status.Succeeded() && arguments.command == "cook")
    {
        if (!IsValidUtf8(arguments.output) || arguments.output.find('\0') != std::string::npos)
        {
            status = {StatusCode::InvalidArgument, "Output must be a valid UTF-8 path without null bytes"};
        }
        else
        {
            std::error_code             error;
            const std::filesystem::path input  = std::filesystem::u8path(arguments.input);
            const std::filesystem::path output = std::filesystem::u8path(arguments.output);
            const bool                  same   = std::filesystem::equivalent(input, output, error);
            if (same)
            {
                status = {StatusCode::InvalidArgument, "The output package must differ from the source file"};
            }
        }
    }
    return status;
}

Status Inspect(const Arguments& arguments, const Cancellation& cancellation)
{
    const PackageResult result = OpenPackage(arguments.input, arguments.limits.package, &cancellation);
    if (result.status.Succeeded())
    {
        std::cout << "{\"schema\":" << PackageSchemaVersion << ",\"resources\":[";
        size_t index = 0;
        for (const ResourceRecord& record : result.package->Resources())
        {
            std::cout << (index++ == 0 ? "" : ",") << "{\"id\":" << record.id
                      << ",\"type\":" << static_cast<uint32_t>(record.type) << ",\"version\":" << record.version
                      << ",\"codec\":" << static_cast<uint32_t>(record.codec) << ",\"encoding\":" << record.encoding
                      << ",\"stored_bytes\":" << record.storedBytes << ",\"decoded_bytes\":" << record.decodedBytes << '}';
        }
        std::cout << "]}\n";
    }
    return result.status;
}

void PrintSceneSummary(const Scene& scene)
{
    std::cout << "\"nodes\":" << scene.nodes.size() << ",\"meshes\":" << scene.meshes.size()
              << ",\"vertices\":" << scene.vertices.size() << ",\"indices\":" << scene.indices.size()
              << ",\"materials\":" << scene.materials.size() << ",\"images\":" << scene.images.size()
              << ",\"animations\":" << scene.animations.size() << ",\"skins\":" << scene.skins.size();
}

Status Validate(const Arguments& arguments, const Cancellation& cancellation)
{
    const SceneReadResult result = ReadScene(arguments.input, arguments.limits, &cancellation);
    if (result.status.Succeeded())
    {
        std::cout << "{\"valid\":true,";
        PrintSceneSummary(*result.scene);
        std::cout << "}\n";
    }
    return result.status;
}

void PrintDiagnostics(const std::vector<Diagnostic>& diagnostics)
{
    for (const Diagnostic& diagnostic : diagnostics)
    {
        std::cerr << "{\"severity\":" << static_cast<uint32_t>(diagnostic.severity) << ",\"source\":";
        JsonString(std::cerr, diagnostic.source);
        std::cerr << ",\"object\":";
        JsonString(std::cerr, diagnostic.object);
        std::cerr << ",\"message\":";
        JsonString(std::cerr, diagnostic.message);
        std::cerr << "}\n";
    }
}

Status Build(const std::vector<std::string>& values, const Cancellation& cancellation)
{
    Status                   status;
    SceneBuildOptions        options;
    std::string              input;
    std::vector<std::string> seen;
    for (size_t index = 2; status.Succeeded() && index < values.size(); ++index)
    {
        const std::string& option = values[index];
        if (option.empty() || !IsValidUtf8(option) || option.find('\0') != std::string::npos)
        {
            status = {StatusCode::InvalidArgument, "Build arguments require nonempty UTF-8 text"};
        }
        else if (option[0] != '-' && input.empty())
        {
            input = option;
        }
        else if (option[0] != '-' || std::find(seen.begin(), seen.end(), option) != seen.end())
        {
            status = {StatusCode::InvalidArgument, "Repeated build option or input: " + option};
        }
        else if (++index == values.size())
        {
            status = {StatusCode::InvalidArgument, "Missing build option value: " + option};
        }
        else
        {
            seen.push_back(option);
            const std::string& value = values[index];
            if (option == "--output-dir")
            {
                options.outputDirectory = value;
            }
            else if (option == "--force")
            {
                if (value == "on" || value == "off")
                {
                    options.force = value == "on";
                }
                else
                {
                    status = {StatusCode::InvalidArgument, "--force must be on or off"};
                }
            }
            else if (option == "--jobs" || option == "--memory-budget" || option == "--job-memory"
                     || option == "--manifest-budget")
            {
                uint64_t number = 0;
                status          = ParseUnsigned(value, number);
                if (status.Succeeded() && option == "--jobs")
                {
                    if (number == 0 || number > 64)
                    {
                        status = {StatusCode::InvalidArgument, "--jobs must be 1..64"};
                    }
                    else
                    {
                        options.workerCount = static_cast<uint32_t>(number);
                    }
                }
                else if (status.Succeeded() && option == "--memory-budget")
                {
                    options.maxReservedMemoryBytes = number;
                }
                else if (status.Succeeded() && option == "--manifest-budget")
                {
                    options.maxManifestBytes = number;
                }
                else if (status.Succeeded())
                {
                    if (number < (64ull << 20) || number > (1ull << 40))
                    {
                        status = {StatusCode::InvalidArgument, "--job-memory must be between 64 MiB and 1 TiB"};
                    }
                    else
                    {
                        const uint64_t unit                        = (number - (16ull << 20)) / 9;
                        options.importContext.maxSourceBytes       = unit;
                        options.importContext.maxDecodedImageBytes = unit * 2;
                        options.importContext.maxObjGeometryBytes  = unit * 2;
                        options.importContext.maxGeometryBytes     = unit * 2;
                        options.limits.maxAllocationBytes          = unit * 2;
                        options.limits.maxRuntimeGeometryBytes     = unit * 2;
                        options.maxEncoderBytes                    = std::min<uint64_t>(64ull << 20, unit / 2);
                    }
                }
            }
            else
            {
                status = {StatusCode::InvalidArgument, "Unknown build option: " + option};
            }
        }
    }
    if (status.Succeeded() && input.empty())
    {
        status = {StatusCode::InvalidArgument, "build requires a manifest path"};
    }
    if (status.Succeeded())
    {
        const SceneBuildManifest manifest = ReadSceneBuildManifest(input, options, &cancellation);
        status                            = manifest.status;
        if (status.Succeeded())
        {
            if (options.outputDirectory.empty())
            {
                options.outputDirectory = manifest.outputDirectory;
            }
            const SceneBuildResult result = BuildSceneBatch(manifest.jobs, options, &cancellation);
            status                        = result.status;
            std::cout << "{\"status\":" << static_cast<uint32_t>(result.status.code) << ",\"jobs\":[";
            size_t index = 0;
            for (const SceneBuildJobResult& job : result.jobs)
            {
                std::cout << (index++ == 0 ? "" : ",") << "{\"output\":";
                JsonString(std::cout, job.output);
                std::cout << ",\"status\":" << static_cast<uint32_t>(job.status.code) << ",\"disposition\":";
                JsonString(std::cout, job.disposition == SceneBuildDisposition::Built    ? "built"
                                      : job.disposition == SceneBuildDisposition::Reused ? "reused"
                                                                                         : "not_built");
                std::cout << ",\"published\":" << (job.published ? "true" : "false") << ",\"cache_key\":";
                JsonString(std::cout, job.cacheKey);
                std::cout << ",\"message\":";
                JsonString(std::cout, job.status.message);
                std::cout << ",\"diagnostics_omitted\":" << job.diagnosticsOmitted;
                std::cout << '}';
            }
            std::cout << "]}\n";
            for (const SceneBuildJobResult& job : result.jobs)
            {
                PrintDiagnostics(job.diagnostics);
            }
        }
    }
    return status;
}

Status Cook(const Arguments& arguments)
{
    Status             status;
    SceneImportRequest request;
    request.path     = arguments.input;
    request.settings = arguments.settings;
    request.control  = std::make_shared<SceneImportControl>(SceneImportCallbacks{nullptr, &Interrupted, nullptr});
    const SceneImportResult imported = ImportScene(request, arguments.context);
    status                           = {imported.status, imported.error};
    PrintDiagnostics(imported.diagnostics);
    SceneCookResult cooked;
    if (status.Succeeded())
    {
        SceneCookOptions options;
        options.settings = arguments.processing;
        options.limits   = arguments.limits;
        cooked           = CookScene(imported.asset, options, request.control.get());
        status           = cooked.status;
        PrintDiagnostics(cooked.diagnostics);
    }
    if (status.Succeeded())
    {
        SceneWriteOptions options;
        options.metadata.sourceFormat     = imported.format;
        options.metadata.importerVersion  = imported.importerVersion;
        options.metadata.buildFingerprint = LibraryBuildFingerprint();
        options.metadata.profile          = arguments.profile;
        options.metadata.settings         = arguments.settings;
        options.metadata.diagnostics      = imported.diagnostics;
        options.metadata.diagnostics.insert(options.metadata.diagnostics.end(), cooked.diagnostics.begin(),
                                            cooked.diagnostics.end());
        options.metadata.processing = arguments.processing;
        options.limits              = arguments.limits;
        options.compression         = arguments.compression;
        options.maxEncoderBytes     = arguments.maxEncoderBytes;
        options.durable             = true;
        status                      = WriteScene(arguments.output, *cooked.scene, options, request.control.get());
    }
    if (status.Succeeded())
    {
        std::cout << "{\"output\":";
        JsonString(std::cout, arguments.output);
        std::cout << ",\"profile\":";
        JsonString(std::cout, arguments.profile);
        std::cout << ',';
        PrintSceneSummary(*cooked.scene);
        std::cout << "}\n";
    }
    return status;
}

void Help()
{
    std::cout
        << "zenassetc cook SOURCE --output PACKAGE [--profile reference|desktop] [--workers N] [--scene-index N]\n"
           "               [--image-budget BYTES] [--geometry-budget BYTES] [--allocation-budget BYTES] [--max-file-bytes BYTES]\n"
           "               [--compression none|zstd] [--zstd-level 1..19] [--encoder-budget BYTES]\n"
           "               [--mesh-compression none|meshopt]\n"
           "               [--vertex-fetch off|on] [--position-bits 0..24]\n"
           "               [--position-max-error METERS] [--position-relative-error FRACTION]\n"
           "               [--texture-mipmaps off|on] [--texture-compression none|bc7]\n"
           "               [--normal-bc5 off|on] [--occlusion-bc4 off|on] [--environment-bc6h off|on]\n"
           "               [--hdr-relative-rmse FRACTION]\n"
           "               [--texture-quality 0..4] [--texture-max-dimension N]\n"
           "               [--atlas off|array|spatial] [--atlas-max-dimension N] [--atlas-max-layers 2..2048]\n"
           "               [--atlas-max-pages N] [--atlas-max-images N]\n"
           "               [--color-min-psnr DB] [--color-max-rmse FRACTION] [--scalar-max-error FRACTION]\n"
           "               [--normal-rms-degrees DEGREES] [--normal-max-degrees DEGREES]\n"
           "               [--alpha-coverage-error FRACTION]\n"
           "zenassetc inspect PACKAGE [--max-file-bytes BYTES]\n"
           "zenassetc validate PACKAGE [--allocation-budget BYTES] [--max-file-bytes BYTES] [--codec-budget BYTES]\n"
           "zenassetc --version\n"
           "zenassetc build MANIFEST [--output-dir DIR] [--jobs 1..64] [--force off|on]\n"
           "               [--memory-budget BYTES] [--job-memory BYTES] [--manifest-budget BYTES]\n"
           "Successful commands print JSON to stdout; diagnostics go to stderr. Ctrl+C cancels cooperatively.\n";
}

int Run(const std::vector<std::string>& values)
{
    Status status;
    std::signal(SIGINT, &Interrupt);
    if (values.size() == 2 && (values[1] == "--help" || values[1] == "-h"))
    {
        Help();
    }
    else if (values.size() == 2 && values[1] == "--version")
    {
        std::cout << "ZenAsset " << LibraryVersion() << " schema=" << PackageSchemaVersion << " processor=" << ProcessorVersion
                  << " build=" << LibraryBuildFingerprint() << '\n';
    }
    else if (values.size() > 1 && values[1] == "build")
    {
        const Cancellation cancellation(&Interrupted);
        status = Build(values, cancellation);
    }
    else
    {
        Arguments arguments;
        status = ParseArguments(values, arguments);
        if (status.Succeeded())
        {
            const Cancellation cancellation(&Interrupted);
            if (arguments.command == "cook")
            {
                status = Cook(arguments);
            }
            else if (arguments.command == "inspect")
            {
                status = Inspect(arguments, cancellation);
            }
            else
            {
                status = Validate(arguments, cancellation);
            }
        }
    }
    if (!status.Succeeded())
    {
        PrintFailure(status);
    }
    return status.Succeeded()                         ? 0
         : status.code == StatusCode::InvalidArgument ? 2
         : status.code == StatusCode::Cancelled       ? 4
                                                      : 3;
}
} // namespace

#if defined(_WIN32)
int wmain(int count, wchar_t** values)
{
    std::vector<std::string> arguments;
    bool                     valid = true;
    for (int index = 0; valid && index < count; ++index)
    {
        const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, values[index], -1, nullptr, 0, nullptr, nullptr);
        valid           = bytes > 0;
        if (valid)
        {
            std::string argument(static_cast<size_t>(bytes), '\0');
            valid =
                WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, values[index], -1, argument.data(), bytes, nullptr, nullptr)
                == bytes;
            argument.pop_back();
            arguments.push_back(std::move(argument));
        }
    }
    if (!valid)
    {
        PrintFailure({StatusCode::InvalidArgument, "Command line contains malformed Unicode"});
    }
    return valid ? Run(arguments) : 2;
}
#else
int main(int count, char** values)
{
    return Run(std::vector<std::string>(values, values + count));
}
#endif
