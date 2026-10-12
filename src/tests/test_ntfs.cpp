// Synthetic NTFS volume builder + parser tests
#include "test_framework.h"
#include "fixtures.h"

#include <cstring>

#include "df/ntfs.h"

using namespace df;

namespace {

struct NtfsImage {
    TempImage img;
    Lba partStart = 2048;
    uint64_t partSectors = 16384;  // 8 MiB
    static constexpr uint32_t kSpc = 1;
    static constexpr uint32_t kRecordSize = 1024;
    static constexpr uint64_t kMftLcn = 32;

    // record placement: records 0..3 at clusters 32..40, records 4..12 at 48..66
    std::vector<uint8_t> image;  // full partition image buffer (partSectors*512)

    NtfsImage() : img(16 * kMiB, "ntfs") {
        image.assign(size_t(partSectors) * 512, 0);
        buildBoot();
        buildRecords();
        // commit
        for (uint64_t s = 0; s < partSectors; s += 256) {
            uint64_t n = std::min<uint64_t>(256, partSectors - s);
            img.dev->Write(partStart + s, image.data() + s * 512, size_t(n));
        }
    }

    uint8_t* Sector(uint64_t partRelSector) { return image.data() + partRelSector * 512; }
    uint8_t* Cluster(uint64_t lcn) { return Sector(lcn * kSpc); }

    void buildBoot() {
        uint8_t* b = Sector(0);
        std::memcpy(b + 3, "NTFS    ", 8);
        Wr16(b + 11, 512);
        b[13] = kSpc;
        Wr16(b + 14, 0);
        b[16] = 0;
        Wr16(b + 17, 0);
        b[21] = 0xF8;
        Wr16(b + 24, 63);
        Wr16(b + 26, 255);
        Wr64(b + 0x28, partSectors);
        Wr64(b + 0x30, kMftLcn);
        Wr64(b + 0x38, kMftLcn + 1);
        b[0x40] = uint8_t(-10);  // 1024-byte records
        b[0x44] = 1;
        Wr32(b + 0x48, 0x12345678);
        Wr16(b + 510, 0xAA55);
    }

    // ---- record building ---------------------------------------------------
    struct AttrWriter {
        std::vector<uint8_t>* buf;
        uint32_t off;

        uint32_t beginResident(uint32_t type) {
            uint32_t at = off;
            ensure(0x18);
            Wr32(ptr(), type);
            buf->at(off + 8) = 0;  // resident
            return at;
        }
        void endResident(uint32_t at, const uint8_t* value, uint32_t len) {
            uint32_t voff = 0x18;
            if (size_t(at) + voff + len > buf->size())
                throw std::runtime_error("resident attribute overflows record");
            uint32_t end = off + len;
            uint8_t* p = buf->data() + at;
            Wr32(p + 4, voff + len);          // total length
            Wr16(p + 0x0A, voff);             // name offset (no name)
            Wr32(p + 0x10, len);              // value length
            Wr16(p + 0x14, voff);             // value offset
            Wr16(p + 0x16, 0);
            std::memcpy(buf->data() + at + voff, value, len);
            off = at + voff + len;
            (void)end;
        }
        uint32_t beginNonResident(uint32_t type) {
            uint32_t at = off;
            ensure(0x40);
            Wr32(ptr(), type);
            buf->at(off + 8) = 1;
            Wr16(ptr() + 0x0A, 0x40);
            Wr16(ptr() + 0x20, 0x40);  // runlist offset
            return at;
        }
        void endNonResident(uint32_t at, const std::vector<std::pair<uint64_t, uint64_t>>& runs,
                            uint64_t realSize) {
            // build runlist
            std::vector<uint8_t> rl;
            int64_t prev = 0;
            uint64_t vcn = 0;
            for (auto& [lcn, cnt] : runs) {
                uint8_t lenB = cnt < 256 ? 1 : (cnt < 65536 ? 2 : 4);
                int64_t delta = int64_t(lcn) - prev;
                uint8_t offB = 1;
                int64_t d2 = delta < 0 ? -delta : delta;
                while (d2 >> (8 * offB)) ++offB;
                // positive deltas must keep the top bit of their last byte clear
                // (runlist offsets are sign-extended)
                if (delta >= 0 && (d2 >> (8 * (offB - 1))) & 0x80) ++offB;
                rl.push_back(uint8_t((offB << 4) | lenB));
                for (int i = 0; i < lenB; ++i) rl.push_back(uint8_t(cnt >> (8 * i)));
                for (int i = 0; i < offB; ++i) {
                    uint64_t mask = 0xFFull << (8 * i);
                    rl.push_back(uint8_t((uint64_t(delta) & mask) >> (8 * i)));
                }
                prev = int64_t(lcn);
                vcn += cnt;
            }
            rl.push_back(0);
            uint64_t clusters = vcn;
            std::memcpy(buf->data() + at + 0x40, rl.data(), rl.size());
            uint8_t* p = buf->data() + at;
            Wr64(p + 0x10, 0);                                  // start VCN
            Wr64(p + 0x18, clusters ? clusters - 1 : 0);        // last VCN
            Wr64(p + 0x28, clusters * kSpc * 512);              // alloc
            Wr64(p + 0x30, realSize);                           // real
            Wr64(p + 0x38, realSize);                           // init
            off = at + 0x40 + uint32_t(rl.size());
            Wr32(p + 4, off - at);                              // total length
        }

    private:
        void ensure(size_t n) { if (buf->size() < off + n + 16) buf->resize(off + n + 16, 0); }
        uint8_t* ptr() { return buf->data() + off; }
    };

    std::vector<uint8_t> makeRecord(uint16_t flags, uint32_t recNo,
                                    const std::function<void(AttrWriter&)>& body) {
        std::vector<uint8_t> rec(kRecordSize, 0);
        std::memcpy(rec.data(), "FILE", 4);
        Wr16(rec.data() + 4, 0x30);   // USA offset
        Wr16(rec.data() + 6, 3);      // USN + 2 sectors
        Wr16(rec.data() + 0x14, 0x38);
        Wr16(rec.data() + 0x16, flags);
        Wr32(rec.data() + 0x1C, kRecordSize);
        Wr64(rec.data() + 0x20, 0);
        AttrWriter w{&rec, 0x38};
        if (body) body(w);
        Wr32(rec.data() + 0x18, w.off);
        Wr32(rec.data() + 0x2C, recNo);
        // end marker
        Wr32(rec.data() + w.off, 0xFFFFFFFF);
        // fixups
        Wr16(rec.data() + 0x30, 0x1234);
        for (int s = 0; s < 2; ++s) {
            uint8_t* tail = rec.data() + s * 512 + 510;
            Wr16(rec.data() + 0x32 + s * 2, Rd16(tail));  // save original
            Wr16(tail, 0x1234);                           // replace with USN
        }
        return rec;
    }

    static void addFileName(AttrWriter& w, uint64_t parent, const std::string& name, bool dosToo = false) {
        std::u16string u16 = U8ToU16(name);
        uint32_t contentLen = 0x42 + uint32_t(u16.size() * 2);
        std::vector<uint8_t> val(contentLen, 0);
        Wr64(val.data(), parent);
        Wr64(val.data() + 0x28, 0x1000);
        Wr64(val.data() + 0x30, 0x1000);
        val[0x40] = uint8_t(u16.size());
        val[0x41] = 0x02;  // win32 name
        for (size_t i = 0; i < u16.size(); ++i) Wr16(val.data() + 0x42 + i * 2, uint16_t(u16[i]));
        uint32_t at = w.beginResident(0x30);
        w.endResident(at, val.data(), contentLen);
    }

    static void addResidentData(AttrWriter& w, const uint8_t* data, uint32_t len) {
        uint32_t at = w.beginResident(0x80);
        w.endResident(at, data, len);
    }

    void placeRecord(uint32_t index, const std::vector<uint8_t>& rec) {
        // record index -> stream sector 2*index; first 4 records at kMftLcn, rest at kMftLcn+16
        uint64_t streamSector = uint64_t(index) * 2;
        uint64_t lcn = streamSector < 8 ? kMftLcn : kMftLcn + 16;
        uint64_t secInRun = streamSector < 8 ? streamSector : streamSector - 8;
        std::memcpy(Cluster(lcn) + secInRun * 512, rec.data(), rec.size());
    }

    void buildRecords() {
        // record 0: $MFT with non-resident $DATA: runs (32,8),(48,18)
        {
            auto rec = makeRecord(0x01, 0, [&](AttrWriter& w) {
                std::vector<std::pair<uint64_t, uint64_t>> runs{{32, 8}, {48, 18}};
                uint32_t at = w.beginNonResident(0x80);
                w.endNonResident(at, runs, 13 * 1024);
            });
            placeRecord(0, rec);
        }
        // record 1: $MFTMirr (dummy)
        placeRecord(1, makeRecord(0x01, 1, [&](AttrWriter& w) {
            static const uint8_t dummy[8] = "mirr";
            addResidentData(w, dummy, 4);
        }));
        // record 2: $LogFile (dummy)
        placeRecord(2, makeRecord(0x01, 2, [&](AttrWriter& w) {
            static const uint8_t dummy[8] = "log";
            addResidentData(w, dummy, 3);
        }));
        // record 3: $Volume with $VOLUME_NAME
        placeRecord(3, makeRecord(0x01, 3, [&](AttrWriter& w) {
            std::u16string label = u"TESTVOL";
            uint32_t at = w.beginResident(0x60);
            w.endResident(at, (const uint8_t*)label.data(), uint32_t(label.size() * 2));
        }));
        // record 4: $AttrDef (dummy)
        placeRecord(4, makeRecord(0x01, 4, [&](AttrWriter& w) {
            addFileName(w, 5, "$AttrDef");
        }));
        // record 5: root directory
        placeRecord(5, makeRecord(0x03, 5, [&](AttrWriter& w) {
            addFileName(w, 5, ".");
        }));
        // record 6: $Bitmap — non-resident (2048 bytes live in clusters 500..503)
        {
            uint64_t totalClusters = partSectors / kSpc;
            std::vector<uint8_t> bm(size_t((totalClusters + 7) / 8), 0);
            auto mark = [&](uint64_t c) { bm[c / 8] |= 1 << (c % 8); };
            mark(0);                       // boot
            for (uint64_t c = 32; c < 40; ++c) mark(c);
            for (uint64_t c = 48; c < 66; ++c) mark(c);
            for (uint64_t c = 200; c < 217; ++c) mark(c);
            for (uint64_t c = 300; c < 309; ++c) mark(c);
            for (uint64_t c = 400; c < 417; ++c) mark(c);
            for (uint64_t c = 500; c < 504; ++c) mark(c);  // bitmap storage itself
            std::memcpy(Cluster(500), bm.data(), bm.size());
            placeRecord(6, makeRecord(0x01, 6, [&](AttrWriter& w) {
                addFileName(w, 5, "$Bitmap");
                uint32_t at = w.beginNonResident(0xB0);
                w.endNonResident(at, std::vector<std::pair<uint64_t, uint64_t>>{{500, 4}}, bm.size());
            }));
        }
        // record 7: alive.txt (resident data)
        placeRecord(7, makeRecord(0x01, 7, [&](AttrWriter& w) {
            addFileName(w, 5, "alive.txt");
            addResidentData(w, (const uint8_t*)"Hello, DiskForge!", 17);
        }));
        // record 8: big.bin DELETED, non-resident 3 runs: (200,16),(300,8),(400,16)
        placeRecord(8, makeRecord(0x00, 8, [&](AttrWriter& w) {
            addFileName(w, 5, "big.bin");
            std::vector<std::pair<uint64_t, uint64_t>> runs{{200, 16}, {300, 8}, {400, 16}};
            uint32_t at = w.beginNonResident(0x80);
            w.endNonResident(at, runs, 20000);
        }));
        // record 9: gone.docx DELETED resident
        placeRecord(9, makeRecord(0x00, 9, [&](AttrWriter& w) {
            addFileName(w, 5, "gone.docx");
            auto d = MakePattern(600, 33);
            addResidentData(w, d.data(), 600);
        }));
        // record 10: subdir (alive, dir)
        placeRecord(10, makeRecord(0x03, 10, [&](AttrWriter& w) {
            addFileName(w, 5, "subdir");
        }));
        // record 11: inner.txt inside subdir
        placeRecord(11, makeRecord(0x01, 11, [&](AttrWriter& w) {
            addFileName(w, 10, "inner.txt");
            addResidentData(w, (const uint8_t*)"inner data", 10);
        }));
        // record 12: orphan.txt DELETED with missing parent
        placeRecord(12, makeRecord(0x00, 12, [&](AttrWriter& w) {
            addFileName(w, 999, "orphan.txt");
            addResidentData(w, (const uint8_t*)"orphan data", 11);
        }));
    }
};

std::vector<uint8_t> ReadAll(NtfsVolume& v, const NtfsEntry& e) {
    std::vector<uint8_t> out;
    v.ReadFileData(e, [&](const uint8_t* p, size_t n) { out.insert(out.end(), p, p + n); });
    return out;
}

NtfsEntry* FindEntry(std::vector<NtfsEntry>& v, const std::string& path) {
    for (auto& e : v)
        if (e.path == path) return &e;
    return nullptr;
}

}  // namespace

DF_TEST(NtfsOpenAndLabel) {
    NtfsImage im;
    NtfsVolume v = NtfsVolume::Open(im.img.dev, im.partStart, im.partSectors);
    CHECK_EQ(v.Label(), std::string("TESTVOL"));
    CHECK_EQ(v.TotalSectors(), im.partSectors);
    CHECK_EQ(v.ClusterBytes(), 512ull);
    CHECK_EQ(v.MftRecordCount(), 13ull);
}

DF_TEST(NtfsEnumerateAlive) {
    NtfsImage im;
    NtfsVolume v = NtfsVolume::Open(im.img.dev, im.partStart, im.partSectors);
    auto entries = v.Enumerate(false);
    bool sawAlive = false, sawInner = false, sawSubdir = false;
    for (auto& e : entries) {
        CHECK(!e.deleted);
        if (e.path == "/alive.txt") sawAlive = true;
        if (e.path == "/subdir/inner.txt") sawInner = true;
        if (e.path == "/subdir") sawSubdir = true;
    }
    CHECK(sawAlive);
    CHECK(sawInner);
    CHECK(sawSubdir);
    // metadata hidden
    for (auto& e : entries)
        CHECK(e.path.find("/$") == std::string::npos);
}

DF_TEST(NtfsEnumerateDeleted) {
    NtfsImage im;
    NtfsVolume v = NtfsVolume::Open(im.img.dev, im.partStart, im.partSectors);
    auto entries = v.Enumerate(true);
    auto* big = FindEntry(entries, "/big.bin");
    CHECK(big);
    CHECK(big->deleted);
    CHECK_EQ(big->size, 20000ull);
    auto* gone = FindEntry(entries, "/gone.docx");
    CHECK(gone);
    CHECK(gone->deleted);
    auto* orphan = FindEntry(entries, "/Lost Files/orphan.txt");
    CHECK(orphan);
    CHECK(orphan->deleted);
    CHECK(orphan->orphaned);
}

DF_TEST(NtfsReadResident) {
    NtfsImage im;
    NtfsVolume v = NtfsVolume::Open(im.img.dev, im.partStart, im.partSectors);
    auto entries = v.Enumerate(true);
    auto* e = FindEntry(entries, "/alive.txt");
    CHECK(e);
    auto data = ReadAll(v, *e);
    CHECK_EQ(data.size(), 17u);
    CHECK(std::memcmp(data.data(), "Hello, DiskForge!", 17) == 0);

    e = FindEntry(entries, "/gone.docx");
    CHECK(e);
    data = ReadAll(v, *e);
    CHECK(data == MakePattern(600, 33));

    e = FindEntry(entries, "/Lost Files/orphan.txt");
    CHECK(e);
    data = ReadAll(v, *e);
    CHECK(std::memcmp(data.data(), "orphan data", 11) == 0);
}

DF_TEST(NtfsReadNonResidentMultiRun) {
    NtfsImage im;
    // write big.bin content into its runs
    auto content = MakePattern(20000, 55);
    {
        // runs: (200,16),(300,8),(400,16) — 40 clusters of 512 bytes = 20480 >= 20000
        uint64_t off = 0;
        for (auto [lcn, cnt] : std::vector<std::pair<uint64_t, uint64_t>>{{200, 16}, {300, 8}, {400, 16}}) {
            for (uint64_t c = 0; c < cnt && off < 20000; ++c, off += 512)
                im.img.dev->Write(im.partStart + (lcn + c) * 1, content.data() + off, 1);
        }
    }
    NtfsVolume v = NtfsVolume::Open(im.img.dev, im.partStart, im.partSectors);
    auto entries = v.Enumerate(true);
    auto* e = FindEntry(entries, "/big.bin");
    CHECK(e);
    auto got = ReadAll(v, *e);
    CHECK_EQ(got.size(), 20000u);
    CHECK(got == content);
}

DF_TEST(NtfsUsedBytesFromBitmap) {
    NtfsImage im;
    NtfsVolume v = NtfsVolume::Open(im.img.dev, im.partStart, im.partSectors);
    // marked: 1 + 8 + 18 + 17 + 9 + 17 + 4 = 74 clusters
    CHECK_EQ(v.UsedBytes(), 74ull * 512);
}

DF_TEST(NtfsDetect) {
    NtfsImage im;
    CHECK(NtfsVolume::Detect(*im.img.dev, im.partStart));
    CHECK(!NtfsVolume::Detect(*im.img.dev, 4096));
}

DF_TEST(NtfsRunlistDecode) {
    // hand-crafted runlist: (32,8),(48,16)
    uint8_t rl[] = {0x11, 8, 32, 0x11, 16, 16, 0x00};
    std::vector<NtfsVolume::Run> runs;
    CHECK(NtfsVolume::DecodeRunlist(rl, sizeof(rl), runs));
    CHECK_EQ(runs.size(), 2u);
    CHECK_EQ(runs[0].lcn, 32ull);
    CHECK_EQ(runs[0].count, 8ull);
    CHECK_EQ(runs[1].lcn, 48ull);
    CHECK_EQ(runs[1].count, 16ull);
    // sparse run (offset size 0)
    uint8_t rl2[] = {0x01, 5, 0x00};
    CHECK(NtfsVolume::DecodeRunlist(rl2, sizeof(rl2), runs));
    CHECK_EQ(runs.size(), 1u);
    CHECK_EQ(runs[0].lcn, UINT64_MAX);
    // malformed: zero length run
    uint8_t rl3[] = {0x11, 0, 32, 0x00};
    CHECK(!NtfsVolume::DecodeRunlist(rl3, sizeof(rl3), runs));
    // multi-byte offsets: header 0x31 = len 1 byte, offset 3 bytes
    uint8_t rl4[] = {0x31, 16, 0x00, 0x00, 0x01, 0x00};
    CHECK(NtfsVolume::DecodeRunlist(rl4, sizeof(rl4), runs));
    CHECK_EQ(runs.size(), 1u);
    CHECK_EQ(runs[0].lcn, 65536ull);
}
