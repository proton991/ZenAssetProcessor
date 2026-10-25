#include "ZenAsset/Processor/Import.h"
#include "Format/FileIO.h"
#include <algorithm>
#include <cwctype>
#include <filesystem>

namespace zenasset
{
namespace
{
std::string FileIdentity(const std::string& text, bool requireFile)
{
    std::string           result;
    std::filesystem::path path;
    std::error_code       error;
    if (detail::MakePath(text, path).Succeeded())
    {
        path = std::filesystem::weakly_canonical(path, error);
        if (!error && (!requireFile || std::filesystem::is_regular_file(path, error)) && !error)
        {
#if defined(_WIN32)
            std::wstring native = path.native();
            std::transform(native.begin(), native.end(), native.begin(),
                           [](wchar_t character) { return static_cast<wchar_t>(std::towupper(character)); });
            path = std::filesystem::path(native);
#endif
            const std::u8string utf8 = path.generic_u8string();
            result.assign(reinterpret_cast<const char*>(utf8.data()), utf8.size());
        }
    }
    return result;
}
} // namespace

SceneImportFileProgress SceneImportControl::GetFileProgress() const
{
    std::lock_guard<std::mutex> lock(m_fileMutex);
    return m_fileProgress;
}

void SceneImportControl::RegisterFile(const std::string& path)
{
    UpdateFileProgress(path, FileProgressAction::Register);
}

void SceneImportControl::CompleteFile(const std::string& path)
{
    UpdateFileProgress(path, FileProgressAction::Complete);
}

void SceneImportControl::FinishFileDiscovery()
{
    UpdateFileProgress({}, FileProgressAction::FinishDiscovery);
}

void SceneImportControl::UpdateFileProgress(const std::string& path, FileProgressAction action)
{
    const std::string identity = action == FileProgressAction::FinishDiscovery
                                   ? std::string()
                                   : FileIdentity(path, action == FileProgressAction::Register);
    // Serialize reporting as well as mutations, so callbacks cannot arrive out of order.
    std::lock_guard<std::mutex> callbackLock(m_fileCallbackMutex);
    const bool                  cancelled = IsCancelled();
    SceneImportFileProgress     snapshot;
    bool                        changed = false;
    {
        std::lock_guard<std::mutex> lock(m_fileMutex);
        if (!cancelled)
        {
            if (action == FileProgressAction::FinishDiscovery)
            {
                changed                   = !m_fileProgress.totalKnown;
                m_fileProgress.totalKnown = true;
            }
            else if (!identity.empty())
            {
                if (action == FileProgressAction::Register)
                {
                    if (m_files.emplace(identity, false).second)
                    {
                        ++m_fileProgress.totalFiles;
                        m_fileProgress.totalKnown = false;
                        changed                   = true;
                    }
                }
                else
                {
                    std::map<std::string, bool>::iterator found = m_files.find(identity);
                    if (found != m_files.end() && !found->second)
                    {
                        found->second = true;
                        ++m_fileProgress.processedFiles;
                        changed = true;
                    }
                }
            }
        }
        snapshot = m_fileProgress;
    }
    if (changed && m_callbacks.fileProgressChanged != nullptr)
    {
        m_callbacks.fileProgressChanged(m_callbacks.user, snapshot);
    }
}
} // namespace zenasset
