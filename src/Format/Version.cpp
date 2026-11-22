#include "ZenAsset/Format/Version.h"
#include "BuildFingerprint.h"

namespace zenasset
{
std::string_view LibraryVersion()
{
    return ZENASSET_LIBRARY_VERSION;
}
std::string_view LibraryBuildFingerprint()
{
    return ZENASSET_BUILD_FINGERPRINT;
}
} // namespace zenasset
