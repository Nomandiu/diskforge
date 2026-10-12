// Unified partition layout model (MBR + GPT) with free-extent computation
#pragma once

#include <optional>
#include <vector>

#include "common.h"
#include "gpt.h"
#include "mbr.h"

namespace df {

enum class DiskStyle { Blank, Mbr, Gpt };

enum class PartKind {
    Basic, Esp, Msr, Linux, LinuxSwap, WindowsRe, Extended, Logical, BiosBoot, Unknown
};

// One entry of a parsed layout (allocated or unallocated).
struct PartInfo {
    Lba start = 0;
    uint64_t sectors = 0;

    // Identity (negative/zero when not yet present on disk)
    int number = 0;                 // windows partition number when known

    // MBR fields
    uint8_t mbrType = 0;
    bool boot = false;              // active flag
    bool isLogical = false;         // inside extended partition
    uint32_t ebrLba = 0;            // hosting EBR (for logical)

    // GPT fields
    Guid gptType, gptId;
    uint64_t gptAttrs = 0;
    std::u16string gptName;

    // Probed at runtime
    std::string fs;                 // NTFS / FAT32 / FAT16 / exFAT / "" (raw)
    std::string label;
    char letter = 0;                // mounted drive letter or 0
    uint64_t usedBytes = 0;         // 0 when unknown
    uint64_t freeBytes = 0;         // 0 when unknown

    PartKind Kind() const;
    const char* KindName() const;
    bool IsUnallocated() const { return sectors == 0; }
    Lba End() const { return start + sectors; }
};

struct FreeExtent {
    Lba start;
    uint64_t sectors;
    Lba End() const { return start + sectors; }
};

struct DiskLayout {
    DiskStyle style = DiskStyle::Blank;
    Lba totalSectors = 0;
    uint32_t sectorSize = kSectorSize;
    Guid diskGuid;                  // GPT only
    uint32_t mbrSignature = 0;      // MBR only
    std::vector<PartInfo> parts;    // allocated partitions (sorted by start)
    uint32_t extendedLba = 0;       // MBR extended partition start (0 if none)

    std::vector<FreeExtent> FreeExtents(uint64_t alignSectors = kAlignSectors) const;
    // Find a free slot of `wantSectors` aligned to `alignSectors`, preferring a slot
    // after `afterLba` when given. Returns nullopt if none.
    std::optional<FreeExtent> FindSlot(uint64_t wantSectors, Lba afterLba = 0,
                                       uint64_t alignSectors = kAlignSectors) const;
    bool OverlapsAny(Lba start, uint64_t sectors) const;
    const PartInfo* FindByStart(Lba start) const;

    // Raw table readers (no volume enrichment). Throws on IO errors only.
    static DiskLayout ReadFromTables(IDiskDevice& dev);

    // Validate + apply this layout back to raw tables (GPT full rewrite / MBR primary+EBR).
    // Careful: production Windows code should prefer the ioctl-based ops layer; this raw
    // writer exists for tests, recovery restore, and offline images.
    void WriteToTables(IDiskDevice& dev) const;
};

// Ensure `start` and `sectors` are `align`-aligned; returns adjusted values.
std::pair<Lba, uint64_t> AlignRange(Lba start, uint64_t sectors, uint64_t align);

}  // namespace df
