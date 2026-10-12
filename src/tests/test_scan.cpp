// Lost partition scanning + recovery roundtrips
#include "test_framework.h"
#include "fixtures.h"

#include "df/fat.h"
#include "df/gpt.h"
#include "df/layout.h"
#include "df/mbr.h"
#include "df/ntfs.h"
#include "df/scan.h"

using namespace df;

namespace {
struct LostDisk {
    TempImage img;
    uint64_t p1Start = 2048, p1Sectors = 100000;    // ~48.8 MiB FAT32
    uint64_t p2Start = 139264, p2Sectors = 100000;  // ~48.8 MiB FAT32
    uint64_t total = 0;

    LostDisk() : img(256 * kMiB, "lost") {
        total = img.dev->SizeSectors();
        FatVolume::FormatFat32(*img.dev, p1Start, p1Sectors, "FIRST", 512);
        FatVolume::FormatFat32(*img.dev, p2Start, p2Sectors, "SECOND", 512);
        // add files to both
        FatVolume v1 = FatVolume::Open(img.dev, p1Start, p1Sectors);
        v1.AddFile("/", "ONE.TXT", MakePattern(5000, 1).data(), 5000);
        FatVolume v2 = FatVolume::Open(img.dev, p2Start, p2Sectors);
        v2.AddFile("/", "TWO.TXT", MakePattern(5000, 2).data(), 5000);

        // write GPT layout with both partitions
        DiskLayout L;
        L.style = DiskStyle::Gpt;
        L.totalSectors = total;
        PartInfo a;
        a.start = p1Start;
        a.sectors = p1Sectors;
        a.gptType = Guid::GptBasicData();
        a.gptId = Guid::Random();
        a.gptName = u"First";
        PartInfo b;
        b.start = p2Start;
        b.sectors = p2Sectors;
        b.gptType = Guid::GptBasicData();
        b.gptId = Guid::Random();
        b.gptName = u"Second";
        L.parts = {a, b};
        L.WriteToTables(*img.dev);
    }
};

void WipeGpt(FileImageDevice& dev, uint64_t total) {
    Buffer zeros(512, 0);
    for (uint64_t s = 1; s <= 34; ++s) dev.Write(s, zeros.data(), 1);
    for (uint64_t s = 1; s <= 34; ++s) dev.Write(total - s, zeros.data(), 1);
    dev.Write(0, zeros.data(), 1);  // protective MBR
}
}  // namespace

DF_TEST(ScanFindsDeletedGptPartitions) {
    LostDisk ld;
    WipeGpt(*ld.img.dev, ld.total);

    DiskLayout now = DiskLayout::ReadFromTables(*ld.img.dev);
    CHECK(now.style == DiskStyle::Blank);
    CHECK(now.parts.empty());

    Progress p;
    ScanResult r = ScanLostPartitions(*ld.img.dev, now, ScanMode::Quick, p);
    CHECK_EQ(r.lost.size(), 2u);
    CHECK_EQ(r.lost[0].start, ld.p1Start);
    CHECK_EQ(r.lost[0].sectors, ld.p1Sectors);
    CHECK_EQ(r.lost[0].fs, std::string("FAT32"));
    CHECK(r.lost[0].confidence >= 0.85);
    CHECK(!r.lost[0].overlapsExisting);
    CHECK_EQ(r.lost[1].start, ld.p2Start);
    CHECK_EQ(r.lost[1].sectors, ld.p2Sectors);
}

DF_TEST(ScanRestoredPartitionsKeepFiles) {
    LostDisk ld;
    WipeGpt(*ld.img.dev, ld.total);

    DiskLayout now = DiskLayout::ReadFromTables(*ld.img.dev);
    ScanResult r = ScanLostPartitions(*ld.img.dev, now, ScanMode::Quick, Progress{});
    CHECK_EQ(r.lost.size(), 2u);

    // restore them via the raw table writer (ops layer uses ioctls on Windows)
    DiskLayout L;
    L.style = DiskStyle::Gpt;
    L.totalSectors = ld.total;
    for (auto& lp : r.lost) {
        PartInfo pi;
        pi.start = lp.start;
        pi.sectors = lp.sectors;
        pi.gptType = Guid::GptBasicData();
        pi.gptId = Guid::Random();
        pi.gptName = U8ToU16(lp.label);
        L.parts.push_back(pi);
    }
    L.WriteToTables(*ld.img.dev);

    DiskLayout check = DiskLayout::ReadFromTables(*ld.img.dev);
    CHECK(check.style == DiskStyle::Gpt);
    CHECK_EQ(check.parts.size(), 2u);

    FatVolume v1 = FatVolume::Open(ld.img.dev, ld.p1Start, ld.p1Sectors);
    auto files1 = v1.Enumerate(false);
    bool saw = false;
    for (auto& f : files1)
        if (f.path == "/ONE.TXT") saw = true;
    CHECK(saw);
    FatVolume v2 = FatVolume::Open(ld.img.dev, ld.p2Start, ld.p2Sectors);
    for (auto& f : v2.Enumerate(false))
        if (f.path == "/TWO.TXT") saw = true;
    CHECK(saw);
}

DF_TEST(ScanDetectsOverlapWithExisting) {
    LostDisk ld;
    // keep partition 1 in the table, delete partition 2
    {
        DiskLayout L = DiskLayout::ReadFromTables(*ld.img.dev);
        L.parts.resize(1);
        L.WriteToTables(*ld.img.dev);
    }
    DiskLayout now = DiskLayout::ReadFromTables(*ld.img.dev);
    CHECK_EQ(now.parts.size(), 1u);

    ScanResult r = ScanLostPartitions(*ld.img.dev, now, ScanMode::Quick, Progress{});
    // partition 1 is present (not reported); partition 2 found, no overlap
    bool foundP2 = false;
    for (auto& lp : r.lost)
        if (lp.start == ld.p2Start) {
            foundP2 = true;
            CHECK(!lp.overlapsExisting);
        }
    CHECK(foundP2);
    for (auto& lp : r.lost)
        CHECK(lp.start != ld.p1Start);
}

DF_TEST(ScanFullModeFindsUnalignedPartition) {
    TempImage img(128 * kMiB, "lostfull");
    uint64_t total = img.dev->SizeSectors();
    uint64_t start = 2048 + 123;  // deliberately NOT 1MiB-aligned
    uint64_t sectors = 100000;
    FatVolume::FormatFat32(*img.dev, start, sectors, "SKEWED", 512);
    FatVolume v = FatVolume::Open(img.dev, start, sectors);
    v.AddFile("/", "S.DAT", MakePattern(1000, 9).data(), 1000);

    DiskLayout blank;
    blank.style = DiskStyle::Blank;
    blank.totalSectors = total;

    ScanResult r = ScanLostPartitions(*img.dev, blank, ScanMode::Full, Progress{});
    bool found = false;
    for (auto& lp : r.lost)
        if (lp.start == start) {
            found = true;
            CHECK_EQ(lp.sectors, sectors);
            CHECK(lp.confidence < 0.9);  // unaligned -> lower confidence
        }
    CHECK(found);
}

DF_TEST(GptRepairFromBackup) {
    LostDisk ld;
    // smash only the primary header
    Buffer zeros(512, 0);
    ld.img.dev->Write(1, zeros.data(), 1);

    GptTable t = GptTable::Read(*ld.img.dev);
    CHECK(!t.primaryValid);
    CHECK(t.backupValid);
    CHECK_EQ(t.entries.size(), 128u);
    CHECK(!t.entries[0].IsUnused());

    // repair: rewrite both copies from the healthy state
    {
        GptHeader h = t.backup;
        t.primary = h;
        t.primary.diskGuid = h.diskGuid;
        t.primary.numEntries = h.numEntries;
        t.primary.entrySize = h.entrySize;
    }
    t.Write(*ld.img.dev, ld.total);

    GptTable after = GptTable::Read(*ld.img.dev);
    CHECK(after.primaryValid);
    CHECK(after.backupValid);
    CHECK_EQ(after.entries[0].firstLba, ld.p1Start);

    DiskLayout L = DiskLayout::ReadFromTables(*ld.img.dev);
    CHECK_EQ(L.parts.size(), 2u);
}

DF_TEST(ScanMbrDeletedPartitions) {
    TempImage img(128 * kMiB, "lostmbr");
    uint64_t total = img.dev->SizeSectors();
    uint64_t p1 = 2048, s1 = 100000;
    FatVolume::FormatFat32(*img.dev, p1, s1, "MBRDATA", 512);
    FatVolume::Open(img.dev, p1, s1).AddFile("/", "M.DAT", MakePattern(100, 3).data(), 100);

    // MBR layout with one partition
    Buffer sec(512);
    MbrTable t = MbrTable::MakeEmpty();
    t.entries[0].status = 0x80;
    t.entries[0].type = 0x0C;
    t.entries[0].startLba = uint32_t(p1);
    t.entries[0].sectors = uint32_t(s1);
    t.Write(sec.data());
    Wr32(sec.data() + 440, 0x12345678);
    img.dev->Write(0, sec.data(), 1);

    DiskLayout before = DiskLayout::ReadFromTables(*img.dev);
    CHECK(before.style == DiskStyle::Mbr);
    CHECK_EQ(before.parts.size(), 1u);

    // delete the entry (wipe MBR)
    Buffer zeros(512, 0);
    img.dev->Write(0, zeros.data(), 1);

    DiskLayout blank = DiskLayout::ReadFromTables(*img.dev);
    CHECK(blank.style == DiskStyle::Blank);

    ScanResult r = ScanLostPartitions(*img.dev, blank, ScanMode::Quick, Progress{});
    bool found = false;
    for (auto& lp : r.lost)
        if (lp.start == p1) {
            found = true;
            CHECK_EQ(lp.fs, std::string("FAT32"));
            CHECK_EQ(lp.sectors, s1);
        }
    CHECK(found);
}
