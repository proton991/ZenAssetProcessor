#include "ZenAsset/Format/Utf8.h"
#include <cstdint>

namespace zenasset
{
bool IsValidUtf8(std::string_view text)
{
    bool   valid  = true;
    size_t offset = 0;
    while (valid && offset < text.size())
    {
        const uint8_t lead      = static_cast<uint8_t>(text[offset++]);
        uint32_t      remaining = 0;
        uint32_t      point     = lead;
        uint32_t      minimum   = 0;
        if (lead >= 0xc2 && lead <= 0xdf)
        {
            remaining = 1;
            point     = lead & 0x1f;
            minimum   = 0x80;
        }
        else if (lead >= 0xe0 && lead <= 0xef)
        {
            remaining = 2;
            point     = lead & 0x0f;
            minimum   = 0x800;
        }
        else if (lead >= 0xf0 && lead <= 0xf4)
        {
            remaining = 3;
            point     = lead & 0x07;
            minimum   = 0x10000;
        }
        else
        {
            valid = lead < 0x80;
        }
        valid = valid && remaining <= text.size() - offset;
        for (uint32_t index = 0; valid && index < remaining; ++index)
        {
            const uint8_t next = static_cast<uint8_t>(text[offset++]);
            valid              = (next & 0xc0) == 0x80;
            point              = (point << 6) | (next & 0x3f);
        }
        valid = valid && point >= minimum && point <= 0x10ffff && (point < 0xd800 || point > 0xdfff);
    }
    return valid;
}
} // namespace zenasset
