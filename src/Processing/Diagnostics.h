#pragma once
#include "ZenAsset/Format/BuildMetadata.h"

namespace zenasset::detail
{
// Account for both the new contiguous record allocation and retained strings.
// Existing diagnostic storage is already charged by its producing pass.
inline Status AddProcessingWarning(std::string_view         object,
                                   std::string_view         reason,
                                   uint64_t&                remaining,
                                   std::vector<Diagnostic>& diagnostics)
{
    Status         status;
    const uint64_t count = diagnostics.size() + 1;
    const uint64_t bytes = object.size() + reason.size() + 2;
    if (count > SIZE_MAX / sizeof(Diagnostic) || count * sizeof(Diagnostic) > remaining
        || bytes > remaining - count * sizeof(Diagnostic))
    {
        status = {StatusCode::ResourceLimit, "Processing diagnostics exceed the allocation budget"};
    }
    else
    {
        remaining -= bytes + sizeof(Diagnostic);
        diagnostics.reserve(static_cast<size_t>(count));
        diagnostics.push_back(
            {DiagnosticSeverity::Warning, StatusCode::UnsupportedFeature, {}, std::string(object), std::string(reason)});
    }
    return status;
}
} // namespace zenasset::detail
