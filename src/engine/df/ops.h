// High-level disk operations service (implemented for Windows via ioctls in ops_win.cpp)
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "carve.h"
#include "common.h"
#include "diskio.h"
#include "fat.h"
#include "layout.h"
#include "ntfs.h"
#include "scan.h"
#include "units.h"

namespace df::ops {

// ---------------------------------------------------------------------------
// info types
// ---------------------------------------------------------------------------
struct VolumeInfo {
    char letter = 0;               // 0 when unmounted
    std::wstring volumeGuid;       // L"\\?\Volume{...}\"
    int disk = -1;
    Lba startLba = 0;
    uint64_t sectors = 0;
    std::string label;
    std::string fs;
    uint64_t totalBytes = 0;
    uint64_t freeBytes = 0;
    bool isSystem = false;         // hosts \Windows
    bool isPageFile = false;
    bool isEsp = false;            // EFI System Partition
    bool isBoot = false;
};

struct DiskInfo {
    int number = -1;
    std::string path;              // \\.\PhysicalDriveN
    uint64_t sizeBytes = 0;
    uint32_t sectorSize = 512;
    uint32_t physicalSectorSize = 512;
    std::string model;
    std::string serial;
    std::string busType;
    bool isRemovable = false;
    bool isSsd = false;
    bool readOnly = false;
    DiskLayout layout;
    std::vector<VolumeInfo> volumes;
};

// Filesystem choice constants
std::vector<std::string> SupportedFilesystems();   // {"NTFS","FAT32","exFAT"}

// ---------------------------------------------------------------------------
// enumeration
// ---------------------------------------------------------------------------
std::vector<DiskInfo> EnumerateDisks();            // throws DfError on hard failure
DiskInfo GetDisk(int diskNumber);

// ---------------------------------------------------------------------------
// partition management (positions identify partitions; all throw DfError)
// ---------------------------------------------------------------------------
void InitializeDisk(int disk, DiskStyle style, const Progress& prog = {});
// Create + format (fs "" => leave raw). Returns created partition info.
PartInfo CreatePartition(int disk, uint64_t startLba, uint64_t sectors,
                         const std::string& fs, const std::string& label,
                         char letter, const Progress& prog = {});
void DeletePartition(int disk, uint64_t partStart, const Progress& prog = {});
void FormatPartition(int disk, uint64_t partStart, const std::string& fs,
                     const std::string& label, bool quick, const Progress& prog = {});
void SetPartitionLabel(int disk, uint64_t partStart, const std::string& label);
void SetDriveLetter(int disk, uint64_t partStart, char newLetter);  // 0 = remove
void SetActivePartition(int disk, uint64_t partStart);              // MBR only
void SetHidden(int disk, uint64_t partStart, bool hidden);
void ChangeMbrTypeId(int disk, uint64_t partStart, uint8_t typeId);
void ChangeGptType(int disk, uint64_t partStart, const Guid& type);

// ---------------------------------------------------------------------------
// resize / move / composite
// ---------------------------------------------------------------------------
void ShrinkPartition(int disk, uint64_t partStart, uint64_t newSectors, const Progress& prog = {});
void ExtendPartition(int disk, uint64_t partStart, uint64_t newSectors, const Progress& prog = {});
void MovePartition(int disk, uint64_t partStart, uint64_t newStart, const Progress& prog = {});
void SplitPartition(int disk, uint64_t partStart, uint64_t shrinkToSectors,
                    const std::string& newFs, const std::string& newLabel, const Progress& prog = {});
void MergePartitions(int disk, uint64_t targetStart, uint64_t sourceStart, const Progress& prog = {});
// Move `bytes` from donor to target (slides intervening partitions as needed).
void AllocateFreeSpace(int disk, uint64_t donorStart, uint64_t targetStart, uint64_t bytes,
                       const Progress& prog = {});
// Find free space anywhere and extend target (the "1-click extend C" flow).
uint64_t OneClickExtend(int disk, uint64_t targetStart, uint64_t maxAddBytes, const Progress& prog = {});

// minimum-size helper for UI validation (FS-aware)
uint64_t PartitionMinSectors(int disk, uint64_t partStart);

// ---------------------------------------------------------------------------
// lost partition recovery
// ---------------------------------------------------------------------------
ScanResult ScanLostPartitions(int disk, ScanMode mode, const Progress& prog = {});
void RestoreLostPartition(int disk, const LostPartition& lp, const Progress& prog = {});
// Rebuild a damaged GPT primary from the backup copy (or vice versa)
void RepairGpt(int disk, const Progress& prog = {});

// ---------------------------------------------------------------------------
// file recovery session
// ---------------------------------------------------------------------------
struct RecoveryHit {
    std::string name;
    std::string path;          // in-volume path (deleted FS files) or "(carved)"
    std::string category;      // Document / Photo / Video / Audio / Archive / Other
    std::string typeName;
    std::string ext;
    uint64_t size = 0;
    bool deleted = false;
    bool orphaned = false;
    bool carved = false;
    bool sizeEstimated = false;

    // engine linkage
    int sourceKind = 0;        // 1 = NTFS, 2 = FAT, 3 = carved
    NtfsEntry ntfs;
    FatEntry fat;
    CarvedFile carvedFile;
};

struct RecoverStats {
    size_t recovered = 0;
    size_t failed = 0;
    uint64_t bytes = 0;
    std::vector<std::string> errors;
};

class RecoverySession {
public:
    // Scan a whole disk (optionally carving unallocated areas) or a partition.
    static std::unique_ptr<RecoverySession> Start(int disk, bool wholeDisk, uint64_t partStart,
                                                  bool includeDeleted, bool doCarve,
                                                  const CarveOptions& carveOpt, const Progress& prog);
    std::vector<RecoveryHit> Hits() const { return hits_; }
    RecoverStats Recover(const std::vector<RecoveryHit>& items, const std::string& destDir,
                         const Progress& prog);

private:
    int disk_ = -1;
    Lba partStart_ = 0;
    uint64_t partSectors_ = 0;
    bool hasNtfs_ = false;
    bool hasFat_ = false;
    std::unique_ptr<NtfsVolume> ntfs_;
    std::unique_ptr<FatVolume> fat_;
    std::unique_ptr<IDiskDevice> device_;
    std::vector<RecoveryHit> hits_;
};

}  // namespace df::ops
