#include "ZenAsset/Format/Version.h"
#include "BuildFingerprint.h"

namespace zenasset
{
std::string_view LibraryVersion()
{
    return "0.1.0";
}
std::string_view LibraryBuildFingerprint()
{
    return ZENASSET_BUILD_FINGERPRINT;
}
} // namespace zenasset
