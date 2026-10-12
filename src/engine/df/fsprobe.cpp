#include "fsprobe.h"

#include <cstring>

#include "diskio.h"

namespace df {

static bool LooksLikeNtfs(const uint8_t* s) { return std::memcmp(s + 3, "NTFS    ", 8) == 0; }

FsProbeResult ProbeFs(IDiskDevice& dev, Lba partStart, uint64_t partSectors) {
    FsProbeResult r;
    Buffer s(kSectorSize);
    try {
        dev.Read(partStart, s.data(), 1);
    } catch (const DfError&) {
        return r;
    }

    // ---- NTFS ----
    if (LooksLikeNtfs(s.data())) {
        uint16_t bps = Rd16(s.data() + 11);
        uint8_t spc = s[13];
        if (bps == 0 || (bps & (bps - 1)) || spc == 0) return r;
        uint64_t total = Rd64(s.data() + 0x28);
        r.fs = "NTFS";
        r.bytesPerSector = bps;
        r.sectorsPerCluster = spc;
        r.totalSectorsFs = total;
        r.serial = Rd32(s.data() + 0x48);
        // NTFS label lives in $Volume; label left empty here (ntfs.cpp fills it).
        // Validate: MFT LCN reasonable
        uint64_t mftLcn = Rd64(s.data() + 0x30);
        uint64_t clusters = total / spc;
        if (mftLcn >= clusters) { r.fs = "NTFS?"; return r; }
        // Backup boot sector at end of volume?
        if (partSectors >= 2) {
            try {
                Buffer b(kSectorSize);
                dev.Read(partStart + total - 1, b.data(), 1);
                if (LooksLikeNtfs(b.data()) && Rd64(b.data() + 0x28) == total &&
                    Rd64(b.data() + 0x30) == mftLcn)
                    r.backupBootMatch = true;
            } catch (const DfError&) { /* leave false */ }
        }
        return r;
    }

    // ---- exFAT ----
    if (std::memcmp(s.data() + 3, "EXFAT ", 7) == 0) {
        r.fs = "exFAT";
        r.bytesPerSector = 1u << s.data()[0x6C];
        r.sectorsPerCluster = 1u << s.data()[0x6D];
        r.serial = Rd32(s.data() + 0x64);
        uint64_t total = Rd64(s.data() + 0x48);
        r.totalSectorsFs = total;
        return r;
    }

    // ---- FAT family ----
    uint16_t bps = Rd16(s.data() + 11);
    uint8_t spc = s[13];
    if (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) return r;
    if (spc == 0 || (spc & (spc - 1))) return r;
    uint16_t rsvd = Rd16(s.data() + 14);
    uint8_t nfats = s[16];
    if (rsvd == 0 || nfats == 0 || nfats > 4) return r;
    uint16_t rootEnt = Rd16(s.data() + 17);
    uint16_t tot16 = Rd16(s.data() + 19);
    uint32_t tot32 = Rd32(s.data() + 32);
    uint32_t total = tot16 ? tot16 : tot32;
    if (total == 0) return r;
    uint16_t fatSz16 = Rd16(s.data() + 22);
    uint32_t fatSz32 = Rd32(s.data() + 36);
    uint32_t fatSz = fatSz16 ? fatSz16 : fatSz32;
    if (fatSz == 0) return r;
    uint32_t rootSectors = (uint32_t(rootEnt) * 32 + bps - 1) / bps;
    uint64_t dataSectors = uint64_t(total) - rsvd - uint64_t(nfats) * fatSz - rootSectors;
    if (dataSectors <= 0 || dataSectors > total) return r;
    uint64_t clusters = dataSectors / spc;

    uint32_t media = s[21];
    if (media < 0xE5 && media != 0x01 && media != 0xF0) return r;
    const bool isFat32 = (fatSz16 == 0);
    if (isFat32 && clusters < 65525) return r;  // FAT32 must have >= 65525 clusters
    if (!isFat32 && clusters >= 65525) return r;
    // FAT[0] sanity: media byte + fill pattern
    try {
        Buffer fat1(kSectorSize);
        dev.Read(partStart + rsvd, fat1.data(), 1);
        if (isFat32) {
            uint32_t f0 = Rd32(fat1.data());
            if ((f0 & 0xFFFFFF00u) != 0xFFFFFF00u) return r;
        } else if (clusters >= 4085) {
            uint16_t f0 = Rd16(fat1.data());
            if ((f0 & 0xFF00) != 0xFF00) return r;
        } else {
            uint16_t f0 = Rd16(fat1.data());
            if ((f0 & 0xF000) != 0xF000) return r;
        }
    } catch (const DfError&) { return r; }

    if (isFat32) {
        r.fs = "FAT32";
        char label[12];
        std::memcpy(label, s.data() + 71, 11);
        label[11] = 0;
        std::string l(label);
        // trim trailing spaces
        while (!l.empty() && l.back() == ' ') l.pop_back();
        if (Rd8(s.data() + 66) == 0x29) {
            r.label = l;
            r.serial = Rd32(s.data() + 67);
        }
    } else if (clusters >= 4085) {
        r.fs = "FAT16";
        if (Rd8(s.data() + 38) == 0x28 || Rd8(s.data() + 38) == 0x29) {
            char label[12];
            std::memcpy(label, s.data() + 43, 11);
            label[11] = 0;
            std::string l(label);
            while (!l.empty() && l.back() == ' ') l.pop_back();
            r.label = l;
            r.serial = Rd32(s.data() + 39);
        }
    } else {
        r.fs = "FAT12";
    }
    r.bytesPerSector = bps;
    r.sectorsPerCluster = spc;
    r.totalSectorsFs = total;
    return r;
}

void PatchBootHiddenSectors(IDiskDevice& dev, Lba partStart, uint64_t partSectors,
                            const std::string& fs, uint32_t newHiddenSectors) {
    Buffer s(kSectorSize);
    dev.Read(partStart, s.data(), 1);
    Wr32(s.data() + kBpbHiddenSectorsOffset, newHiddenSectors);
    dev.Write(partStart, s.data(), 1);
    if (fs == "FAT32" || fs == "FAT16" || fs == "FAT12") {
        // FAT32 has a backup boot sector at offset 6 (BPB field bkBootSec)
        uint16_t bk = Rd16(s.data() + 50);  // only valid on FAT32; harmless if 0
        if (fs == "FAT32" && bk != 0 && bk != 0xFFFF) {
            dev.Write(partStart + bk, s.data(), 1);
        }
    } else if (fs == "NTFS" && partSectors >= 2) {
        // Backup boot sector is the last sector of the volume.
        dev.Write(partStart + partSectors - 1, s.data(), 1);
    }
}

}  // namespace df
