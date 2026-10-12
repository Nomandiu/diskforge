#include "common.h"

namespace df {

static void AppendU8(std::string& out, unsigned cp) {
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F));
        out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F));
    }
}

std::string U16ToU8(const std::u16string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        uint16_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size()) {
            uint16_t c2 = s[++i];
            if (c2 >= 0xDC00 && c2 <= 0xDFFF) {
                AppendU8(out, 0x10000 + (((uint32_t(c) - 0xD800) << 10) | (uint32_t(c2) - 0xDC00)));
                continue;
            }
        }
        AppendU8(out, c);
    }
    return out;
}

std::u16string U8ToU16(const std::string& s) {
    std::u16string out;
    auto peek = [&](size_t i) -> unsigned { return i < s.size() ? uint8_t(s[i]) : 0u; };
    for (size_t i = 0; i < s.size();) {
        unsigned b = peek(i);
        unsigned cp = b; size_t len = 1;
        if (b >= 0xF0) { cp = b & 0x07; len = 4; }
        else if (b >= 0xE0) { cp = b & 0x0F; len = 3; }
        else if (b >= 0xC0) { cp = b & 0x1F; len = 2; }
        for (size_t k = 1; k < len && i + k < s.size(); ++k) cp = (cp << 6) | (peek(i + k) & 0x3F);
        i += len;
        if (cp > 0xFFFF) {
            cp -= 0x10000;
            out += char16_t(0xD800 + (cp >> 10));
            out += char16_t(0xDC00 + (cp & 0x3FF));
        } else {
            out += char16_t(cp);
        }
    }
    return out;
}

std::string LowerCopy(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

}  // namespace df
