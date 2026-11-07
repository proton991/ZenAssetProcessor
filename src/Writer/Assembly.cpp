#include "ZenAsset/Processor/Assembly.h"
#include "Format/AssemblySchema.h"
#include "Writer/PackageWriter.h"

namespace zenasset
{
namespace
{
struct AssemblyEncodingContext
{
    const SceneAssembly&     assembly;
    const SceneWriteOptions& options;
    const Cancellation*      cancellation;
};

Status EncodeAssembly(FILE* file, void* user)
{
    const AssemblyEncodingContext& context = *static_cast<AssemblyEncodingContext*>(user);
    detail::ResourceEncoder        encoder(file, 1, context.options, context.cancellation);
    encoder.Encode(ResourceType::Assembly, 0, context.assembly);
    return encoder.Finish();
}
} // namespace

Status WriteAssembly(const std::string&       path,
                     const SceneAssembly&     assembly,
                     const SceneWriteOptions& options,
                     const Cancellation*      cancellation)
{
    if (options.published != nullptr)
    {
        *options.published = false;
    }
    Status status = detail::ValidateCompressionSettings(options.compression);
    if (status.Succeeded() && cancellation != nullptr && cancellation->IsCancelled())
    {
        status = {StatusCode::Cancelled, "Assembly write cancelled"};
    }
    if (status.Succeeded() && !detail::AssemblyValidationFits(assembly, options.limits.maxAllocationBytes))
    {
        status = {StatusCode::ResourceLimit, "Assembly validation exceeds allocation budget"};
    }
    if (status.Succeeded())
    {
        status = ValidateAssembly(assembly);
    }
    if (status.Succeeded())
    {
        AssemblyEncodingContext context{assembly, options, cancellation};
        status = detail::WritePackage(path, 1, options, cancellation, EncodeAssembly, &context);
    }
    return status;
}
} // namespace zenasset
