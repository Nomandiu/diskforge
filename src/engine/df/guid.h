// 128-bit GUID (mixed-endian disk format aware helpers)
#pragma once

#include "common.h"

namespace df {

struct Guid {
    uint8_t b[16];

    bool operator==(const Guid& o) const { return std::memcmp(b, o.b, 16) == 0; }
    bool operator!=(const Guid& o) const { return !(*this == o); }
    bool IsZero() const {
        for (int i = 0; i < 16; ++i) if (b[i]) return false;
        return true;
    }

    // Parse "12345678-1234-1234-1234-123456789ABC" (case-insensitive); zero on failure.
    static Guid Parse(const std::string& s);
    std::string Str() const;          // canonical string
    static Guid Random();             // random v4 GUID

    void ReadLE(const uint8_t* p);    // GUID as stored in GPT structures (mixed endian)
    void WriteLE(uint8_t* p) const;

    // Well-known GPT partition type GUIDs
    static Guid GptSystemEsp();       // C12A7328-F81F-11D2-BA4B-00A0C93EC93B
    static Guid GptBiosBoot();        // 21686148-6449-6E6F-744E-656564454649
    static Guid GptMsr();             // E3C9E316-0B5C-4DB8-817D-F92DF00215AE
    static Guid GptBasicData();       // EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
    static Guid GptLinuxFs();         // 0FC63DAF-8483-4772-8E79-3D69D8477DE4
    static Guid GptLinuxLvm();        // E6D6D379-F507-44C2-A23C-238F2A3DF928
    static Guid GptLinuxSwap();       // 0657FD6D-A4AB-43C4-84E5-0933C84B4F4F
    static Guid GptWindowsRe();       // DE94BBA4-06D1-4D40-A16A-BFD50179D6AC
    static Guid GptVmwareVmfs();      // AA31E02A-400F-11DB-9590-000C2911D1B8
    static Guid GptFreebsdBoot();     // 83BD6B9D-7F41-11DC-BE0B-001560B84F0F
};

}  // namespace df
