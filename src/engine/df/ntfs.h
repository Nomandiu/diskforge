// NTFS volume: boot parse, $MFT walk with USA fixups, attribute/runlist decode,
// deleted-file recovery, $Bitmap usage, boot hidden-sectors fixup.
#pragma once

#include <functional>
#include <vector>

#include "common.h"
#include "diskio.h"
#include "units.h"

namespace df {

struct NtfsEntry {
    std::string name;
    std::string path;          // "/Windows/system32/..." or "/Lost Files/..."
    uint64_t mftRef = 0;       // record number
    uint64_t parentRef = 0;    // parent record number (48-bit)
    bool isDir = false;
    bool deleted = false;
    bool resident = false;     // $DATA resident in MFT record
    bool compressed = false;
    bool encrypted = false;
    bool sparse = false;
    bool hasData = false;
    uint64_t size = 0;         // real size of unnamed $DATA
    uint64_t allocSize = 0;
    uint64_t createUnix = 0;
    uint64_t modifyUnix = 0;
    bool orphaned = false;     // parent chain broken -> placed under Lost Files
};

class NtfsVolume {
public:
    static NtfsVolume Open(IDiskDevice* dev, Lba partStart, uint64_t partSectors);
    static bool Detect(IDiskDevice& dev, Lba partStart);

    uint64_t TotalSectors() const { return totalSectors_; }
    uint64_t ClusterBytes() const { return uint64_t(spc_) * bps_; }
    uint32_t SectorSize() const { return bps_; }
    uint64_t MftRecordCount() const { return mftRecordCount_; }
    std::string Label() const;   // lazily reads $Volume
    uint32_t Serial() const { return serial_; }
    uint64_t UsedBytes() const;         // via $Bitmap
    uint64_t FreeBytes() const;
    Lba PartStart() const { return partStart_; }

    std::vector<NtfsEntry> Enumerate(bool includeDeleted, const Progress& prog = {}) const;
    void ReadFileData(const NtfsEntry& e, const std::function<void(const uint8_t*, size_t)>& sink) const;

    // Update the "hidden sectors" field in boot + backup boot (after partition move).
    void FixupBootHiddenSectors(uint32_t newHiddenSectors);

    // ---- low-level access shared with tests / recovery ----
    bool ReadMftRecord(uint64_t index, Buffer& out) const;  // applies USA fixups

    struct Run {
        uint64_t vcn;      // starting VCN
        uint64_t lcn;      // starting LCN (UINT64_MAX = sparse)
        uint64_t count;    // clusters
    };
    // Decode a runlist into runs. Returns false on malformed data.
    static bool DecodeRunlist(const uint8_t* p, size_t len, std::vector<Run>& runs);

private:
    NtfsVolume() = default;

    bool ApplyFixups(Buffer& rec) const;   // in-place USA fixups; false on mismatch

    IDiskDevice* dev_ = nullptr;
    Lba partStart_ = 0;
    uint32_t bps_ = 512;
    uint32_t spc_ = 1;
    uint64_t totalSectors_ = 0;
    uint64_t mftLcn_ = 0;
    uint64_t mft2Lcn_ = 0;
    uint32_t recordSize_ = 1024;
    uint64_t mftRecordCount_ = 0;
    std::vector<Run> mftRuns_;     // $MFT $DATA runs (cluster granularity)
    mutable std::string label_;
    uint32_t serial_ = 0;

    // one-cluster cache for sequential MFT scans
    mutable uint64_t cachedVcn_ = UINT64_MAX;
    mutable Buffer cachedCluster_;
    bool ReadMftCluster(uint64_t vcn) const;  // fills cache_; false on sparse/IO error
};

// NTFS timestamp (100ns since 1601) -> unix seconds
inline uint64_t NtfsTimeToUnix(uint64_t t) {
    if (t == 0) return 0;
    int64_t v = int64_t(t) - 116444736000000000ll;
    return v <= 0 ? 0 : uint64_t(v) / 10000000ull;
}

}  // namespace df
