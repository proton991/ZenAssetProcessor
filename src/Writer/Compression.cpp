#include "Writer/Compression.h"
#include "Diagnostics/StageMeasurements.h"
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <algorithm>

namespace zenasset::detail
{
namespace
{
Status ZstandardStatus(size_t result)
{
    Status status;
    if (ZSTD_isError(result))
    {
        status = {StatusCode::InvalidData, std::string("Zstandard compression failed: ") + ZSTD_getErrorName(result)};
    }
    return status;
}

Status ConfigureEncoder(ZSTD_CCtx* encoder, const ZSTD_compressionParameters& parameters, size_t sourceBytes)
{
    Status status = ZstandardStatus(ZSTD_CCtx_setCParams(encoder, parameters));
    if (status.Succeeded())
    {
        status = ZstandardStatus(ZSTD_CCtx_setParameter(encoder, ZSTD_c_contentSizeFlag, 1));
    }
    if (status.Succeeded())
    {
        status = ZstandardStatus(ZSTD_CCtx_setParameter(encoder, ZSTD_c_checksumFlag, 0));
    }
    if (status.Succeeded())
    {
        status = ZstandardStatus(ZSTD_CCtx_setParameter(encoder, ZSTD_c_dictIDFlag, 0));
    }
    if (status.Succeeded())
    {
        status = ZstandardStatus(ZSTD_CCtx_setPledgedSrcSize(encoder, sourceBytes));
    }
    return status;
}

void Encode(ZSTD_CCtx* encoder, std::span<const std::byte> input, CompressedResource& result, const Cancellation* cancellation)
{
    ZSTD_outBuffer output{result.bytes.data(), result.bytes.size(), 0};
    size_t         position = 0;
    size_t         pending  = 1;
    // Feed bounded slices with e_continue first. Passing the complete input to
    // e_end on the first call would select Zstandard's blocking one-shot path.
    while (result.status.Succeeded() && pending != 0 && output.pos < output.size)
    {
        if (cancellation != nullptr && cancellation->IsCancelled())
        {
            result.status = {StatusCode::Cancelled, "Scene compression cancelled"};
        }
        else
        {
            const size_t            count = std::min<size_t>(input.size() - position, 128ull << 10);
            ZSTD_inBuffer           source{input.data() + position, count, 0};
            const ZSTD_EndDirective directive  = position < input.size() ? ZSTD_e_continue : ZSTD_e_end;
            pending                            = ZSTD_compressStream2(encoder, &output, &source, directive);
            result.status                      = ZstandardStatus(pending);
            position                          += source.pos;
            // e_continue may report zero before the frame has been finished.
            if (directive == ZSTD_e_continue)
            {
                pending = 1;
            }
        }
    }
    if (result.status.Succeeded() && pending == 0 && output.pos < input.size())
    {
        result.codec = TransportCodec::Zstandard;
        result.bytes.resize(output.pos);
    }
    else
    {
        std::vector<std::byte>().swap(result.bytes);
    }
}
} // namespace

Status ValidateCompressionSettings(const CompressionSettings& settings)
{
    Status status;
    if (settings.transport != TransportCodec::None && settings.transport != TransportCodec::Zstandard)
    {
        status = {StatusCode::UnsupportedCodec, "Unsupported scene transport compression codec"};
    }
    else if (settings.zstandardLevel < 1 || settings.zstandardLevel > 19)
    {
        status = {StatusCode::InvalidArgument, "Zstandard compression level must be between 1 and 19"};
    }
    return status;
}

CompressedResource CompressResource(std::span<const std::byte> input,
                                    const CompressionSettings& settings,
                                    uint64_t                   availableBytes,
                                    uint64_t                   maxEncoderBytes,
                                    const Cancellation*        cancellation)
{
    ZENASSET_MEASURE_SCOPE(TransportEncode);
    CompressedResource result;
    result.status = ValidateCompressionSettings(settings);
    if (result.status.Succeeded() && cancellation != nullptr && cancellation->IsCancelled())
    {
        result.status = {StatusCode::Cancelled, "Scene compression cancelled"};
    }
    if (result.status.Succeeded() && settings.transport == TransportCodec::Zstandard && input.size() >= 64)
    {
        ZENASSET_MEASURE_ADD(TransportEncodeAttempts, 1);
        ZSTD_compressionParameters parameters = ZSTD_getCParams(settings.zstandardLevel, input.size(), 0);
        parameters.windowLog                  = std::min(parameters.windowLog, 23u);
        const size_t estimate                 = ZSTD_estimateCStreamSize_usingCParams(parameters);
        result.status                         = ZstandardStatus(estimate);
        size_t words                          = 0;
        if (result.status.Succeeded())
        {
            if (estimate > SIZE_MAX - 7)
            {
                result.status = {StatusCode::ResourceLimit, "Zstandard workspace size overflows addressable memory"};
            }
            else
            {
                words                         = (estimate + 7) / sizeof(uint64_t);
                const uint64_t workspaceBytes = words * sizeof(uint64_t);
                if (workspaceBytes > maxEncoderBytes || workspaceBytes > availableBytes
                    || input.size() > availableBytes - workspaceBytes)
                {
                    result.status = {StatusCode::ResourceLimit, "Zstandard encoder exceeds the allocation budget"};
                }
            }
        }
        if (result.status.Succeeded())
        {
            // uint64_t provides the eight-byte alignment required by static
            // contexts. Static mode never allocates hidden codec heap storage.
            std::vector<uint64_t> workspace(words);
            ZSTD_CCtx*            encoder = ZSTD_initStaticCCtx(workspace.data(), workspace.size() * sizeof(uint64_t));
            if (encoder == nullptr)
            {
                result.status = {StatusCode::ResourceLimit, "Cannot initialize bounded Zstandard encoder"};
            }
            else
            {
                result.status = ConfigureEncoder(encoder, parameters, input.size());
                if (result.status.Succeeded())
                {
                    // More than raw size cannot improve the package. Stop and
                    // use raw bytes as soon as the bounded output is exhausted.
                    result.bytes.resize(input.size());
                    Encode(encoder, input, result, cancellation);
                }
            }
        }
    }
    ZENASSET_MEASURE_ADD(TransportEncodedResources, result.status.Succeeded() && result.codec != TransportCodec::None ? 1 : 0);
    return result;
}
} // namespace zenasset::detail
