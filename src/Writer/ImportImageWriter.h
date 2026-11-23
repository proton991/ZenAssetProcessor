#pragma once
#include "Writer/DirectoryParts.h"
#include "ZenAsset/Processor/Import.h"

namespace zenasset::detail
{
// Owns the publication lock only while import workers stage immutable texture parts.
// Descriptors retain a shared reader lease through final publication or source fallback.
class ImportImageWriter
{
public:
    static std::unique_ptr<ImportImageWriter> Open(const SceneImportImageCache& target,
                                                   size_t                       imageCount,
                                                   const Cancellation*          cancellation);
    ~ImportImageWriter();
    bool Store(uint32_t index, const std::string& name, const std::shared_ptr<ImagePixels>& pixels);

private:
    ImportImageWriter(const std::filesystem::path& manifest,
                      const SceneWriteOptions&     options,
                      const Cancellation*          cancellation);
    SceneWriteOptions              m_options;
    std::shared_ptr<CacheFileLock> m_pin;
    const Cancellation*            m_cancellation;
    DirectoryParts                 m_parts;
};
} // namespace zenasset::detail
