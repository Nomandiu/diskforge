// FAT12/16/32 volume: parse, enumerate (incl. deleted), read, format (FAT32),
// offline resize (FAT32) with cluster compaction + region move.
#pragma once

#include <functional>
#include <vector>

#include "common.h"
#include "diskio.h"
#include "units.h"

namespace df {

struct FatEntry {
    std::string name;          // display name (LFN when present)
    std::string path;          // full path inside volume, e.g. "/DOCS/report.txt"
    bool isDir = false;
    bool deleted = false;
    uint32_t firstCluster = 0;
    uint64_t size = 0;
    uint16_t dosDate = 0;
    uint16_t dosTime = 0;
    bool chainOrphaned = false;  // deleted entry whose chain head looks reused
};

class FatVolume {
public:
    // Throws DfError when the volume at partStart is not a valid FAT volume.
    static FatVolume Open(IDiskDevice* dev, Lba partStart, uint64_t partSectors);
    static bool Detect(IDiskDevice& dev, Lba partStart);

    // ---- info ----
    int FatBits() const { return fatBits_; }
    bool IsFat32() const { return fatBits_ == 32; }
    std::string FsName() const;
    std::string Label() const { return label_; }
    void SetLabel(const std::string& label);
    uint32_t Serial() const { return serial_; }
    uint64_t TotalBytes() const { return totalSectors_ * bps_; }
    uint64_t ClusterBytes() const { return uint64_t(spc_) * bps_; }
    uint64_t ClusterCount() const { return clusterCount_; }
    uint64_t UsedBytes() const;
    uint64_t FreeBytes() const;
    Lba PartStart() const { return partStart_; }
    uint64_t PartSectors() const { return totalSectors_; }

    // ---- enumeration / recovery ----
    // Enumerate the whole tree. includeDeleted surfaces deleted entries (recovery).
    std::vector<FatEntry> Enumerate(bool includeDeleted, const Progress& prog = {}) const;

    // Stream file content. For deleted entries, falls back to the
    // "contiguous clusters up to recorded size" heuristic.
    void ReadFileData(const FatEntry& e, const std::function<void(const uint8_t*, size_t)>& sink) const;

    // ---- mutation (tests, restore-to-FAT, format) ----
    static void FormatFat32(IDiskDevice& dev, Lba partStart, uint64_t partSectors,
                            const std::string& label, uint32_t clusterSize = 0);

    void AddDir(const std::string& parentPath, const std::string& name);
    void AddFile(const std::string& parentPath, const std::string& name,
                 const uint8_t* data, size_t len);
    void DeleteFile(const std::string& path);   // 0xE5 + free chain

    // ---- resize (FAT32, offline: volume must be dismounted) ----
    uint64_t MinSectors() const;
    void Resize(uint64_t newSectors, const Progress& prog);
    // Relocate allocated clusters below `limit` (monotone compaction). Test hook.
    void CompactTo(uint64_t newClusterCount, const Progress& prog);

    // ---- misc ----
    uint32_t NextFreeCluster() const;
    uint32_t FreeClusterCount() const;
    uint32_t FatGetPublicForTest(uint32_t cluster) const { return FatGet(cluster); }
    Lba ClusterToLba(uint32_t cluster) const {
        return partStart_ + dataStartSector_ + (uint64_t(cluster) - 2) * spc_;
    }

private:
    FatVolume() = default;

    uint32_t FatGet(uint32_t cluster) const;
    void FatSet(uint32_t cluster, uint32_t value);
    void FlushFat() const;

    std::vector<uint32_t> ChainOf(uint32_t head, bool* intact = nullptr) const;
    bool FollowsChain(const FatEntry& e) const;

    struct DirPos {
        uint32_t dirCluster = 0;   // cluster holding the dirent (0 = FAT12/16 root, sector-encoded offset)
        uint32_t offset = 0;       // byte offset (cluster-relative, or s*bps+inSec for root16)
    };
    // Find directory chain by path ("/" = root). Returns empty chain when missing.
    std::vector<uint32_t> DirChainOf(const std::string& path) const;
    // Find `slots` consecutive free dirents; returns position of the LAST slot.
    DirPos AllocDirEntry(const std::vector<uint32_t>& chain, int slots = 1);
    uint32_t AllocChain(uint64_t bytes, std::vector<uint32_t>& chain);
    void FreeChain(uint32_t head);
    void ReadDirBuf(const DirPos& pos, uint8_t* buf, uint64_t bufLen);
    void WriteDirBuf(const DirPos& pos, const uint8_t* buf);

    void ReadCluster(uint32_t cluster, uint8_t* buf) const;
    void WriteCluster(uint32_t cluster, const uint8_t* buf);

    void WriteFsInfo(uint64_t freeCount, uint32_t nextFree);
    void ParseBoot();

    IDiskDevice* dev_ = nullptr;
    Lba partStart_ = 0;
    uint16_t bps_ = 512;
    uint8_t spc_ = 1;
    uint16_t rsvd_ = 1;
    uint8_t nfats_ = 2;
    uint32_t fatSz_ = 0;          // sectors per FAT
    uint64_t totalSectors_ = 0;
    uint64_t dataStartSector_ = 0;
    uint64_t clusterCount_ = 0;
    uint32_t rootClus_ = 2;       // FAT32
    uint16_t rootEnt_ = 0;        // FAT12/16
    uint64_t rootDirSectors_ = 0; // FAT12/16
    uint16_t fsInfoSec_ = 0;
    uint16_t bkBootSec_ = 0;
    int fatBits_ = 32;
    std::string label_;
    uint32_t serial_ = 0;

    // mutable FAT window cache
    struct FatPage {
        uint64_t startEntry = UINT64_MAX;
        std::vector<uint8_t> data;
        bool dirty = false;
    };
    mutable std::vector<FatPage> fatCache_;
    mutable std::vector<uint32_t> chainEpoch_;
    mutable uint32_t curEpoch_ = 0;
    FatPage* LoadFatPage(uint64_t entryIndex) const;
};

// ---- shared low-level helpers ---------------------------------------------
uint32_t Fat32CalcFatSectors(uint64_t dataSectors, uint32_t spc);  // fixed point solve
uint8_t LfnChecksum(const uint8_t sfn[11]);
void SfnFromName(const std::string& name, uint8_t out[11]);  // 8.3 uppercase + ~N tilde

}  // namespace df
