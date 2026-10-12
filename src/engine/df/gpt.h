// GPT header / entry array: parse, validate CRC, write primary + backup, repair
#pragma once

#include <vector>

#include "common.h"
#include "guid.h"

namespace df {

class IDiskDevice;  // fwd — see diskio.h

constexpr uint32_t kGptHeaderSize = 92;
constexpr uint32_t kGptEntriesDefault = 128;
constexpr uint32_t kGptEntrySize = 128;
constexpr uint64_t kGptFirstUsableLba = 34;  // with default entry array

struct GptEntry {
    Guid type;                 // zero type = unused
    Guid id;
    uint64_t firstLba = 0;     // inclusive
    uint64_t lastLba = 0;      // INCLUSIVE
    uint64_t attrs = 0;
    std::u16string name;       // up to 36 UTF-16 units

    bool IsUnused() const { return type.IsZero(); }
    uint64_t Sectors() const { return lastLba >= firstLba ? (lastLba - firstLba + 1) : 0; }

    void Parse(const uint8_t* p);
    void Write(uint8_t* p) const;

    // Attribute bits
    static constexpr uint64_t kAttrSystemCritical = 0x1;
    static constexpr uint64_t kAttrReadOnly = 1ull << 60;
    static constexpr uint64_t kAttrShadowCopy = 1ull << 61;
    static constexpr uint64_t kAttrHidden = 1ull << 62;
    static constexpr uint64_t kAttrNoDriveLetter = 1ull << 63;
};

struct GptHeader {
    char signature[8] = {};          // "EFI PART"
    uint32_t revision = 0x00010000;
    uint32_t headerSize = kGptHeaderSize;
    uint32_t crc = 0;
    uint64_t currentLba = 0;
    uint64_t backupLba = 0;
    uint64_t firstUsableLba = kGptFirstUsableLba;
    uint64_t lastUsableLba = 0;
    Guid diskGuid;
    uint64_t entriesLba = 2;
    uint32_t numEntries = kGptEntriesDefault;
    uint32_t entrySize = kGptEntrySize;
    uint32_t entriesCrc = 0;

    void Parse(const uint8_t* p);
    void Write(uint8_t* p) const;
    bool ValidateSignature() const;
};

// Full on-disk GPT state.
struct GptTable {
    GptHeader primary, backup;
    std::vector<GptEntry> entries;   // numEntries entries
    bool primaryValid = false;       // header CRC + entries CRC ok
    bool backupValid = false;

    // Read both copies; flags report validity (corrupt data does not throw).
    static GptTable Read(IDiskDevice& dev);

    // Rebuild entries CRC and write entries + both headers to disk.
    // `totalSectors` defines backup header location (last LBA).
    void Write(IDiskDevice& dev, uint64_t totalSectors);

    // Recompute entries CRC into both headers.
    void RefreshCrc();

    // Build a fresh empty table for a disk of `totalSectors`.
    static GptTable CreateEmpty(uint64_t totalSectors);
};

const char* GptTypeName(const Guid& type);

}  // namespace df
