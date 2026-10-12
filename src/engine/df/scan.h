// Lost partition scanning: find deleted/overwritten-table partitions by FS signatures
#pragma once

#include <vector>

#include "common.h"
#include "diskio.h"
#include "layout.h"
#include "units.h"

namespace df {

enum class ScanMode {
    Quick,   // 1 MiB grid + free-extent starts + GPT backup structures
    Full,    // every sector (deep scan)
};

struct LostPartition {
    Lba start = 0;
    uint64_t sectors = 0;
    std::string fs;
    std::string label;
    double confidence = 0;      // 0..1
    std::string evidence;
    bool overlapsExisting = false;
    bool backupBootMatch = false;  // NTFS: backup boot sector at end matches
};

struct ScanResult {
    std::vector<LostPartition> lost;
    // GPT health report (when disk is GPT)
    bool gptPrimaryDamaged = false;
    bool gptBackupDamaged = false;
    bool gptRestorePossible = false;  // at least one copy + entries are intact
};

// Scan the whole disk for recoverable partitions. `current` is the live layout
// (may itself be rebuilt from the healthy GPT copy when the primary is damaged).
ScanResult ScanLostPartitions(IDiskDevice& dev, const DiskLayout& current, ScanMode mode,
                              const Progress& prog);

}  // namespace df
