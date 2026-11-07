#pragma once
#include "ZenAsset/Format/Assembly.h"
#include "ZenAsset/Reader/Scene.h"

namespace zenasset
{
struct AssemblyReadResult
{
    Status                               status;
    std::shared_ptr<const SceneAssembly> assembly;
};

// Reads the native manifest only. Referenced model payloads remain unopened.
AssemblyReadResult ReadAssembly(const std::string&     path,
                                const SceneReadLimits& limits       = {},
                                const Cancellation*    cancellation = nullptr);
AssemblyReadResult ReadAssembly(const Package&         package,
                                const SceneReadLimits& limits       = {},
                                const Cancellation*    cancellation = nullptr);
// Opens each model sequentially and verifies its header/directory identity.
// Does not validate/decode model payloads; ReadScene/ValidatePayloads do that.
Status VerifyAssemblyDependencies(const std::string&   path,
                                  const SceneAssembly& assembly,
                                  const ReadLimits&    limits       = {},
                                  const Cancellation*  cancellation = nullptr);
} // namespace zenasset
