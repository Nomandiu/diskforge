// MBR (including EBR chains for logical partitions inside extended partitions)
#pragma once

#include "common.h"

namespace df {

struct MbrEntry {
    uint8_t status = 0;      // 0x80 bootable
    uint8_t type = 0;        // 0x00 unused, 0x05/0x0F extended
    uint32_t startLba = 0;   // absolute for primary, absolute EBR-relative for logical
    uint32_t sectors = 0;

    bool IsUnused() const { return type == 0; }
    bool IsExtended() const { return type == 0x05 || type == 0x0F || type == 0x85; }
    bool IsBootable() const { return (status & 0x80) != 0; }
};

constexpr uint16_t kMbrSignature = 0xAA55;

struct MbrTable {
    MbrEntry entries[4];
    bool signatureOk = false;

    static MbrTable Parse(const uint8_t* sector);
    void Write(uint8_t* sector) const;

    // True if this looks like a protective MBR for GPT (single 0xEE entry)
    bool IsProtective() const;
    static MbrTable MakeProtective(uint32_t diskSectors);
    static MbrTable MakeEmpty();
};

// Well-known MBR partition type bytes
namespace mbr {
constexpr uint8_t kFAT12 = 0x01;
constexpr uint8_t kFAT16Small = 0x04;
constexpr uint8_t kExtendedChs = 0x05;
constexpr uint8_t kFAT16Bigs = 0x06;
constexpr uint8_t kNTFSorIfs = 0x07;     // NTFS / exFAT / IFS
constexpr uint8_t kFAT32Chs = 0x0B;
constexpr uint8_t kFAT32Lba = 0x0C;
constexpr uint8_t kFAT16Lba = 0x0E;
constexpr uint8_t kExtendedLba = 0x0F;
constexpr uint8_t kExtendedLinux = 0x85;
constexpr uint8_t kLinux = 0x83;
constexpr uint8_t kLinuxSwap = 0x82;
constexpr uint8_t kLvm = 0x8E;
constexpr uint8_t kWindowsRe = 0x27;
constexpr uint8_t kHiddenNTFS = 0x17;
constexpr uint8_t kHiddenFAT32Lba = 0x1C;

bool IsHiddenType(uint8_t t);
uint8_t HideType(uint8_t t);
uint8_t UnhideType(uint8_t t);
const char* TypeName(uint8_t t);

// CHS encode/decode (max 1023/254/63 clamp)
void EncodeChs(uint8_t out[3], uint64_t lba, uint32_t heads, uint32_t sectorsPerTrack);
}  // namespace mbr

// Logical partition view inside an extended partition chain.
struct LogicalPart {
    uint32_t ebrLba;         // LBA of the EBR that hosts this link
    MbrEntry entry;          // type/start(absolute)/sectors
};

// Walk an EBR chain starting at the extended partition LBA. Returns links.
// Tolerates malformed chains: stops at first invalid link (returned separately).
std::vector<LogicalPart> WalkEbrChain(class IDiskDevice& dev, uint32_t extendedLba);

// Append a new logical partition to an EBR chain (writes EBR sector).
// Returns EBR LBA used.
uint32_t AppendLogicalToChain(IDiskDevice& dev, uint32_t extendedLba,
                              uint32_t dataStartLba, uint32_t sectors, uint8_t type);

// Rewrite the entire chain for `links` (used when deleting/reordering).
void RewriteEbrChain(IDiskDevice& dev, uint32_t extendedLba, const std::vector<LogicalPart>& links);

}  // namespace df
