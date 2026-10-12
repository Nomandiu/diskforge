// Filesystem probe: identify FS at a partition start, read label/serial/geometry hints
#pragma once

#include "common.h"

namespace df {

class IDiskDevice;  // fwd — see diskio.h

struct FsProbeResult {
    std::string fs;        // "", "NTFS", "FAT12", "FAT16", "FAT32", "exFAT"
    std::string label;     // volume label when available
    uint32_t serial = 0;
    uint32_t bytesPerSector = 0;
    uint32_t sectorsPerCluster = 0;
    uint64_t totalSectorsFs = 0;   // FS-reported volume size in sectors
    bool backupBootMatch = false;  // NTFS: backup boot sector at end matches

    bool Ok() const { return !fs.empty(); }
};

FsProbeResult ProbeFs(IDiskDevice& dev, Lba partStart, uint64_t partSectors);

// Offset of "HiddenSectors" (sectors preceding FS) in a FAT/NTFS BPB: byte 28 (FAT) and
// byte 28 for NTFS too (both classic BPB layouts).
constexpr size_t kBpbHiddenSectorsOffset = 28;

// Patch the hidden-sectors (partition-relative offset) field in a boot sector AND its
// copies (FAT32 backup boot at +6; NTFS backup boot at partition end - 1 sector).
void PatchBootHiddenSectors(IDiskDevice& dev, Lba partStart, uint64_t partSectors,
                            const std::string& fs, uint32_t newHiddenSectors);

}  // namespace df
