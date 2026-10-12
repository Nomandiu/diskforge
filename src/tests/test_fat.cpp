#include "test_framework.h"
#include "fixtures.h"

#include "df/fat.h"
#include "df/fsprobe.h"

using namespace df;

namespace {
struct FatFixture {
    TempImage img;
    uint64_t partStart = 2048;
    uint64_t partSectors = 129024;  // ~63 MiB
    FatFixture() : img(64 * kMiB, "fat") {
        FatVolume::FormatFat32(*img.dev, partStart, partSectors, "TESTDISK", 512);
    }
    FatVolume Open() { return FatVolume::Open(img.dev, partStart, partSectors); }
};

bool HasFile(const std::vector<FatEntry>& v, const std::string& path) {
    for (auto& e : v)
        if (e.path == path && !e.deleted) return true;
    return false;
}
FatEntry* Find(std::vector<FatEntry>& v, const std::string& path) {
    for (auto& e : v)
        if (e.path == path) return &e;
    return nullptr;
}
std::vector<uint8_t> ReadAll(FatVolume& fv, const FatEntry& e) {
    std::vector<uint8_t> out;
    out.reserve(size_t(e.size));
    fv.ReadFileData(e, [&](const uint8_t* p, size_t n) { out.insert(out.end(), p, p + n); });
    return out;
}
}  // namespace

DF_TEST(Fat32FormatAndOpen) {
    FatFixture fx;
    FatVolume fv = fx.Open();
    CHECK_EQ(fv.FsName(), std::string("FAT32"));
    CHECK_EQ(fv.Label(), std::string("TESTDISK"));
    CHECK_EQ(fv.ClusterBytes(), 512ull);
    CHECK(fv.ClusterCount() >= 65525);
    CHECK(fv.ClusterCount() <= fx.partSectors);
    CHECK(fv.FreeBytes() > 60 * kMiB);
    CHECK(fv.UsedBytes() <= fv.ClusterBytes());  // just the root dir cluster
    // boot sector sanity via probe
    FsProbeResult pr = ProbeFs(*fx.img.dev, fx.partStart, fx.partSectors);
    CHECK_EQ(pr.fs, std::string("FAT32"));
}

DF_TEST(Fat32AddAndReadFiles) {
    FatFixture fx;
    {
        FatVolume fv = fx.Open();
        auto data1 = MakePattern(1000, 1);
        fv.AddFile("/", "HELLO.TXT", data1.data(), data1.size());
        auto data2 = MakePattern(50 * kKiB, 2);  // spans multiple clusters
        fv.AddFile("/", "BIG.BIN", data2.data(), data2.size());
        fv.AddDir("/", "DOCS");
        auto data3 = MakePattern(10 * kKiB, 3);
        fv.AddFile("/DOCS", "REPORT.TXT", data3.data(), data3.size());
    }
    FatVolume fv = fx.Open();
    auto files = fv.Enumerate(false);
    CHECK(HasFile(files, "/HELLO.TXT"));
    CHECK(HasFile(files, "/BIG.BIN"));
    CHECK(HasFile(files, "/DOCS"));
    CHECK(HasFile(files, "/DOCS/REPORT.TXT"));

    auto* e = Find(files, "/BIG.BIN");
    CHECK(e);
    CHECK_EQ(e->size, 50 * kKiB);
    auto got = ReadAll(fv, *e);
    auto want = MakePattern(50 * kKiB, 2);
    CHECK(got == want);

    e = Find(files, "/DOCS/REPORT.TXT");
    CHECK(e);
    CHECK(ReadAll(fv, *e) == MakePattern(10 * kKiB, 3));

    e = Find(files, "/HELLO.TXT");
    CHECK(e);
    CHECK(ReadAll(fv, *e) == MakePattern(1000, 1));
}

DF_TEST(Fat32LongNames) {
    FatFixture fx;
    std::string longName = "My Vacation Photos 2026 (edited).jpg";
    {
        FatVolume fv = fx.Open();
        auto data = MakePattern(9000, 7);
        fv.AddFile("/", longName, data.data(), data.size());
    }
    FatVolume fv = fx.Open();
    auto files = fv.Enumerate(false);
    CHECK(HasFile(files, "/" + longName));
    auto* e = Find(files, "/" + longName);
    CHECK(e);
    CHECK(ReadAll(fv, *e) == MakePattern(9000, 7));
}

DF_TEST(Fat32DeleteAndRecoverDeleted) {
    FatFixture fx;
    {
        FatVolume fv = fx.Open();
        auto data = MakePattern(20 * kKiB, 11);
        fv.AddFile("/", "GONER.DAT", data.data(), data.size());
        fv.AddFile("/", "KEEPER.DAT", MakePattern(100, 12).data(), 100);
        fv.DeleteFile("/GONER.DAT");
    }
    FatVolume fv = fx.Open();
    auto alive = fv.Enumerate(false);
    CHECK(!HasFile(alive, "/GONER.DAT"));
    CHECK(HasFile(alive, "/KEEPER.DAT"));

    auto all = fv.Enumerate(true);
    // FAT deletion replaces the SFN's first character with 0xE5 -> recovered as "_ONER.DAT"
    FatEntry* del = Find(all, "/_ONER.DAT");
    CHECK(del);
    CHECK(del->deleted);
    CHECK_EQ(del->size, 20 * kKiB);
    // contiguous heuristic must restore content
    auto got = ReadAll(fv, *del);
    CHECK(got == MakePattern(20 * kKiB, 11));
}

DF_TEST(Fat32DeletedLongNameRecovery) {
    FatFixture fx;
    std::string longName = "important tax documents 2026 final.pdf";
    {
        FatVolume fv = fx.Open();
        auto data = MakePattern(40 * kKiB, 21);
        fv.AddFile("/", longName, data.data(), data.size());
        fv.DeleteFile("/" + longName);
    }
    FatVolume fv = fx.Open();
    auto all = fv.Enumerate(true);
    FatEntry* del = Find(all, "/" + longName);
    CHECK(del);
    CHECK(del->deleted);
    CHECK(ReadAll(fv, *del) == MakePattern(40 * kKiB, 21));
}

DF_TEST(Fat32ShrinkKeepsFiles) {
    FatFixture fx;
    {
        FatVolume fv = fx.Open();
        fv.AddFile("/", "A.DAT", MakePattern(4 * kMiB, 1).data(), 4 * kMiB);
        fv.AddFile("/", "B.DAT", MakePattern(4 * kMiB, 2).data(), 4 * kMiB);
        fv.AddDir("/", "SUB");
        fv.AddFile("/SUB", "C.DAT", MakePattern(2 * kMiB, 3).data(), 2 * kMiB);
    }
    uint64_t newSectors = fx.partSectors - 8192;  // shrink by 4 MiB
    {
        FatVolume fv = fx.Open();
        CHECK(newSectors >= fv.MinSectors());
        Progress p;
        fv.Resize(newSectors, p);
    }
    FatVolume fv = FatVolume::Open(fx.img.dev, fx.partStart, newSectors);
    CHECK_EQ(fv.PartSectors(), newSectors);
    auto files = fv.Enumerate(false);
    CHECK(HasFile(files, "/A.DAT"));
    CHECK(HasFile(files, "/B.DAT"));
    CHECK(HasFile(files, "/SUB/C.DAT"));
    auto* e = Find(files, "/A.DAT");
    CHECK(ReadAll(fv, *e) == MakePattern(4 * kMiB, 1));
    e = Find(files, "/SUB/C.DAT");
    CHECK(ReadAll(fv, *e) == MakePattern(2 * kMiB, 3));
    // probe agrees on the new size
    FsProbeResult pr = ProbeFs(*fx.img.dev, fx.partStart, newSectors);
    CHECK_EQ(pr.totalSectorsFs, newSectors);
}

DF_TEST(Fat32ExtendAcrossFatClass) {
    // FAT size class grows -> data region moves; needs a larger image
    TempImage big(192 * kMiB, "fatbig");
    uint64_t partStart = 2048;
    uint64_t partSectors = 129024;
    FatVolume::FormatFat32(*big.dev, partStart, partSectors, "BIGVOL", 512);
    {
        FatVolume fv = FatVolume::Open(big.dev, partStart, partSectors);
        fv.AddFile("/", "A.DAT", MakePattern(1 * kMiB, 5).data(), 1 * kMiB);
    }
    uint64_t newSectors = 320000;  // ~156 MiB
    {
        FatVolume fv = FatVolume::Open(big.dev, partStart, partSectors);
        Progress p;
        fv.Resize(newSectors, p);
    }
    FatVolume fv = FatVolume::Open(big.dev, partStart, newSectors);
    CHECK_EQ(fv.PartSectors(), newSectors);
    CHECK(fv.ClusterCount() > 65525);
    auto files = fv.Enumerate(false);
    auto* e = Find(files, "/A.DAT");
    CHECK(e);
    CHECK(ReadAll(fv, *e) == MakePattern(1 * kMiB, 5));
    // can still add files after the move
    fv.AddFile("/", "POST.DAT", MakePattern(100 * kKiB, 6).data(), 100 * kKiB);
    auto files2 = fv.Enumerate(false);
    e = Find(files2, "/POST.DAT");
    CHECK(e);
    CHECK(ReadAll(fv, *e) == MakePattern(100 * kKiB, 6));
}

DF_TEST(Fat32ShrinkTooFarThrows) {
    FatFixture fx;
    {
        FatVolume fv = fx.Open();
        fv.AddFile("/", "BIG.DAT", MakePattern(30 * kMiB, 9).data(), 30 * kMiB);
    }
    FatVolume fv = fx.Open();
    uint64_t minS = fv.MinSectors();
    CHECK(minS > 29 * kMiB / 512);   // 30 MiB of data in 512-byte clusters
    CHECK(minS < fx.partSectors);
    CHECK_THROWS(fv.Resize(16 * kMiB / 512, Progress{}));
}

DF_TEST(Fat32CompactToRelocatesClusters) {
    FatFixture fx;
    {
        FatVolume fv = fx.Open();
        // A (4MiB), B (8MiB), C (8MiB) contiguous allocations
        fv.AddFile("/", "A.DAT", MakePattern(4 * kMiB, 1).data(), 4 * kMiB);
        fv.AddFile("/", "B.DAT", MakePattern(8 * kMiB, 2).data(), 8 * kMiB);
        fv.AddFile("/", "C.DAT", MakePattern(8 * kMiB, 3).data(), 8 * kMiB);
        // free the middle -> hole
        fv.DeleteFile("/B.DAT");
        // fill part of the hole with D so chains interleave
        fv.AddFile("/", "D.DAT", MakePattern(3 * kMiB, 4).data(), 3 * kMiB);
    }
    FatVolume fv = fx.Open();
    // allocated clusters span roughly [2 .. maxAlloc] with a hole where B was freed
    uint64_t maxAlloc = 2, reachable = 0;
    for (uint64_t c = fv.ClusterCount() + 1; c >= 2; --c)
        if (fv.FatGetPublicForTest(uint32_t(c)) != 0) {
            maxAlloc = c;
            break;
        }
    for (uint64_t c = 2; c < fv.ClusterCount() + 2; ++c)
        if (fv.FatGetPublicForTest(uint32_t(c)) != 0) ++reachable;
    // limit must leave room for all reachable clusters but cut off the high ones
    uint64_t limit = reachable + (maxAlloc - reachable) / 2;
    CHECK(limit >= reachable);
    CHECK(limit < maxAlloc);
    bool anyAbove = false;
    for (uint64_t c = limit; c < fv.ClusterCount() + 2; ++c)
        if (fv.FatGetPublicForTest(uint32_t(c)) != 0) anyAbove = true;
    CHECK(anyAbove);

    Progress p;
    fv.CompactTo(limit, p);

    FatVolume fv2 = fx.Open();
    // no allocated cluster beyond the limit
    for (uint64_t c = limit; c < fv2.ClusterCount() + 2; ++c)
        CHECK_EQ(fv2.FatGetPublicForTest(uint32_t(c)), 0u);
    // all files intact and content preserved
    auto files = fv2.Enumerate(false);
    auto check = [&](const char* name, uint32_t seed, uint64_t size) {
        auto* e = Find(files, std::string("/") + name);
        CHECK(e);
        CHECK_EQ(e->size, size);
        CHECK(ReadAll(fv2, *e) == MakePattern(size, seed));
    };
    check("A.DAT", 1, 4 * kMiB);
    check("C.DAT", 3, 8 * kMiB);
    check("D.DAT", 4, 3 * kMiB);
    // new writes still work
    fv2.AddFile("/", "E.DAT", MakePattern(64 * kKiB, 8).data(), 64 * kKiB);
    auto files2 = fv2.Enumerate(false);
    auto* e = Find(files2, "/E.DAT");
    CHECK(ReadAll(fv2, *e) == MakePattern(64 * kKiB, 8));
}

DF_TEST(Fat32SetLabel) {
    FatFixture fx;
    {
        FatVolume fv = fx.Open();
        fv.SetLabel("NEWLABEL");
    }
    FatVolume fv = fx.Open();
    CHECK_EQ(fv.Label(), std::string("NEWLABEL"));
    FsProbeResult pr = ProbeFs(*fx.img.dev, fx.partStart, fx.partSectors);
    CHECK_EQ(pr.label, std::string("NEWLABEL"));
}

DF_TEST(Fat32ShrinkThenExtendIntegrity) {
    FatFixture fx;
    {
        FatVolume fv = fx.Open();
        auto d = MakePattern(8 * kMiB, 77);
        fv.AddFile("/", "X.DAT", d.data(), d.size());
        fv.AddDir("/", "DIR");
        fv.AddFile("/DIR", "Y.DAT", MakePattern(64 * kKiB, 78).data(), 64 * kKiB);
    }
    const uint64_t shrunk = fx.partSectors - 20480;  // -10 MiB
    {
        FatVolume fv = fx.Open();
        fv.Resize(shrunk, Progress{});
    }
    {
        FatVolume fv = FatVolume::Open(fx.img.dev, fx.partStart, shrunk);
        fv.Resize(fx.partSectors + 4096, Progress{});  // +2 MiB past original
    }
    FatVolume fv = FatVolume::Open(fx.img.dev, fx.partStart, fx.partSectors + 4096);
    auto files = fv.Enumerate(false);
    auto* e = Find(files, "/X.DAT");
    CHECK(e);
    CHECK(ReadAll(fv, *e) == MakePattern(8 * kMiB, 77));
    e = Find(files, "/DIR/Y.DAT");
    CHECK(e);
    CHECK(ReadAll(fv, *e) == MakePattern(64 * kKiB, 78));
}

DF_TEST(Fat32MinSectorsGrowsWithFiles) {
    FatFixture fx;
    FatVolume fv = fx.Open();
    uint64_t emptyMin = fv.MinSectors();
    auto d = MakePattern(16 * kMiB, 3);
    fv.AddFile("/", "FILL.DAT", d.data(), d.size());
    uint64_t fullMin = fv.MinSectors();
    CHECK(fullMin > emptyMin);
    CHECK(fullMin > 16 * kMiB / 512);
    CHECK(fullMin < fx.partSectors);
}
