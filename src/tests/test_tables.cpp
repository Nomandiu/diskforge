#include "test_framework.h"
#include "fixtures.h"

#include "df/diskio.h"
#include "df/fat.h"
#include "df/fsprobe.h"
#include "df/gpt.h"
#include "df/layout.h"
#include "df/mbr.h"

using namespace df;

DF_TEST(MbrTableRoundtrip) {
    MbrTable t = MbrTable::MakeEmpty();
    t.entries[0].status = 0x80;
    t.entries[0].type = 0x07;
    t.entries[0].startLba = 2048;
    t.entries[0].sectors = 204800;
    t.entries[1].type = 0x0C;
    t.entries[1].startLba = 206848;
    t.entries[1].sectors = 409600;
    Buffer sec(kSectorSize);
    t.Write(sec.data());
    MbrTable p = MbrTable::Parse(sec.data());
    CHECK(p.signatureOk);
    CHECK_EQ(p.entries[0].type, 0x07);
    CHECK(p.entries[0].IsBootable());
    CHECK_EQ(p.entries[0].startLba, 2048u);
    CHECK_EQ(p.entries[1].sectors, 409600u);
    CHECK(!p.IsProtective());
}

DF_TEST(MbrProtective) {
    MbrTable t = MbrTable::MakeProtective(100000000);
    CHECK(t.IsProtective());
    CHECK_EQ(t.entries[0].type, 0xEE);
    Buffer sec(kSectorSize);
    t.Write(sec.data());
    CHECK(MbrTable::Parse(sec.data()).IsProtective());
}

DF_TEST(MbrHideUnhideTypes) {
    CHECK_EQ(mbr::HideType(0x07), 0x17);
    CHECK_EQ(mbr::UnhideType(0x17), 0x07);
    CHECK_EQ(mbr::HideType(0x0C), 0x1C);
    CHECK_EQ(mbr::UnhideType(0x0C), 0x0C);
    CHECK(mbr::IsHiddenType(0x17));
    CHECK(!mbr::IsHiddenType(0x07));
    CHECK_EQ(mbr::HideType(0x17), 0x17);  // idempotent
}

DF_TEST(EbrChainWalkAndAppend) {
    TempImage img(64 * kMiB, "ebr");
    auto* dev = img.dev;
    // extended partition at 4096 with two logicals
    uint32_t ext = 4096;
    uint32_t l1Data = 6144, l2Data = 18432;
    Buffer sec(kSectorSize);
    std::memset(sec.data(), 0, kSectorSize);
    // first EBR
    Wr16(sec.data() + 510, 0xAA55);
    {
        uint8_t* e0 = sec.data() + 446;
        e0[4] = 0x0C;
        Wr32(e0 + 8, l1Data - ext);
        Wr32(e0 + 12, 8192);
        uint8_t* e1 = sec.data() + 462;
        e1[4] = 0x05;
        Wr32(e1 + 8, 16384 - ext);  // second EBR, relative to extended start
        Wr32(e1 + 12, 63);
    }
    dev->Write(ext, sec.data(), 1);
    // second EBR at 16384
    std::memset(sec.data(), 0, kSectorSize);
    Wr16(sec.data() + 510, 0xAA55);
    {
        uint8_t* e0 = sec.data() + 446;
        e0[4] = 0x07;
        Wr32(e0 + 8, l2Data - 16384);
        Wr32(e0 + 12, 8192);
    }
    dev->Write(16384, sec.data(), 1);

    auto links = WalkEbrChain(*dev, ext);
    CHECK_EQ(links.size(), 2u);
    CHECK_EQ(links[0].entry.startLba, l1Data);
    CHECK_EQ(links[0].entry.sectors, 8192u);
    CHECK_EQ(links[1].entry.startLba, l2Data);
    CHECK_EQ(links[1].entry.sectors, 8192u);

    // append a third logical
    uint32_t newEbr = AppendLogicalToChain(*dev, ext, 40960, 4096, 0x0C);
    links = WalkEbrChain(*dev, ext);
    CHECK_EQ(links.size(), 3u);
    CHECK_EQ(links[2].entry.startLba, 40960u);
    CHECK(links[2].ebrLba >= 16384);
    (void)newEbr;
}

DF_TEST(GptTableRoundtrip) {
    TempImage img(64 * kMiB, "gpt");
    auto* dev = img.dev;
    uint64_t total = dev->SizeSectors();

    GptTable t = GptTable::CreateEmpty(total);
    t.entries[0].type = Guid::GptBasicData();
    t.entries[0].id = Guid::Random();
    t.entries[0].firstLba = 2048;
    t.entries[0].lastLba = 2048 + 20480 - 1;
    t.entries[0].name = u"Data";
    t.entries[1].type = Guid::GptSystemEsp();
    t.entries[1].id = Guid::Random();
    t.entries[1].firstLba = 22528;
    t.entries[1].lastLba = 22528 + 2048 - 1;
    t.Write(*dev, total);

    GptTable r = GptTable::Read(*dev);
    CHECK(r.primaryValid);
    CHECK(r.backupValid);
    CHECK_EQ(r.entries.size(), 128u);
    CHECK(r.entries[0].type == Guid::GptBasicData());
    CHECK_EQ(r.entries[0].firstLba, 2048ull);
    CHECK_EQ(r.entries[0].Sectors(), 20480ull);
    CHECK(r.entries[0].name == u"Data");
    CHECK(r.entries[1].type == Guid::GptSystemEsp());
    CHECK(r.primary.diskGuid == t.primary.diskGuid);
}

DF_TEST(GptBackupRecoversPrimary) {
    TempImage img(64 * kMiB, "gptbak");
    auto* dev = img.dev;
    uint64_t total = dev->SizeSectors();
    GptTable t = GptTable::CreateEmpty(total);
    t.entries[0].type = Guid::GptBasicData();
    t.entries[0].id = Guid::Random();
    t.entries[0].firstLba = 2048;
    t.entries[0].lastLba = 2048 + 4096 - 1;
    t.Write(*dev, total);

    // smash the primary header + entries
    Buffer zeros(kSectorSize, 0);
    dev->Write(1, zeros.data(), 1);
    dev->Write(2, zeros.data(), 20);

    GptTable r = GptTable::Read(*dev);
    CHECK(!r.primaryValid);
    CHECK(r.backupValid);
    CHECK_EQ(r.entries.size(), 128u);
    CHECK(r.entries[0].type == Guid::GptBasicData());
    CHECK_EQ(r.entries[0].firstLba, 2048ull);
    CHECK_EQ(r.entries[0].Sectors(), 4096ull);
}

DF_TEST(LayoutReadGptAndFreeExtents) {
    TempImage img(128 * kMiB, "layout");
    auto* dev = img.dev;
    uint64_t total = dev->SizeSectors();

    GptTable t = GptTable::CreateEmpty(total);
    t.entries[0].type = Guid::GptBasicData();
    t.entries[0].id = Guid::Random();
    t.entries[0].firstLba = 2048;
    t.entries[0].lastLba = 2048 + 20480 - 1;
    t.entries[1].type = Guid::GptBasicData();
    t.entries[1].id = Guid::Random();
    t.entries[1].firstLba = 63488;
    t.entries[1].lastLba = 63488 + 40960 - 1;
    t.Write(*dev, total);
    MbrTable prot = MbrTable::MakeProtective(uint32_t(total));
    Buffer b(kSectorSize);
    prot.Write(b.data());
    dev->Write(0, b.data(), 1);

    DiskLayout L = DiskLayout::ReadFromTables(*dev);
    CHECK(L.style == DiskStyle::Gpt);
    CHECK_EQ(L.parts.size(), 2u);
    CHECK_EQ(L.parts[0].start, 2048ull);
    CHECK_EQ(L.parts[0].sectors, 20480ull);
    CHECK_EQ(L.parts[1].start, 63488ull);

    auto free = L.FreeExtents();
    CHECK(free.size() >= 2);
    CHECK_EQ(free[0].start, 22528ull);  // gap between the two
    CHECK_EQ(free[0].sectors, 40960ull);

    // FindSlot preferring after the second partition
    auto slot = L.FindSlot(1024, 63488);
    CHECK(slot.has_value());
    CHECK_EQ(slot->start, 104448ull);
    CHECK(L.OverlapsAny(2048, 100));
    CHECK(!L.OverlapsAny(22528, 100));

    // GPT usable bounds respected: FindSlot cannot allocate in header area
    auto bad = L.FindSlot(kGiB / kSectorSize);
    CHECK(!bad.has_value() || bad->start >= 34);
}

DF_TEST(LayoutMbrWithLogical) {
    TempImage img(64 * kMiB, "mbrlay");
    auto* dev = img.dev;
    Buffer sec(kSectorSize);
    MbrTable t = MbrTable::MakeEmpty();
    t.entries[0].type = 0x07;
    t.entries[0].startLba = 2048;
    t.entries[0].sectors = 8192;
    t.entries[1].type = 0x0F;  // extended LBA
    t.entries[1].startLba = 10240;
    t.entries[1].sectors = 100000;
    t.Write(sec.data());
    dev->Write(0, sec.data(), 1);
    // one logical at 12288
    std::memset(sec.data(), 0, kSectorSize);
    Wr16(sec.data() + 510, 0xAA55);
    uint8_t* e0 = sec.data() + 446;
    e0[4] = 0x0C;
    Wr32(e0 + 8, 12288 - 10240);
    Wr32(e0 + 12, 4096);
    dev->Write(10240, sec.data(), 1);

    DiskLayout L = DiskLayout::ReadFromTables(*dev);
    CHECK(L.style == DiskStyle::Mbr);
    CHECK_EQ(L.extendedLba, 10240u);
    // primary + extended container + logical
    CHECK_EQ(L.parts.size(), 3u);
    bool sawLogical = false;
    for (auto& p : L.parts)
        if (p.isLogical) {
            sawLogical = true;
            CHECK_EQ(p.start, 12288ull);
        }
    CHECK(sawLogical);

    // free extents exclude occupied ranges
    auto free = L.FreeExtents();
    for (auto& e : free) {
        CHECK(!L.OverlapsAny(e.start, e.sectors));
    }
}

DF_TEST(LayoutWriteTablesRoundtrip) {
    TempImage img(128 * kMiB, "laywr");
    auto* dev = img.dev;
    uint64_t total = dev->SizeSectors();

    DiskLayout L;
    L.style = DiskStyle::Gpt;
    L.totalSectors = total;
    PartInfo p1;
    p1.start = 2048;
    p1.sectors = 20480;
    p1.gptType = Guid::GptBasicData();
    p1.gptId = Guid::Random();
    p1.gptName = u"First";
    PartInfo p2;
    p2.start = 40960;
    p2.sectors = 81920;
    p2.gptType = Guid::GptSystemEsp();
    p2.gptId = Guid::Random();
    L.parts = {p1, p2};
    L.WriteToTables(*dev);

    DiskLayout r = DiskLayout::ReadFromTables(*dev);
    CHECK(r.style == DiskStyle::Gpt);
    CHECK_EQ(r.parts.size(), 2u);
    CHECK_EQ(r.parts[0].start, 2048ull);
    CHECK_EQ(r.parts[0].sectors, 20480ull);
    CHECK(r.parts[0].gptType == Guid::GptBasicData());
    CHECK(r.parts[0].gptName == u"First");
    CHECK_EQ(r.parts[1].start, 40960ull);
    // GPT still fully valid after rewrite
    GptTable g = GptTable::Read(*dev);
    CHECK(g.primaryValid);
    CHECK(g.backupValid);
}

DF_TEST(FsProbeDetectsFat32) {
    TempImage img(64 * kMiB, "probe");
    auto* dev = img.dev;
    uint64_t partStart = 2048, partSectors = 129024;
    FatVolume::FormatFat32(*dev, partStart, partSectors, "PROBEVOL", 512);

    FsProbeResult r = ProbeFs(*dev, partStart, partSectors);
    CHECK(r.Ok());
    CHECK_EQ(r.fs, std::string("FAT32"));
    CHECK_EQ(r.label, std::string("PROBEVOL"));
    CHECK_EQ(r.bytesPerSector, 512u);
    CHECK_EQ(r.sectorsPerCluster, 1u);
    CHECK_EQ(r.totalSectorsFs, partSectors);
}

DF_TEST(CopySectorsHandlesOverlap) {
    TempImage img(4 * kMiB, "copy");
    auto* dev = img.dev;
    // fill first 1MiB with pattern
    auto pat = MakePattern(kMiB, 42);
    dev->Write(0, pat.data(), kMiB / kSectorSize);
    // verify pristine
    Buffer buf(kMiB);
    dev->Read(0, buf.data(), kMiB / kSectorSize);
    CHECK(std::memcmp(buf.data(), pat.data(), kMiB) == 0);

    // overlapping forward move (+ 256 KiB) — must not corrupt
    CopySectors(*dev, 0, 512, kMiB / kSectorSize, Progress{});
    dev->Read(512, buf.data(), kMiB / kSectorSize);
    CHECK(std::memcmp(buf.data(), pat.data(), kMiB) == 0);

    // overlapping backward move (back to 0)
    CopySectors(*dev, 512, 0, kMiB / kSectorSize, Progress{});
    dev->Read(0, buf.data(), kMiB / kSectorSize);
    CHECK(std::memcmp(buf.data(), pat.data(), kMiB) == 0);
}
