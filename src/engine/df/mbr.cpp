#include "mbr.h"

#include "diskio.h"

namespace df {

MbrTable MbrTable::Parse(const uint8_t* s) {
    MbrTable t;
    t.signatureOk = Rd16(s + 510) == kMbrSignature;
    for (int i = 0; i < 4; ++i) {
        const uint8_t* e = s + 446 + i * 16;
        t.entries[i].status = e[0];
        t.entries[i].type = e[4];
        t.entries[i].startLba = Rd32(e + 8);
        t.entries[i].sectors = Rd32(e + 12);
    }
    return t;
}

void MbrTable::Write(uint8_t* sector) const {
    std::memset(sector, 0, kSectorSize);
    for (int i = 0; i < 4; ++i) {
        uint8_t* e = sector + 446 + i * 16;
        e[0] = entries[i].status;
        e[4] = entries[i].type;
        Wr32(e + 8, entries[i].startLba);
        Wr32(e + 12, entries[i].sectors);
        if (!entries[i].IsUnused()) {
            mbr::EncodeChs(e + 1, entries[i].startLba, 255, 63);
            mbr::EncodeChs(e + 5, entries[i].startLba + entries[i].sectors - 1, 255, 63);
        }
    }
    Wr16(sector + 510, kMbrSignature);
}

bool MbrTable::IsProtective() const {
    int ee = 0, used = 0;
    for (auto& e : entries) {
        if (e.IsUnused()) continue;
        ++used;
        if (e.type == 0xEE) ++ee;
    }
    return ee == 1 && used == 1;
}

MbrTable MbrTable::MakeProtective(uint32_t diskSectors) {
    MbrTable t = MakeEmpty();
    t.entries[0].status = 0;
    t.entries[0].type = 0xEE;
    t.entries[0].startLba = 1;
    // Clamp to uint32 max
    t.entries[0].sectors = diskSectors > 1 ? (diskSectors - 1 > 0xFFFFFFFFu ? 0xFFFFFFFFu : diskSectors - 1) : 1;
    return t;
}

MbrTable MbrTable::MakeEmpty() {
    MbrTable t;
    for (auto& e : t.entries) e = MbrEntry{};
    return t;
}

namespace mbr {

bool IsHiddenType(uint8_t t) { return (t & 0xF0) == 0x10 && (t & 0x0F) != 0; }

uint8_t HideType(uint8_t t) {
    if (IsHiddenType(t)) return t;
    return uint8_t(t | 0x10);
}

uint8_t UnhideType(uint8_t t) {
    if (!IsHiddenType(t)) return t;
    return uint8_t(t & 0x0F);
}

const char* TypeName(uint8_t t) {
    switch (t) {
        case 0x01: return "FAT12";
        case 0x04: return "FAT16 <32MB";
        case 0x05: return "Extended (CHS)";
        case 0x06: return "FAT16B";
        case 0x07: return "NTFS / exFAT";
        case 0x0B: return "FAT32 (CHS)";
        case 0x0C: return "FAT32 (LBA)";
        case 0x0E: return "FAT16 (LBA)";
        case 0x0F: return "Extended (LBA)";
        case 0x11: return "Hidden FAT12";
        case 0x16: return "Hidden FAT16B";
        case 0x17: return "Hidden NTFS";
        case 0x1B: return "Hidden FAT32";
        case 0x1C: return "Hidden FAT32 LBA";
        case 0x27: return "Windows RE";
        case 0x82: return "Linux swap";
        case 0x83: return "Linux";
        case 0x85: return "Linux extended";
        case 0x8E: return "Linux LVM";
        case 0xEE: return "GPT protective";
        default: break;
    }
    return "Unknown";
}

void EncodeChs(uint8_t out[3], uint64_t lba, uint32_t heads, uint32_t spt) {
    if (spt == 0) spt = 63;
    if (heads == 0) heads = 255;
    uint64_t c = lba / (uint64_t(spt) * heads);
    uint64_t rem = lba % (uint64_t(spt) * heads);
    uint64_t h = rem / spt;
    uint64_t s = rem % spt + 1;
    if (c > 1023) { c = 1023; h = heads - 1; s = spt; }
    out[0] = uint8_t(h);
    out[1] = uint8_t((c >> 2) | ((s >> 8) & 3));
    out[2] = uint8_t(s);
}

}  // namespace mbr

std::vector<LogicalPart> WalkEbrChain(IDiskDevice& dev, uint32_t extendedLba) {
    std::vector<LogicalPart> out;
    std::vector<uint32_t> visited;
    uint32_t cur = extendedLba;
    Buffer sec(kSectorSize);
    int guard = 0;
    while (cur && guard++ < 128) {
        bool seen = false;
        for (auto v : visited) if (v == cur) seen = true;
        if (seen) break;
        visited.push_back(cur);

        dev.Read(cur, sec.data(), 1);
        MbrTable t = MbrTable::Parse(sec.data());
        if (!t.signatureOk) break;
        // Entry 0: this link's data partition (relative to EBR LBA)
        if (!t.entries[0].IsUnused() && !t.entries[0].IsExtended()) {
            LogicalPart lp;
            lp.ebrLba = cur;
            lp.entry = t.entries[0];
            lp.entry.startLba = cur + t.entries[0].startLba;  // make absolute
            out.push_back(lp);
        }
        // Entry 1: next EBR (absolute or relative depending on tool; spec says relative to extended start)
        if (t.entries[1].IsExtended()) {
            uint32_t next = extendedLba + t.entries[1].startLba;
            if (next <= cur) break;  // malformed/backwards — stop
            cur = next;
        } else {
            break;
        }
    }
    return out;
}

void RewriteEbrChain(IDiskDevice& dev, uint32_t extendedLba, const std::vector<LogicalPart>& links) {
    if (links.empty()) {
        // Zero the first EBR except keep protective signature? Deleting all logical
        // partitions: caller removes the extended partition entry itself.
        return;
    }
    Buffer sec(kSectorSize);
    for (size_t i = 0; i < links.size(); ++i) {
        std::memset(sec.data(), 0, kSectorSize);
        uint8_t* s = sec.data();
        // data entry (relative to this EBR)
        Wr16(s + 510, kMbrSignature);
        uint8_t* e0 = s + 446;
        e0[0] = 0;
        e0[4] = links[i].entry.type;
        Wr32(e0 + 8, links[i].entry.startLba - links[i].ebrLba);
        Wr32(e0 + 12, links[i].entry.sectors);
        mbr::EncodeChs(e0 + 1, links[i].entry.startLba, 255, 63);
        mbr::EncodeChs(e0 + 5, links[i].entry.startLba + links[i].entry.sectors - 1, 255, 63);
        if (i + 1 < links.size()) {
            uint8_t* e1 = s + 462;
            e1[0] = 0;
            e1[4] = 0x05;
            Wr32(e1 + 8, links[i + 1].ebrLba - extendedLba);
            Wr32(e1 + 12, 63);  // nominal size of next EBR link region
        }
        dev.Write(links[i].ebrLba, sec.data(), 1);
    }
    // Zero trailing EBR of the previous chain if chain got shorter is handled by caller.
}

uint32_t AppendLogicalToChain(IDiskDevice& dev, uint32_t extendedLba,
                              uint32_t dataStartLba, uint32_t sectors, uint8_t type) {
    auto links = WalkEbrChain(dev, extendedLba);
    if (links.empty()) {
        // First logical partition: EBR at extendedLba
        LogicalPart lp;
        lp.ebrLba = extendedLba;
        lp.entry.type = type;
        lp.entry.startLba = dataStartLba;
        lp.entry.sectors = sectors;
        std::vector<LogicalPart> one{lp};
        RewriteEbrChain(dev, extendedLba, one);
        return extendedLba;
    }
    // Place new EBR just before dataStartLba (caller guarantees alignment/free)
    uint32_t newEbr = dataStartLba > kAlignSectors ? uint32_t(dataStartLba - kAlignSectors) : dataStartLba - 1;
    // Keep EBR inside the extended region and not overlapping last link
    LogicalPart& last = links.back();
    if (newEbr <= last.entry.startLba + last.entry.sectors - 1) newEbr = last.entry.startLba + last.entry.sectors;

    LogicalPart lp;
    lp.ebrLba = newEbr;
    lp.entry.type = type;
    lp.entry.startLba = dataStartLba;
    lp.entry.sectors = sectors;
    links.push_back(lp);
    RewriteEbrChain(dev, extendedLba, links);
    return newEbr;
}

}  // namespace df
