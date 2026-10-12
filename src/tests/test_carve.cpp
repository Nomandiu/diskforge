#include "test_framework.h"
#include "fixtures.h"

#include "df/carve.h"

using namespace df;

namespace {
struct Region {
    std::vector<uint8_t> data;
    void add(const uint8_t* p, size_t n) { data.insert(data.end(), p, p + n); }
    void junk(size_t n, uint32_t seed) {
        auto d = MakePattern(n, seed);
        add(d.data(), d.size());
    }
};
}  // namespace

DF_TEST(CarveFindsJpegPngPdfZip) {
    TempImage img(8 * kMiB, "carve");
    Region r;

    // JPEG with valid footer
    auto jpg = MakePattern(64 * kKiB, 1);
    r.junk(64 * kKiB, 100);
    const uint8_t jpgH[] = {0xFF, 0xD8, 0xFF, 0xE0};
    const uint8_t jpgF[] = {0xFF, 0xD9};
    r.add(jpgH, 4);
    r.add(jpg.data(), jpg.size());
    r.add(jpgF, 2);

    r.junk(4 * kKiB, 101);

    // PNG with IEND trailer
    auto png = MakePattern(32 * kKiB, 2);
    const uint8_t pngH[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    const uint8_t pngF[] = {'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82};
    r.add(pngH, 8);
    r.add(png.data(), png.size());
    r.add(pngF, 8);

    r.junk(2 * kKiB, 102);

    // PDF
    auto pdf = MakePattern(16 * kKiB, 3);
    const char* pdfH = "%PDF-1.4\n";
    const char* pdfF = "%%EOF\n";
    r.add((const uint8_t*)pdfH, strlen(pdfH));
    r.add(pdf.data(), pdf.size());
    r.add((const uint8_t*)pdfF, strlen(pdfF));

    r.junk(2 * kKiB, 103);

    // ZIP: local header + data + EOCD
    auto zip = MakePattern(8 * kKiB, 4);
    const uint8_t zipH[] = {'P', 'K', 0x03, 0x04, 0x14, 0x00, 0x00, 0x00};
    const uint8_t zipF[] = {'P', 'K', 0x05, 0x06};
    r.add(zipH, 8);
    r.add(zip.data(), zip.size());
    r.add(zipF, 4);
    r.add(MakePattern(18, 5).data(), 18);  // EOCD tail

    // write region at LBA 2048
    uint64_t startLba = 2048;
    uint64_t sectors = (r.data.size() + 511) / 512;
    CHECK(sectors * 512 <= 4 * kMiB);
    img.dev->Write(startLba, r.data.data(), sectors);

    CarveOptions opt;
    auto found = CarveRegion(*img.dev, startLba, sectors, opt, Progress{});
    CHECK(found.size() >= 4);

    bool sawJpg = false, sawPng = false, sawPdf = false, sawZip = false;
    for (auto& f : found) {
        CHECK(f.sizeExact);
        switch (f.type) {
            case CarveType::Jpeg:
                sawJpg = true;
                CHECK_EQ(f.sizeBytes, 4 + jpg.size() + 2);
                break;
            case CarveType::Png:
                sawPng = true;
                CHECK_EQ(f.sizeBytes, 8 + png.size() + 8);
                break;
            case CarveType::Pdf:
                sawPdf = true;
                // engine footer is the 5-byte "%%EOF" (trailing newline not included)
                CHECK_EQ(f.sizeBytes, strlen(pdfH) + pdf.size() + 5);
                break;
            case CarveType::Zip:
                sawZip = true;
                CHECK_EQ(f.sizeBytes, 8 + zip.size() + 4 + 18);
                break;
            default: break;
        }
    }
    CHECK(sawJpg);
    CHECK(sawPng);
    CHECK(sawPdf);
    CHECK(sawZip);
}

DF_TEST(CarveMp4AtomWalk) {
    TempImage img(8 * kMiB, "carvemp4");
    Region r;
    r.junk(32 * kKiB, 200);
    // ftyp atom (16 bytes) + mdat atom with size
    uint8_t atom[16] = {0, 0, 0, 16, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm', 0, 0, 2, 0};
    r.add(atom, 16);
    auto payload = MakePattern(100 * kKiB, 6);
    uint8_t mdatHdr[8];
    uint32_t mdatSize = 8 + uint32_t(payload.size());
    // MP4 atom sizes are big-endian
    mdatHdr[0] = uint8_t(mdatSize >> 24);
    mdatHdr[1] = uint8_t(mdatSize >> 16);
    mdatHdr[2] = uint8_t(mdatSize >> 8);
    mdatHdr[3] = uint8_t(mdatSize);
    std::memcpy(mdatHdr + 4, "mdat", 4);
    r.add(mdatHdr, 8);
    r.add(payload.data(), payload.size());
    r.junk(4 * kKiB, 201);

    uint64_t startLba = 2048;
    uint64_t sectors = (r.data.size() + 511) / 512;
    img.dev->Write(startLba, r.data.data(), sectors);

    auto found = CarveRegion(*img.dev, startLba, sectors, CarveOptions{}, Progress{});
    bool sawMp4 = false;
    for (auto& f : found)
        if (f.type == CarveType::Mp4) {
            sawMp4 = true;
            CHECK(f.sizeExact);
            CHECK_EQ(f.sizeBytes, 16 + mdatSize);
        }
    CHECK(sawMp4);
}

DF_TEST(CarveTypeFilter) {
    TempImage img(8 * kMiB, "carvefilter");
    Region r;
    auto jpg = MakePattern(4 * kKiB, 10);
    const uint8_t jpgH[] = {0xFF, 0xD8, 0xFF};
    const uint8_t jpgF[] = {0xFF, 0xD9};
    r.add(jpgH, 3);
    r.add(jpg.data(), jpg.size());
    r.add(jpgF, 2);
    r.junk(1 * kKiB, 202);
    auto png = MakePattern(4 * kKiB, 11);
    const uint8_t pngH[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    const uint8_t pngF[] = {'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82};
    r.add(pngH, 8);
    r.add(png.data(), png.size());
    r.add(pngF, 8);

    uint64_t startLba = 2048;
    uint64_t sectors = (r.data.size() + 511) / 512;
    img.dev->Write(startLba, r.data.data(), sectors);

    CarveOptions onlyPhotos;
    onlyPhotos.wanted = CarveOptions::Photos();
    auto found = CarveRegion(*img.dev, startLba, sectors, onlyPhotos, Progress{});
    CHECK(found.size() >= 2);
    for (auto& f : found) {
        CHECK(f.type == CarveType::Jpeg || f.type == CarveType::Png);
    }
}

DF_TEST(CarveEmbeddedFileSkipped) {
    // a carved file's tail must not produce a second overlapping result
    TempImage img(8 * kMiB, "carvenest");
    Region r;
    r.junk(16 * kKiB, 300);
    // JPEG containing an embedded PNG (like a thumbnail)
    auto png = MakePattern(2 * kKiB, 12);
    const uint8_t pngH[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    const uint8_t pngF[] = {'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82};
    std::vector<uint8_t> jpg;
    const uint8_t jpgH[] = {0xFF, 0xD8, 0xFF};
    jpg.insert(jpg.end(), jpgH, jpgH + 3);
    jpg.insert(jpg.end(), 1024, 0xAB);
    jpg.insert(jpg.end(), pngH, pngH + 8);
    jpg.insert(jpg.end(), png.begin(), png.end());
    jpg.insert(jpg.end(), pngF, pngF + 8);
    jpg.insert(jpg.end(), 2048, 0xCD);
    const uint8_t jpgF[] = {0xFF, 0xD9};
    jpg.insert(jpg.end(), jpgF, jpgF + 2);
    r.add(jpg.data(), jpg.size());
    r.junk(8 * kKiB, 301);

    uint64_t startLba = 2048;
    uint64_t sectors = (r.data.size() + 511) / 512;
    img.dev->Write(startLba, r.data.data(), sectors);

    auto found = CarveRegion(*img.dev, startLba, sectors, CarveOptions{}, Progress{});
    // the JPEG spans the PNG; results must not overlap the JPEG range
    const CarvedFile* jpgHit = nullptr;
    for (auto& f : found)
        if (f.type == CarveType::Jpeg) jpgHit = &f;
    CHECK(jpgHit);
    CHECK_EQ(jpgHit->sizeBytes, jpg.size());
    for (auto& f : found) {
        if (f.type == CarveType::Jpeg) continue;
        uint64_t a1 = jpgHit->startLba, a2 = jpgHit->startLba + jpgHit->sizeBytes / 512;
        uint64_t b1 = f.startLba, b2 = f.startLba + (f.sizeBytes + 511) / 512;
        CHECK(!(a1 < b2 && b1 < a2));
    }
}
