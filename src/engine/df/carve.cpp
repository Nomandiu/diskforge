#include "carve.h"

#include <algorithm>
#include <cstring>

namespace df {

const char* CarveTypeName(CarveType t) {
    switch (t) {
        case CarveType::Jpeg: return "JPEG image";
        case CarveType::Png: return "PNG image";
        case CarveType::Gif: return "GIF image";
        case CarveType::Bmp: return "BMP image";
        case CarveType::Pdf: return "PDF document";
        case CarveType::Zip: return "ZIP archive";
        case CarveType::Rar: return "RAR archive";
        case CarveType::SevenZ: return "7-Zip archive";
        case CarveType::Mp4: return "MP4 video";
        case CarveType::Mp3: return "MP3 audio";
        case CarveType::Wav: return "WAV audio";
        case CarveType::Avi: return "AVI video";
        case CarveType::Ole: return "Office document (legacy)";
        case CarveType::Txt: return "Text file";
    }
    return "?";
}

const char* CarveTypeExt(CarveType t) {
    switch (t) {
        case CarveType::Jpeg: return "jpg";
        case CarveType::Png: return "png";
        case CarveType::Gif: return "gif";
        case CarveType::Bmp: return "bmp";
        case CarveType::Pdf: return "pdf";
        case CarveType::Zip: return "zip";
        case CarveType::Rar: return "rar";
        case CarveType::SevenZ: return "7z";
        case CarveType::Mp4: return "mp4";
        case CarveType::Mp3: return "mp3";
        case CarveType::Wav: return "wav";
        case CarveType::Avi: return "avi";
        case CarveType::Ole: return "doc";
        case CarveType::Txt: return "txt";
    }
    return "bin";
}

namespace {

struct Sig {
    CarveType type;
    const uint8_t* header;
    size_t headerLen;
    int sizeFieldOffset;    // -1 none; offset of a size field (u32/u64) from file start
    int sizeFieldBits;      // 32 or 64
    uint64_t sizeBias;      // added to the raw field
    const uint8_t* footer;  // nullptr when size comes from header or is bounded
    size_t footerLen;
    uint64_t cap;
};

const uint8_t kJpgH[] = {0xFF, 0xD8, 0xFF};
const uint8_t kJpgF[] = {0xFF, 0xD9};
const uint8_t kPngH[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
const uint8_t kPngF[] = {'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82};
const uint8_t kGifH[] = {'G', 'I', 'F', '8'};
const uint8_t kBmpH[] = {'B', 'M'};
const uint8_t kPdfH[] = {'%', 'P', 'D', 'F', '-'};
const uint8_t kPdfF[] = {'%', '%', 'E', 'O', 'F'};
const uint8_t kZipH[] = {'P', 'K', 0x03, 0x04};
const uint8_t kZipF[] = {'P', 'K', 0x05, 0x06};
const uint8_t kRarH[] = {'R', 'a', 'r', '!', 0x1A, 0x07};
const uint8_t k7zH[] = {0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C};
const uint8_t kMp4H[] = {'f', 't', 'y', 'p'};
const uint8_t kMp3H[] = {'I', 'D', '3'};
const uint8_t kWavH[] = {'R', 'I', 'F', 'F'};
const uint8_t kOleH[] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};

const Sig kSigs[] = {
    {CarveType::Jpeg, kJpgH, 3, -1, 0, 0, kJpgF, 2, 64 * kMiB},
    {CarveType::Png, kPngH, 8, -1, 0, 0, kPngF, 8, 64 * kMiB},
    {CarveType::Gif, kGifH, 4, -1, 0, 0, nullptr, 0, 16 * kMiB},
    {CarveType::Bmp, kBmpH, 2, 2, 32, 2, nullptr, 0, 64 * kMiB},
    {CarveType::Pdf, kPdfH, 5, -1, 0, 0, kPdfF, 5, 256 * kMiB},
    {CarveType::Zip, kZipH, 4, -1, 0, 0, kZipF, 4, 1 * kGiB},
    {CarveType::Rar, kRarH, 6, -1, 0, 0, nullptr, 0, 2 * kGiB},
    {CarveType::SevenZ, k7zH, 6, 12, 64, 32, nullptr, 0, 2 * kGiB},
    {CarveType::Mp4, kMp4H, 4, -1, 0, 0, nullptr, 0, 2 * kGiB},
    {CarveType::Mp3, kMp3H, 3, -1, 0, 0, nullptr, 0, 256 * kMiB},
    {CarveType::Wav, kWavH, 4, 4, 32, 8, nullptr, 0, 256 * kMiB},
    {CarveType::Ole, kOleH, 8, -1, 0, 0, nullptr, 0, 256 * kMiB},
};

// strict plausibility checks that kill false positives in random data
bool Plausible(CarveType t, const uint8_t* p, size_t avail) {
    switch (t) {
        case CarveType::Bmp: {
            if (avail < 34) return false;
            uint32_t dibSize = Rd32(p + 14);
            if (dibSize != 12 && dibSize != 40 && dibSize != 52 && dibSize != 56 &&
                dibSize != 64 && dibSize != 108 && dibSize != 124)
                return false;
            uint32_t hdrSize = Rd32(p + 2);
            if (hdrSize < 26 || hdrSize > 64 * kMiB) return false;
            int32_t w = int32_t(Rd32(p + 18)), h = int32_t(Rd32(p + 22));
            if (w <= 0 || h == 0 || w > 1000000 || h < -1000000 || h > 1000000) return false;
            uint16_t planes = Rd16(p + 26);
            if (planes != 1) return false;
            uint16_t bpp = Rd16(p + 28);
            return bpp == 1 || bpp == 4 || bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32;
        }
        case CarveType::Mp4:
            return true;  // validated inline during scanning (size field precedes 'ftyp')
        case CarveType::Wav:
            return avail >= 12 && std::memcmp(p + 8, "WAVE", 4) == 0;
        case CarveType::Gif:
            return p[4] == '7' || p[4] == '9';
        default:
            return true;
    }
}

// read `n` bytes at region-relative offset `abs`
bool ReadAbs(IDiskDevice& dev, Lba regionStart, uint64_t regionBytes, uint64_t abs, uint8_t* out, size_t n) {
    if (abs + n > regionBytes) return false;
    Lba lba = regionStart + abs / kSectorSize;
    uint64_t inSec = abs % kSectorSize;
    uint64_t sectors = (inSec + n + kSectorSize - 1) / kSectorSize;
    Buffer tmp(size_t(sectors) * kSectorSize);
    try {
        dev.Read(lba, tmp.data(), size_t(sectors));
    } catch (const DfError&) {
        return false;
    }
    std::memcpy(out, tmp.data() + inSec, n);
    return true;
}

// stream-search for a footer pattern; returns region-relative offset or UINT64_MAX
uint64_t StreamFind(IDiskDevice& dev, Lba regionStart, uint64_t regionBytes, uint64_t from, uint64_t limit,
                    const uint8_t* pat, size_t patLen, const Progress& prog) {
    const uint64_t kWindow = 1ull * kMiB;
    Buffer win(size_t(kWindow) + patLen + kSectorSize);
    uint64_t scanned = 0;
    while (scanned < limit) {
        prog.CheckCancel();
        uint64_t take = std::min<uint64_t>(kWindow, limit - scanned);
        uint64_t abs = from + scanned;
        if (abs >= regionBytes) break;
        if (abs + take > regionBytes) take = regionBytes - abs;
        if (take == 0) break;
        Lba lba = regionStart + abs / kSectorSize;
        uint64_t inSec = abs % kSectorSize;
        uint64_t sectors = (inSec + take + patLen + kSectorSize - 1) / kSectorSize;
        sectors = std::min<uint64_t>(sectors, (win.size() - inSec + kSectorSize - 1) / kSectorSize);
        try {
            dev.Read(lba, win.data(), size_t(sectors));
        } catch (const DfError&) {
            return UINT64_MAX;
        }
        size_t valid = size_t(sectors * kSectorSize) - size_t(inSec);
        if (valid < patLen) return UINT64_MAX;
        size_t hayLen = size_t(std::min<uint64_t>(take + patLen - 1, valid));
        const uint8_t* found = std::search(win.data() + inSec, win.data() + inSec + hayLen, pat, pat + patLen);
        if (found != win.data() + inSec + hayLen) return abs + uint64_t(found - (win.data() + inSec));
        scanned += take;
    }
    return UINT64_MAX;
}

// find the next plausible signature at/after `from` (bounded) — used to cap unknown-size files
uint64_t StreamFindAny(IDiskDevice& dev, Lba regionStart, uint64_t regionBytes, uint64_t from,
                       uint64_t limit, const Progress& prog) {
    const uint64_t kWindow = 1ull * kMiB;
    Buffer win(size_t(kWindow) + kSectorSize);
    uint64_t scanned = 0;
    while (scanned < limit) {
        prog.CheckCancel();
        uint64_t take = std::min<uint64_t>(kWindow, limit - scanned);
        uint64_t abs = from + scanned;
        if (abs >= regionBytes) break;
        if (abs + take > regionBytes) take = regionBytes - abs;
        if (take == 0) break;
        Lba lba = regionStart + abs / kSectorSize;
        uint64_t inSec = abs % kSectorSize;
        uint64_t sectors = (inSec + take + kSectorSize - 1) / kSectorSize;
        sectors = std::min<uint64_t>(sectors, (win.size() - inSec + kSectorSize - 1) / kSectorSize);
        try {
            dev.Read(lba, win.data(), size_t(sectors));
        } catch (const DfError&) {
            return UINT64_MAX;
        }
        size_t valid = size_t(sectors * kSectorSize) - size_t(inSec);
        for (size_t i = 0; i + 40 <= valid; ++i) {
            const uint8_t* p = win.data() + inSec + i;
            for (auto& s : kSigs) {
                if (i + s.headerLen + 40 > valid) continue;
                if (std::memcmp(p, s.header, s.headerLen) != 0) continue;
                if (!Plausible(s.type, p, valid - i)) continue;
                return abs + i;
            }
        }
        scanned += take;
    }
    return UINT64_MAX;
}

const Sig* MatchAt(const uint8_t* p, size_t avail) {
    for (auto& s : kSigs) {
        if (avail < s.headerLen + 40) continue;
        if (std::memcmp(p, s.header, s.headerLen) != 0) continue;
        if (!Plausible(s.type, p, avail)) continue;
        return &s;
    }
    return nullptr;
}

// MP4 atom sizes are big-endian
inline uint32_t RdBe32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline uint64_t RdBe64(const uint8_t* p) {
    return (uint64_t(RdBe32(p)) << 32) | RdBe32(p + 4);
}

}  // namespace

std::vector<CarvedFile> CarveRegion(IDiskDevice& dev, Lba start, uint64_t sectors,
                                    const CarveOptions& opt, const Progress& prog) {
    std::vector<CarvedFile> out;
    const uint64_t regionBytes = sectors * kSectorSize;
    const uint64_t kWindow = 1ull * kMiB;
    const uint64_t kSlack = 64;
    Buffer win(size_t(kWindow) + kSlack + kSectorSize);

    uint64_t windowBase = 0;
    while (windowBase < regionBytes) {
        prog.CheckCancel();
        prog.Report("Scanning for file signatures", double(windowBase) / double(regionBytes));
        uint64_t take = std::min<uint64_t>(kWindow, regionBytes - windowBase);
        Lba lba = start + windowBase / kSectorSize;
        uint64_t inSec = windowBase % kSectorSize;
        uint64_t readSectors = (inSec + take + kSlack + kSectorSize - 1) / kSectorSize;
        readSectors = std::min<uint64_t>(readSectors, (win.size() - inSec + kSectorSize - 1) / kSectorSize);
        try {
            dev.Read(lba, win.data(), size_t(readSectors));
        } catch (const DfError&) {
            windowBase += take;
            continue;
        }
        const uint8_t* base = win.data() + inSec;
        size_t valid = size_t(readSectors * kSectorSize) - size_t(inSec);
        uint64_t pos = 0;
        uint64_t skipUntil = windowBase + take;

        while (pos < take) {
            if (pos + 64 > valid) break;
            const Sig* s = MatchAt(base + pos, valid - pos);
            if (!s) {
                ++pos;
                continue;
            }
            if (!opt.wanted.empty() && !opt.wanted.count(s->type)) {
                ++pos;
                continue;
            }

            // MP4: the atom size field precedes the 'ftyp' match by 4 bytes
            uint64_t fileStart = windowBase + pos;
            if (s->type == CarveType::Mp4) {
                if (pos < 4) {
                    ++pos;
                    continue;
                }
                uint32_t atomSize = RdBe32(base + pos - 4);
                if (atomSize < 16 || atomSize > 64 * kMiB) {
                    ++pos;
                    continue;
                }
                fileStart -= 4;
            }
            uint64_t size = 0;
            bool exact = false;

            // 1) size from header field
            if (s->sizeFieldOffset >= 0) {
                size_t need = size_t(s->sizeFieldOffset) + (s->sizeFieldBits / 8);
                uint8_t buf[24];
                if (ReadAbs(dev, start, regionBytes, fileStart, buf, std::min<size_t>(need, 24))) {
                    uint64_t v = s->sizeFieldBits == 64 ? Rd64(buf + s->sizeFieldOffset)
                                                        : Rd32(buf + s->sizeFieldOffset);
                    v += s->sizeBias;
                    if (v >= s->headerLen && v <= s->cap) {
                        size = v;
                        exact = true;
                    }
                }
            }

            // 2) size from footer scan
            if (size == 0 && s->footer) {
                uint64_t limit = std::min<uint64_t>(s->cap, regionBytes - fileStart);
                if (limit > s->headerLen) {
                    uint64_t found = StreamFind(dev, start, regionBytes, fileStart + s->headerLen,
                                                limit - s->headerLen, s->footer, s->footerLen, prog);
                    if (found != UINT64_MAX) {
                        size = found - fileStart + s->footerLen;
                        exact = true;
                        // ZIP: include the 18-byte EOCD record that follows the signature
                        if (s->type == CarveType::Zip && fileStart + size + 18 <= regionBytes) size += 18;
                    }
                }
            }

            // 3) MP4: exact size via top-level atom walk
            if (size == 0 && s->type == CarveType::Mp4) {
                uint64_t p = 0;
                bool ok = true;
                for (int guard = 0; guard < 256; ++guard) {
                    uint8_t ah[16];
                    if (!ReadAbs(dev, start, regionBytes, fileStart + p, ah, 16)) {
                        if (p > 0) break;  // ran past readable data after valid atoms
                        ok = false;
                        break;
                    }
                    // atom type must be 4 printable ASCII chars (else we ran into trailing data)
                    bool printable = true;
                    for (int i = 4; i < 8; ++i)
                        if (ah[i] < 0x20 || ah[i] > 0x7E) printable = false;
                    uint32_t asz = RdBe32(ah);
                    bool sane = (asz == 0 || asz == 1 || asz >= 8) && (asz <= 64 * kMiB);
                    if ((!printable || !sane) && p > 0) break;  // end of the atom chain
                    if (!printable || !sane) {
                        ok = false;
                        break;
                    }
                    if (asz == 1) {
                        uint64_t big = RdBe64(ah + 8);
                        if (big < 16 || p + big > s->cap) {
                            if (p > 0) break;
                            ok = false;
                            break;
                        }
                        p += big;
                    } else if (asz == 0) {
                        p = s->cap;  // to-EOF atom
                        break;
                    } else {
                        if (p + asz > s->cap) {
                            if (p > 0) break;
                            ok = false;
                            break;
                        }
                        p += asz;
                    }
                }
                if (ok && p >= 8 && p <= s->cap) {
                    size = p;
                    exact = true;
                }
            }

            // 4) bounded heuristic: end at the next plausible signature within 16 MiB.
            // Search starts past the file's own header (MP4: 'ftyp' sits at +4).
            if (size == 0) {
                uint64_t ownHeader = s->headerLen + (s->type == CarveType::Mp4 ? 4 : 0);
                uint64_t bound = std::min<uint64_t>(16 * kMiB, regionBytes - fileStart);
                uint64_t next = StreamFindAny(dev, start, regionBytes, fileStart + ownHeader,
                                              bound > ownHeader ? bound - ownHeader : 0, prog);
                size = std::min<uint64_t>(s->cap, regionBytes - fileStart);
                if (next != UINT64_MAX && next > fileStart + ownHeader - 1) size = next - fileStart;
                if (size <= ownHeader) size = ownHeader + 1;  // always make progress
                exact = false;
            }

            if (size < s->headerLen) size = s->headerLen;
            if (fileStart + size > regionBytes) size = regionBytes - fileStart;

            CarvedFile cf;
            cf.type = s->type;
            cf.typeName = CarveTypeName(s->type);
            cf.ext = CarveTypeExt(s->type);
            cf.startLba = start + fileStart / kSectorSize;
            cf.sizeBytes = size;
            cf.sizeExact = exact;
            out.push_back(cf);

            uint64_t fileEnd = fileStart + size;
            skipUntil = std::max<uint64_t>(skipUntil, fileEnd);
            uint64_t newPos = fileEnd > windowBase ? fileEnd - windowBase : 0;
            pos = std::max<uint64_t>(newPos, pos + 1);  // always advance
        }
        windowBase = skipUntil;
    }
    return out;
}

}  // namespace df
