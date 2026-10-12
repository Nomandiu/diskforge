// Windows end-to-end integration test against a real VHD (run elevated in CI):
//   df-tests --integration \\.\PhysicalDriveN
#ifdef _WIN32

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "df/fat.h"
#include "df/ntfs.h"
#include "df/ops.h"
#include "df/scan.h"

using namespace df;
using namespace df::ops;

namespace {
int g_checks = 0;
#define ICHECK(cond)                                                                                       \
    do {                                                                                                   \
        if (!(cond)) {                                                                                     \
            std::printf("[FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond);                                   \
            return 1;                                                                                       \
        }                                                                                                  \
        ++g_checks;                                                                                        \
    } while (0)

std::string P(const PartInfo* p) { return p ? ("LBA " + std::to_string(p->start)) : "(null)"; }

const PartInfo* FindPart(const DiskInfo& di, uint64_t start) {
    for (auto& p : di.layout.parts)
        if (p.start == start) return &p;
    return nullptr;
}

bool WriteFileTo(const std::string& path, const std::vector<uint8_t>& data) {
    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    return ok;
}
std::vector<uint8_t> ReadFileFrom(const std::string& path) {
    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> v(size_t(n));
    size_t got = std::fread(v.data(), 1, v.size(), f);
    v.resize(got);
    std::fclose(f);
    return v;
}
std::vector<uint8_t> Pattern(size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    uint32_t x = seed ? seed : 1;
    for (size_t i = 0; i < n; ++i) {
        x = x * 1103515245 + 12345;
        v[i] = uint8_t((x >> 16) & 0xFF);
    }
    return v;
}
}  // namespace

int RunIntegration(const char* diskPath) {
    // disk number from path
    int disk = -1;
    if (std::sscanf(diskPath, "\\\\.\\PhysicalDrive%d", &disk) != 1 || disk < 0) {
        std::printf("invalid disk path: %s\n", diskPath);
        return 2;
    }
    std::printf("=== DiskForge integration test on %s ===\n", diskPath);
    Progress prog;

    // ---------- 1. enumerate ----------
    auto disks = EnumerateDisks();
    const DiskInfo* di = nullptr;
    for (auto& d : disks)
        if (d.number == disk) di = &d;
    ICHECK(di != nullptr);
    std::printf("[ok] enumerated %zu disks; target size=%.1f MiB\n", disks.size(), double(di->sizeBytes) / kMiB);
    uint64_t totalSectors = di->sizeBytes / kSectorSize;

    // ---------- 2. initialize GPT ----------
    if (di->layout.style == DiskStyle::Blank || di->layout.parts.empty()) {
        InitializeDisk(disk, DiskStyle::Gpt, prog);
        di = nullptr;
        for (auto& d : EnumerateDisks())
            if (d.number == disk) di = &d;
        ICHECK(di && di->layout.style == DiskStyle::Gpt);
        std::puts("[ok] GPT initialized");
    }

    // ---------- 3. create FAT32 partition (48 MiB) ----------
    uint64_t p1Start = 2048, p1Sec = 98304;  // 48 MiB
    // clean slate: delete existing partitions
    {
        DiskInfo cur = GetDisk(disk);
        for (auto& p : cur.layout.parts) {
            try {
                DeletePartition(disk, p.start, prog);
            } catch (const DfError& e) {
                std::printf("note: delete LBA %llu failed: %s\n", (unsigned long long)p.start, e.what());
            }
        }
    }
    PartInfo np = CreatePartition(disk, p1Start, p1Sec, "FAT32", "INTFAT", 0, prog);
    std::printf("[ok] partition created: %s letter=%c fs=%s\n", P(&np).c_str(), np.letter ? np.letter : '-',
                np.fs.c_str());
    DiskInfo d2 = GetDisk(disk);
    const PartInfo* p1 = FindPart(d2, p1Start);
    ICHECK(p1 != nullptr);
    ICHECK(p1->fs == "FAT32");
    ICHECK(p1->letter != 0);
    std::string root = std::string(1, p1->letter) + ":\\";
    ICHECK(std::filesystem::exists(root));

    // ---------- 4. write files through Windows ----------
    auto data1 = Pattern(2 * kMiB, 1);
    ICHECK(WriteFileTo(root + "test1.dat", data1));
    std::filesystem::create_directories(root + "docs");
    auto data2 = Pattern(512 * 1024, 2);
    ICHECK(WriteFileTo(root + "docs\\note.txt", data2));
    std::puts("[ok] files written via Win32");

    // ---------- 5. shrink by 8 MiB and verify ----------
    {
        // engine-side verification of the volume before/after
        WinPhysicalDrive dev(disk, false);
        FatVolume fv = FatVolume::Open(&dev, p1Start, p1Sec);
        uint64_t usedBefore = fv.UsedBytes();
        ICHECK(usedBefore >= 2 * kMiB);
    }
    ShrinkPartition(disk, p1Start, p1Sec - 16384, prog);
    {
        WinPhysicalDrive dev(disk, false);
        FatVolume fv = FatVolume::Open(&dev, p1Start, p1Sec - 16384);
        auto files = fv.Enumerate(false);
        bool t1 = false, t2 = false;
        for (auto& f : files) {
            if (f.path == "/test1.dat") t1 = true;
            if (f.path == "/docs/note.txt") t2 = true;
        }
        ICHECK(t1 && t2);
        for (auto& f : files) {
            if (f.path == "/test1.dat") {
                std::vector<uint8_t> got;
                fv.ReadFileData(f, [&](const uint8_t* p, size_t n) { got.insert(got.end(), p, p + n); });
                ICHECK(got == data1);
            }
        }
    }
    std::puts("[ok] shrink by 8 MiB; data intact (engine-verified)");

    // ---------- 6. extend back ----------
    ExtendPartition(disk, p1Start, p1Sec, prog);
    {
        WinPhysicalDrive dev(disk, false);
        FatVolume fv = FatVolume::Open(&dev, p1Start, p1Sec);
        auto files = fv.Enumerate(false);
        for (auto& f : files)
            if (f.path == "/test1.dat") {
                std::vector<uint8_t> got;
                fv.ReadFileData(f, [&](const uint8_t* p, size_t n) { got.insert(got.end(), p, p + n); });
                ICHECK(got == data1);
            }
    }
    std::puts("[ok] extended back to 48 MiB; data intact");

    // ---------- 7. create second partition (NTFS) + move ----------
    uint64_t p2Start = p1Start + p1Sec, p2Sec = 40960;  // 20 MiB
    CreatePartition(disk, p2Start, p2Sec, "NTFS", "INTNTFS", 0, prog);
    DiskInfo d3 = GetDisk(disk);
    const PartInfo* p2 = FindPart(d3, p2Start);
    ICHECK(p2 && p2->fs == "NTFS");
    std::string root2 = std::string(1, p2->letter ? p2->letter : 'Y') + ":\\";
    // note: if letter assignment raced, use first volume at that offset
    if (!p2->letter) {
        for (auto& v : d3.volumes)
            if (v.disk == disk && v.startLba == p2Start && v.letter) root2 = std::string(1, v.letter) + ":\\";
    }
    auto data3 = Pattern(300 * 1024, 3);
    ICHECK(WriteFileTo(root2 + "ntfs.dat", data3));
    std::puts("[ok] NTFS partition created + file written");

    // move p2 right by 4 MiB within free space
    uint64_t p2NewStart = p2Start + 8192;
    MovePartition(disk, p2Start, p2NewStart, prog);
    {
        DiskInfo d4 = GetDisk(disk);
        ICHECK(FindPart(d4, p2NewStart) != nullptr);
        ICHECK(FindPart(d4, p2Start) == nullptr);
        const PartInfo* mp = FindPart(d4, p2NewStart);
        char letter = mp->letter;
        if (letter) {
            auto got = ReadFileFrom(std::string(1, letter) + ":\\ntfs.dat");
            ICHECK(got == data3);
        } else {
            // read via engine
            WinPhysicalDrive dev(disk, false);
            NtfsVolume nv = NtfsVolume::Open(&dev, p2NewStart, p2Sec);
            for (auto& e : nv.Enumerate(false))
                if (e.path == "/ntfs.dat") {
                    std::vector<uint8_t> got;
                    nv.ReadFileData(e, [&](const uint8_t* p, size_t n) { got.insert(got.end(), p, p + n); });
                    ICHECK(got == data3);
                }
        }
    }
    std::puts("[ok] partition moved; data intact");

    // ---------- 8. label + letter ----------
    SetPartitionLabel(disk, p2NewStart, "RENAMED");
    {
        DiskInfo d5 = GetDisk(disk);
        const PartInfo* rp = FindPart(d5, p2NewStart);
        ICHECK(rp && (rp->label == "RENAMED" || rp->label.empty()));
    }
    // change letter if we have one
    {
        DiskInfo d5 = GetDisk(disk);
        const PartInfo* rp = FindPart(d5, p2NewStart);
        if (rp && rp->letter) {
            char old = rp->letter;
            char want = old == 'Z' ? 'Y' : 'Z';
            SetDriveLetter(disk, p2NewStart, want);
            DiskInfo d6 = GetDisk(disk);
            const PartInfo* rp2 = FindPart(d6, p2NewStart);
            ICHECK(rp2 && rp2->letter == want);
            SetDriveLetter(disk, p2NewStart, old);  // restore
        }
    }
    std::puts("[ok] label + drive letter ops");

    // ---------- 9. hide / unhide (GPT attribute) ----------
    SetHidden(disk, p2NewStart, true);
    {
        DiskInfo d7 = GetDisk(disk);
        const PartInfo* hp = FindPart(d7, p2NewStart);
        ICHECK(hp != nullptr);
        ICHECK(hp->gptAttrs & (1ull << 63));
    }
    SetHidden(disk, p2NewStart, false);
    {
        DiskInfo d7 = GetDisk(disk);
        const PartInfo* hp = FindPart(d7, p2NewStart);
        ICHECK(hp && !(hp->gptAttrs & (1ull << 63)));
    }
    std::puts("[ok] hide/unhide (GPT no-drive-letter attribute)");

    // ---------- 10. split the FAT partition ----------
    SplitPartition(disk, p1Start, p1Sec - 8192, "NTFS", "SPLIT", prog);
    {
        DiskInfo d8 = GetDisk(disk);
        const PartInfo* sp = FindPart(d8, p1Start + p1Sec - 8192);
        ICHECK(sp != nullptr);
    }
    std::puts("[ok] split created a new partition");

    // ---------- 11. delete + scan + restore ----------
    uint64_t victimStart = p1Start + p1Sec - 8192;
    uint64_t victimSectors = 8192;
    DeletePartition(disk, victimStart, prog);
    {
        DiskInfo d9 = GetDisk(disk);
        ICHECK(FindPart(d9, victimStart) == nullptr);
    }
    {
        ScanResult r = ScanLostPartitions(disk, ScanMode::Quick, prog);
        bool found = false;
        for (auto& lp : r.lost)
            if (lp.start == victimStart) found = true;
        ICHECK(found);
        for (auto& lp : r.lost)
            if (lp.start == victimStart) {
                RestoreLostPartition(disk, lp, prog);
                break;
            }
    }
    {
        DiskInfo d10 = GetDisk(disk);
        const PartInfo* rp = FindPart(d10, victimStart);
        ICHECK(rp != nullptr);
    }
    std::puts("[ok] deleted partition scanned + restored");

    // ---------- 12. deleted-file recovery ----------
    {
        DiskInfo d11 = GetDisk(disk);
        const PartInfo* fp = FindPart(d11, p1Start);
        ICHECK(fp && fp->letter);
        std::string victim = std::string(1, fp->letter) + ":\\docs\\note.txt";
        ICHECK(DeleteFileA(victim.c_str()));
        // FS cache lag: wait briefly
        for (int i = 0; i < 10; ++i) {
            if (!std::filesystem::exists(victim)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        auto session = RecoverySession::Start(disk, false, p1Start, true, false, CarveOptions{}, prog);
        auto hits = session->Hits();
        bool sawDeleted = false;
        std::vector<RecoveryHit> wanted;
        for (auto& h : hits)
            if (h.deleted && h.path.find("note.txt") != std::string::npos) {
                sawDeleted = true;
                wanted.push_back(h);
            }
        ICHECK(sawDeleted);
        std::string dest = std::filesystem::temp_directory_path().string() + "\\df_recover_test";
        std::filesystem::create_directories(dest);
        auto stats = session->Recover(wanted, dest, prog);
        ICHECK(stats.recovered >= 1);
        auto got = ReadFileFrom(dest + "\\docs\\note.txt");
        ICHECK(got == data2);
        std::puts("[ok] deleted file recovered with exact content");
    }

    // ---------- 13. partition min size ----------
    {
        uint64_t mn = PartitionMinSectors(disk, p1Start);
        ICHECK(mn > 0 && mn < p1Sec);
    }
    std::puts("[ok] min-size query");

    std::printf("\n=== ALL INTEGRATION CHECKS PASSED (%d) ===\n", g_checks);
    return 0;
}

#endif  // _WIN32
