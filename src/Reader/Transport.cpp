#include "Reader/Transport.h"
#include "Diagnostics/StageMeasurements.h"
#include "Format/FileIO.h"
#include "Format/AssetKernels.h"
#include <algorithm>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

namespace zenasset::detail
{
Status ReadZstandardResource(FILE*                 file,
                             const ResourceRecord& record,
                             std::span<std::byte>  output,
                             uint64_t              maxScratchBytes,
                             const Cancellation*   cancellation)
{
    ZENASSET_MEASURE_SCOPE(TransportDecode);
    Status               status;
    const size_t         inputBytes = static_cast<size_t>(std::min<uint64_t>(record.storedBytes, 64u << 10));
    std::vector<uint8_t> input;
    if (record.storedBytes < 6)
    {
        status = {StatusCode::InvalidData, "Zstandard frame header is truncated"};
    }
    else if (inputBytes > maxScratchBytes)
    {
        status = {StatusCode::ResourceLimit, "Zstandard input staging exceeds the codec budget"};
    }
    else
    {
        input.resize(inputBytes);
        status = ReadFileBytes(file, input.data(), input.size(), cancellation);
    }
    ZSTD_FrameHeader header{};
    size_t           workspaceBytes = 0;
    if (status.Succeeded())
    {
        const size_t headerResult = ZSTD_getFrameHeader(&header, input.data(), input.size());
        if (ZSTD_isError(headerResult) || headerResult != 0 || header.frameType != ZSTD_frame || header.dictID != 0
            || header.frameContentSize == ZSTD_CONTENTSIZE_UNKNOWN || header.frameContentSize != output.size())
        {
            status = {StatusCode::InvalidData, "Zstandard frame type, content size or dictionary is invalid"};
        }
        else if (header.windowSize > maxScratchBytes || header.windowSize > SIZE_MAX)
        {
            status = {StatusCode::ResourceLimit, "Zstandard window exceeds the codec budget"};
        }
        else
        {
            const size_t estimate = ZSTD_estimateDStreamSize(static_cast<size_t>(header.windowSize));
            if (ZSTD_isError(estimate) || estimate > SIZE_MAX - 7)
            {
                status = {StatusCode::ResourceLimit, "Zstandard decoder workspace cannot be represented"};
            }
            else
            {
                workspaceBytes = (estimate + 7) & ~size_t(7);
                if (workspaceBytes > maxScratchBytes - inputBytes)
                {
                    status = {StatusCode::ResourceLimit, "Zstandard decoder workspace exceeds the codec budget"};
                }
            }
        }
    }
    std::vector<uint64_t> workspace;
    ZSTD_DStream*         decoder = nullptr;
    if (status.Succeeded())
    {
        workspace.resize(workspaceBytes / sizeof(uint64_t));
        decoder = ZSTD_initStaticDStream(workspace.data(), workspaceBytes);
        if (decoder == nullptr)
        {
            status = {StatusCode::ResourceLimit, "Cannot initialize bounded Zstandard decoder"};
        }
    }
    kernels::HashState hash{};
    kernels::ResetHash(hash, 0);
    ZSTD_inBuffer  encoded{input.data(), input.size(), 0};
    ZSTD_outBuffer decoded{output.data(), output.size(), 0};
    uint64_t       remaining = record.storedBytes - inputBytes;
    size_t         hint      = 1;
    if (status.Succeeded())
    {
        ZENASSET_MEASURE_SCOPE(Checksum);
        kernels::UpdateHash(hash, input.data(), input.size());
    }
    while (status.Succeeded() && hint != 0)
    {
        if (cancellation != nullptr && cancellation->IsCancelled())
        {
            status = {StatusCode::Cancelled, "Zstandard resource read cancelled"};
        }
        if (status.Succeeded() && encoded.pos == encoded.size && remaining > 0)
        {
            const size_t count = static_cast<size_t>(std::min<uint64_t>(remaining, input.size()));
            status             = ReadFileBytes(file, input.data(), count, cancellation);
            if (status.Succeeded())
            {
                ZENASSET_MEASURE_BEGIN(checksum, Checksum);
                kernels::UpdateHash(hash, input.data(), count);
                ZENASSET_MEASURE_END(checksum);
                remaining -= count;
                encoded    = {input.data(), count, 0};
            }
        }
        if (status.Succeeded())
        {
            const size_t beforeInput  = encoded.pos;
            const size_t beforeOutput = decoded.pos;
            // Bound each codec call's output as well as its input. A small
            // compressed block may otherwise expand far past one poll interval.
            decoded.size = decoded.pos + std::min<size_t>(output.size() - decoded.pos, 128u << 10);
            hint         = ZSTD_decompressStream(decoder, &decoded, &encoded);
            if (ZSTD_isError(hint))
            {
                status = {StatusCode::InvalidData, "Zstandard resource decoding failed"};
            }
            else if (hint != 0 && encoded.pos == beforeInput && decoded.pos == beforeOutput)
            {
                status = {StatusCode::InvalidData, "Zstandard frame is truncated or exceeds its declared decoded length"};
            }
        }
    }
    if (status.Succeeded() && (remaining != 0 || encoded.pos != encoded.size || decoded.pos != output.size()))
    {
        status = {StatusCode::InvalidData, "Zstandard resource has trailing data or an incorrect decoded length"};
    }
    ZENASSET_MEASURE_BEGIN(finalChecksum, Checksum);
    if (status.Succeeded() && kernels::FinishHash(hash) != record.checksum)
    {
        status = {StatusCode::InvalidData, "Zstandard stored resource checksum does not match"};
    }
    ZENASSET_MEASURE_END(finalChecksum);
    ZENASSET_MEASURE_ADD(TransportDecodedResources, status.Succeeded() ? 1 : 0);
    return status;
}
} // namespace zenasset::detail
