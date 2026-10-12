#include "guid.h"

#include <chrono>
#include <cstdio>
#include <random>

namespace df {

// Mixed-endian GPT on-disk layout:
//   uint32 b3b2b1b0 | uint16 b5b4 | uint16 b7b6 | bytes b8..b15 verbatim
void Guid::ReadLE(const uint8_t* p) {
    b[0] = p[3]; b[1] = p[2]; b[2] = p[1]; b[3] = p[0];
    b[4] = p[5]; b[5] = p[4];
    b[6] = p[7]; b[7] = p[6];
    for (int i = 0; i < 8; ++i) b[8 + i] = p[8 + i];
}

void Guid::WriteLE(uint8_t* p) const {
    p[0] = b[3]; p[1] = b[2]; p[2] = b[1]; p[3] = b[0];
    p[4] = b[5]; p[5] = b[4];
    p[6] = b[7]; p[7] = b[6];
    for (int i = 0; i < 8; ++i) p[8 + i] = b[8 + i];
}

static int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

Guid Guid::Parse(const std::string& s) {
    Guid g{};
    std::string hex;
    for (char c : s) {
        if (c == '-' || c == '{' || c == '}') continue;
        if (HexVal(c) < 0) return Guid{};
        if (hex.size() < 32) hex += c;
    }
    if (hex.size() != 32) return Guid{};
    for (int i = 0; i < 16; ++i) g.b[i] = uint8_t((HexVal(hex[2 * i]) << 4) | HexVal(hex[2 * i + 1]));
    return g;
}

std::string Guid::Str() const {
    // b[] is stored in text order: b[0] is the most significant byte of Data1
    char buf[40];
    std::snprintf(buf, sizeof(buf),
        "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
        b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return buf;
}

Guid Guid::Random() {
    static std::mt19937_64 rng(uint32_t(std::chrono::high_resolution_clock::now().time_since_epoch().count()) ^ 0x9E3779B9u);
    Guid g;
    for (int i = 0; i < 16; i += 8) {
        uint64_t v = rng();
        std::memcpy(g.b + i, &v, 8);
    }
    g.b[7] = uint8_t((g.b[7] & 0x0F) | 0x40);  // version 4
    g.b[9] = uint8_t((g.b[9] & 0x3F) | 0x80);  // variant
    return g;
}

Guid Guid::GptSystemEsp()  { return Parse("C12A7328-F81F-11D2-BA4B-00A0C93EC93B"); }
Guid Guid::GptBiosBoot()   { return Parse("21686148-6449-6E6F-744E-656564454649"); }
Guid Guid::GptMsr()        { return Parse("E3C9E316-0B5C-4DB8-817D-F92DF00215AE"); }
Guid Guid::GptBasicData()  { return Parse("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"); }
Guid Guid::GptLinuxFs()    { return Parse("0FC63DAF-8483-4772-8E79-3D69D8477DE4"); }
Guid Guid::GptLinuxLvm()   { return Parse("E6D6D379-F507-44C2-A23C-238F2A3DF928"); }
Guid Guid::GptLinuxSwap()  { return Parse("0657FD6D-A4AB-43C4-84E5-0933C84B4F4F"); }
Guid Guid::GptWindowsRe()  { return Parse("DE94BBA4-06D1-4D40-A16A-BFD50179D6AC"); }
Guid Guid::GptVmwareVmfs() { return Parse("AA31E02A-400F-11DB-9590-000C2911D1B8"); }
Guid Guid::GptFreebsdBoot(){ return Parse("83BD6B9D-7F41-11DC-BE0B-001560B84F0F"); }

}  // namespace df
