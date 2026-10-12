#include "scan.h"

#include <algorithm>
#include <unordered_set>

#include "fat.h"
#include "fsprobe.h"
#include "gpt.h"
#include "ntfs.h"

namespace df {

namespace {

bool ProbeCandidate(IDiskDevice& dev, Lba start, uint64_t diskSectors, LostPartition* out) {
    FsProbeResult fs = ProbeFs(dev, start, diskSectors > start ? diskSectors - start : 0);
    if (!fs.Ok() || fs.fs == "NTFS?" || fs.fs.empty()) return false;
    if (fs.fs != "NTFS" && fs.fs != "FAT32" && fs.fs != "FAT16" && fs.fs != "FAT12" && fs.fs != "exFAT")
        return false;
    uint64_t sectors = fs.totalSectorsFs;
    if (sectors < 1024 || start + sectors > diskSectors + 2048) return false;  // tolerate tiny overrun
    if (start + sectors > diskSectors) sectors = diskSectors - start;

    out->start = start;
    out->sectors = sectors;
    out->fs = fs.fs;
    out->label = fs.label;
    out->evidence = fs.fs + " boot sector";
    out->confidence = 0.55;
    out->backupBootMatch = fs.backupBootMatch;

    if (fs.fs == "NTFS") {
        out->confidence = 0.6;
        if (fs.backupBootMatch) {
            out->confidence = 0.95;
            out->evidence += " + matching backup boot sector (size confirmed)";
        }
        // verify $MFT is reachable
        try {
            NtfsVolume v = NtfsVolume::Open(&dev, start, sectors);
            out->label = v.Label();
            out->confidence = fs.backupBootMatch ? 0.97 : 0.8;
        } catch (const DfError&) {
            if (!fs.backupBootMatch) return false;  // boot sector without MFT = likely false positive
        }
    } else if (fs.fs == "FAT32") {
        try {
            FatVolume fv = FatVolume::Open(&dev, start, sectors);
            if (fv.Label().empty()) out->label = fs.label;
            out->confidence = 0.9;
            out->evidence += " + valid FAT structures";
        } catch (const DfError&) {
            return false;
        }
    } else {
        out->confidence = 0.75;
    }
    return true;
}

}  // namespace

ScanResult ScanLostPartitions(IDiskDevice& dev, const DiskLayout& current, ScanMode mode,
                              const Progress& prog) {
    ScanResult res;
    uint64_t total = dev.SizeSectors();

    // ---- GPT health ----
    if (current.style == DiskStyle::Gpt) {
        GptTable g = GptTable::Read(dev);
        res.gptPrimaryDamaged = !g.primaryValid;
        res.gptBackupDamaged = !g.backupValid;
        res.gptRestorePossible = g.primaryValid || g.backupValid;
    }

    // occupied ranges from the CURRENT layout
    struct Range {
        uint64_t a, b;
    };
    std::vector<Range> used;
    for (auto& p : current.parts)
        if (p.sectors) used.push_back({p.start, p.start + p.sectors});
    auto isFree = [&](Lba start, uint64_t sectors) {
        for (auto& r : used)
            if (start < r.b && r.a < start + sectors) return false;
        return true;
    };

    // ---- candidate iteration ----
    std::vector<uint64_t> quickCandidates;
    if (mode == ScanMode::Quick) {
        for (uint64_t s = 2048; s < total; s += kAlignSectors) quickCandidates.push_back(s);
        for (auto& e : current.FreeExtents(1)) quickCandidates.push_back(e.start);
        std::sort(quickCandidates.begin(), quickCandidates.end());
        quickCandidates.erase(std::unique(quickCandidates.begin(), quickCandidates.end()), quickCandidates.end());
    }
    const uint64_t candidateCount = mode == ScanMode::Quick ? quickCandidates.size() : (total ? total - 1 : 0);

    std::unordered_set<uint64_t> foundStarts;
    uint64_t processed = 0;
    auto tryCandidate = [&](uint64_t start) {
        if (start == 0 || start + 1024 > total) return;
        // exact starts of live partitions are not "lost"
        for (auto& p : current.parts)
            if (p.start == start) return;
        if (((++processed) & 0xFFF) == 0) {
            prog.Report(mode == ScanMode::Quick ? "Quick scanning" : "Deep scanning",
                        double(processed) / double(candidateCount ? candidateCount : 1));
            prog.CheckCancel();
        }

        LostPartition lp;
        if (!ProbeCandidate(dev, start, total, &lp)) return;
        if (foundStarts.count(lp.start)) return;

        // alignment check: real partitions are almost always 1 MiB aligned
        if (lp.start % kAlignSectors != 0) lp.confidence *= 0.7;
        // overlap with existing?
        lp.overlapsExisting = !isFree(lp.start, lp.sectors);
        if (lp.overlapsExisting) lp.confidence *= 0.5;

        foundStarts.insert(lp.start);
        res.lost.push_back(lp);

        // in Quick mode, skip past this partition
        if (mode == ScanMode::Quick) used.push_back({lp.start, lp.start + lp.sectors});
    };

    if (mode == ScanMode::Quick) {
        for (uint64_t start : quickCandidates) tryCandidate(start);
    } else {
        for (uint64_t s = 1; s < total; ++s) tryCandidate(s);
    }

    std::sort(res.lost.begin(), res.lost.end(),
              [](const LostPartition& a, const LostPartition& b) { return a.start < b.start; });
    prog.Report("Scan complete", 1.0);
    return res;
}

}  // namespace df
