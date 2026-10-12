#include "fat.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <unordered_map>

namespace df {

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
uint32_t Fat32CalcFatSectors(uint64_t dataSectors, uint32_t spc) {
    uint32_t fatSz = 1;
    for (int i = 0; i < 16; ++i) {
        uint64_t clusters = dataSectors / spc;
        uint32_t next = uint32_t((clusters * 4 + 2 + kSectorSize - 1) / kSectorSize + 4);
        if (next == fatSz) break;
        fatSz = next;
    }
    return fatSz;
}

uint8_t LfnChecksum(const uint8_t sfn[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; ++i) sum = uint8_t((sum >> 1) + ((sum & 1) << 7) + sfn[i]);
    return sum;
}

static std::string SfnToString(const uint8_t* d) {
    auto part = [&](int off, int n) {
        std::string s;
        for (int i = 0; i < n; ++i)
            if (d[off + i] != ' ') s += char(d[off + i]);
        return s;
    };
    std::string base = part(0, 8), ext = part(8, 3);
    if (d[0] == 0x05) base[0] = '\xE5';
    return ext.empty() ? base : base + "." + ext;
}

void SfnFromName(const std::string& name, uint8_t out[11]) {
    std::string base, ext;
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        base = name.substr(0, dot);
        ext = name.substr(dot + 1);
    } else {
        base = name;
    }
    auto clean = [](std::string s, size_t maxLen) {
        std::string o;
        for (char c : s) {
            if (o.size() >= maxLen) break;
            if (uint8_t(c) < 0x20 || c == '.' || c == ' ' || c == '*' || c == '?' || c == '\\' ||
                c == '/' || c == '|' || c == ',' || c == ';' || c == ':' || c == '=' || c == '<' ||
                c == '>' || c == '[' || c == ']' || c == '+' || c == '"')
                o += '_';
            else
                o += char(std::toupper((unsigned char)c));
        }
        return o;
    };
    base = clean(base, 8);
    ext = clean(ext, 3);
    if (base.empty()) base = "FILE";
    if (base.size() > 8) base = base.substr(0, 6) + "~1";
    while (base.size() < 8) base += ' ';
    while (ext.size() < 3) ext += ' ';
    for (int i = 0; i < 8; ++i) out[i] = uint8_t(base[i]);
    for (int i = 0; i < 3; ++i) out[8 + i] = uint8_t(ext[i]);
}

static void DosNow(uint16_t* date, uint16_t* time_) {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    *date = uint16_t(((tm.tm_year - 80) & 0x7F) << 9 | ((tm.tm_mon + 1) & 0xF) << 5 | (tm.tm_mday & 0x1F));
    *time_ = uint16_t((tm.tm_hour & 0x1F) << 11 | (tm.tm_min & 0x3F) << 5 | ((tm.tm_sec / 2) & 0x1F));
}

static bool IsEoc(int bits, uint32_t v) {
    if (bits == 12) return v >= 0xFF8;
    if (bits == 16) return v >= 0xFFF8;
    return v >= 0x0FFFFFF8;
}
static bool IsBadCluster(int bits, uint32_t v) {
    if (bits == 12) return v == 0xFF7;
    if (bits == 16) return v == 0xFFF7;
    return v == 0x0FFFFFF7;
}

// ---------------------------------------------------------------------------
// boot parsing / open
// ---------------------------------------------------------------------------
void FatVolume::ParseBoot() {
    Buffer s(kSectorSize);
    dev_->Read(partStart_, s.data(), 1);
    const uint8_t* b = s.data();

    bps_ = Rd16(b + 11);
    spc_ = b[13];
    rsvd_ = Rd16(b + 14);
    nfats_ = b[16];
    rootEnt_ = Rd16(b + 17);
    uint16_t tot16 = Rd16(b + 19);
    uint32_t tot32 = Rd32(b + 32);
    uint16_t fatSz16 = Rd16(b + 22);
    fatSz_ = fatSz16 ? fatSz16 : Rd32(b + 36);

    if (bps_ == 0 || spc_ == 0 || rsvd_ == 0 || nfats_ == 0 || fatSz_ == 0)
        DF_THROW(Parse, "invalid FAT BPB");
    totalSectors_ = tot16 ? tot16 : tot32;

    rootDirSectors_ = (uint64_t(rootEnt_) * 32 + bps_ - 1) / bps_;
    dataStartSector_ = rsvd_ + uint64_t(nfats_) * fatSz_ + rootDirSectors_;
    uint64_t dataSectors = totalSectors_ > dataStartSector_ ? totalSectors_ - dataStartSector_ : 0;
    clusterCount_ = dataSectors / spc_;

    if (fatSz16 == 0) {
        fatBits_ = 32;
        rootClus_ = Rd32(b + 44);
        fsInfoSec_ = Rd16(b + 48);
        bkBootSec_ = Rd16(b + 50);
        char lb[12];
        std::memcpy(lb, b + 71, 11);
        lb[11] = 0;
        label_ = lb;
        while (!label_.empty() && label_.back() == ' ') label_.pop_back();
        if (Rd8(b + 66) == 0x29) serial_ = Rd32(b + 67);
    } else if (clusterCount_ >= 4085) {
        fatBits_ = 16;
        if (Rd8(b + 38) == 0x28 || Rd8(b + 38) == 0x29) {
            char lb[12];
            std::memcpy(lb, b + 43, 11);
            lb[11] = 0;
            label_ = lb;
            while (!label_.empty() && label_.back() == ' ') label_.pop_back();
            serial_ = Rd32(b + 39);
        }
    } else {
        fatBits_ = 12;
    }
    if (fatBits_ == 32 && clusterCount_ < 65525) DF_THROW(Parse, "FAT32 with too few clusters");
    if (fatBits_ == 32 && (rootClus_ < 2 || rootClus_ >= clusterCount_ + 2))
        DF_THROW(Parse, "invalid FAT32 root cluster");
}

FatVolume FatVolume::Open(IDiskDevice* dev, Lba partStart, uint64_t partSectors) {
    FatVolume v;
    v.dev_ = dev;
    v.partStart_ = partStart;
    (void)partSectors;
    try {
        v.ParseBoot();
    } catch (const DfError&) {
        DF_THROW(Parse, "not a FAT volume at LBA " + std::to_string(partStart));
    }
    return v;
}

bool FatVolume::Detect(IDiskDevice& dev, Lba partStart) {
    try {
        Open(&dev, partStart, 0);
        return true;
    } catch (const DfError&) {
        return false;
    }
}

std::string FatVolume::FsName() const {
    switch (fatBits_) {
        case 12: return "FAT12";
        case 16: return "FAT16";
        default: return "FAT32";
    }
}

// ---------------------------------------------------------------------------
// FAT entry access with windowed cache
// ---------------------------------------------------------------------------
static constexpr uint64_t kFatPageEntries = 32768;

FatVolume::FatPage* FatVolume::LoadFatPage(uint64_t entryIndex) const {
    uint64_t pageStart = (entryIndex / kFatPageEntries) * kFatPageEntries;
    for (auto& p : fatCache_)
        if (p.startEntry == pageStart) return &p;
    if (fatCache_.size() >= 8) {
        for (size_t i = 0; i < fatCache_.size(); ++i) {
            if (!fatCache_[i].dirty) {
                fatCache_.erase(fatCache_.begin() + long(i));
                goto loaded;
            }
        }
        FlushFat();
        fatCache_.erase(fatCache_.begin());
    }
loaded:;
    FatPage p;
    p.startEntry = pageStart;
    uint64_t byteStart, byteLen;
    if (fatBits_ == 12) {
        byteStart = pageStart + pageStart / 2;
        byteLen = kFatPageEntries + kFatPageEntries / 2;
    } else {
        byteStart = pageStart * (fatBits_ == 32 ? 4 : 2);
        byteLen = kFatPageEntries * (fatBits_ == 32 ? 4 : 2);
    }
    p.data.resize(size_t(byteLen));
    uint64_t secStart = rsvd_ + byteStart / bps_;
    uint64_t secOff = byteStart % bps_;
    uint64_t secCount = (secOff + byteLen + bps_ - 1) / bps_;
    Buffer tmp(size_t(secCount) * bps_);
    dev_->Read(partStart_ + secStart, tmp.data(), size_t(secCount));
    std::memcpy(p.data.data(), tmp.data() + secOff, size_t(byteLen));
    fatCache_.push_back(std::move(p));
    return &fatCache_.back();
}

uint32_t FatVolume::FatGet(uint32_t cluster) const {
    if (cluster < 2 || cluster >= clusterCount_ + 2) DF_THROW(Parse, "FAT index out of range");
    FatPage* p = LoadFatPage(cluster);
    uint64_t rel = cluster - p->startEntry;
    if (fatBits_ == 32) return Rd32(p->data.data() + rel * 4) & 0x0FFFFFFFu;
    if (fatBits_ == 16) return Rd16(p->data.data() + rel * 2);
    size_t off = size_t(rel + rel / 2);
    uint32_t v = p->data[off] | (uint32_t(p->data[off + 1]) << 8);
    return (cluster & 1) ? (v >> 4) : (v & 0xFFF);
}

void FatVolume::FatSet(uint32_t cluster, uint32_t value) {
    if (cluster < 2 || cluster >= clusterCount_ + 2) DF_THROW(Parse, "FAT index out of range");
    FatPage* p = LoadFatPage(cluster);
    uint64_t rel = cluster - p->startEntry;
    if (fatBits_ == 32) {
        Wr32(p->data.data() + rel * 4, value & 0x0FFFFFFFu);
    } else if (fatBits_ == 16) {
        Wr16(p->data.data() + rel * 2, uint16_t(value));
    } else {
        size_t off = size_t(rel + rel / 2);
        uint32_t old = p->data[off] | (uint32_t(p->data[off + 1]) << 8);
        uint32_t nv = (cluster & 1) ? ((old & 0x000Fu) | ((value & 0xFFFu) << 4))
                                    : ((old & 0xF000u) | (value & 0xFFFu));
        p->data[off] = uint8_t(nv);
        p->data[off + 1] = uint8_t(nv >> 8);
    }
    p->dirty = true;
}

void FatVolume::FlushFat() const {
    for (auto& p : fatCache_) {
        if (!p.dirty) continue;
        uint64_t byteStart;
        if (fatBits_ == 12)
            byteStart = p.startEntry + p.startEntry / 2;
        else
            byteStart = p.startEntry * (fatBits_ == 32 ? 4 : 2);
        uint64_t byteLen = p.data.size();
        uint64_t secStart = rsvd_ + byteStart / bps_;
        uint64_t secOff = byteStart % bps_;
        uint64_t secCount = (secOff + byteLen + bps_ - 1) / bps_;
        Buffer tmp(size_t(secCount) * bps_);
        dev_->Read(partStart_ + secStart, tmp.data(), size_t(secCount));
        std::memcpy(tmp.data() + secOff, p.data.data(), size_t(byteLen));
        for (uint8_t f = 0; f < nfats_; ++f)
            dev_->Write(partStart_ + secStart + uint64_t(f) * fatSz_, tmp.data(), size_t(secCount));
        p.dirty = false;
    }
}

// ---------------------------------------------------------------------------
// cluster / chain helpers
// ---------------------------------------------------------------------------
void FatVolume::ReadCluster(uint32_t cluster, uint8_t* buf) const {
    dev_->Read(ClusterToLba(cluster), buf, spc_);
}
void FatVolume::WriteCluster(uint32_t cluster, const uint8_t* buf) {
    dev_->Write(ClusterToLba(cluster), buf, spc_);
}

std::vector<uint32_t> FatVolume::ChainOf(uint32_t head, bool* intact) const {
    std::vector<uint32_t> out;
    if (intact) *intact = true;
    if (head == 0) return out;
    if (head < 2 || head >= clusterCount_ + 2) {
        if (intact) *intact = false;
        return out;
    }
    // epoch-marked visited set (cached to keep enumeration O(n) overall)
    if (chainEpoch_.size() != clusterCount_ + 2) {
        chainEpoch_.assign(size_t(clusterCount_) + 2, 0);
        curEpoch_ = 0;
    }
    if (++curEpoch_ == 0) {
        std::fill(chainEpoch_.begin(), chainEpoch_.end(), 0);
        curEpoch_ = 1;
    }
    uint32_t c = head;
    uint64_t steps = 0;
    while (true) {
        if (c < 2 || c >= clusterCount_ + 2 || chainEpoch_[c] == curEpoch_) {
            if (intact) *intact = false;
            break;
        }
        chainEpoch_[c] = curEpoch_;
        out.push_back(c);
        if (++steps > clusterCount_ + 2) {
            if (intact) *intact = false;
            break;
        }
        uint32_t v = FatGet(c);
        if (v == 0 || IsBadCluster(fatBits_, v)) {
            if (intact) *intact = false;
            break;
        }
        if (IsEoc(fatBits_, v)) break;
        c = v;
    }
    return out;
}

uint32_t FatVolume::FreeClusterCount() const {
    uint64_t freeCnt = 0;
    for (uint64_t c = 2; c < clusterCount_ + 2; ++c)
        if (FatGet(uint32_t(c)) == 0) ++freeCnt;
    return uint32_t(freeCnt);
}

uint64_t FatVolume::UsedBytes() const {
    uint64_t used = 0;
    for (uint64_t c = 2; c < clusterCount_ + 2; ++c)
        if (FatGet(uint32_t(c)) != 0) ++used;
    return used * ClusterBytes();
}

uint64_t FatVolume::FreeBytes() const { return uint64_t(FreeClusterCount()) * ClusterBytes(); }

uint32_t FatVolume::NextFreeCluster() const {
    for (uint64_t c = 2; c < clusterCount_ + 2; ++c)
        if (FatGet(uint32_t(c)) == 0) return uint32_t(c);
    return 0;
}

bool FatVolume::FollowsChain(const FatEntry& e) const {
    bool intact = false;
    ChainOf(e.firstCluster, &intact);
    return intact;
}

// ---------------------------------------------------------------------------
// enumeration
// ---------------------------------------------------------------------------
std::vector<FatEntry> FatVolume::Enumerate(bool includeDeleted, const Progress& prog) const {
    std::vector<FatEntry> out;
    uint64_t reported = 0;

    struct LfnPart {
        uint16_t chars[13];
        uint8_t checksum;
    };

    // iterate directory sectors; cb(clusterId, dataOfSector)
    auto forSector = [&](const std::vector<uint32_t>& chain, bool root16,
                         const std::function<void(uint32_t, uint8_t*)>& cb) {
        if (root16) {
            Buffer sec(kSectorSize);
            for (uint64_t i = 0; i < rootDirSectors_; ++i) {
                dev_->Read(partStart_ + rsvd_ + uint64_t(nfats_) * fatSz_ + i, sec.data(), 1);
                cb(0, sec.data());
            }
        } else {
            Buffer cluster((size_t(ClusterBytes())));
            uint64_t secPer = ClusterBytes() / bps_;
            for (uint32_t c : chain) {
                ReadCluster(c, cluster.data());
                for (uint64_t s = 0; s < secPer; ++s) cb(c, cluster.data() + size_t(s * bps_));
            }
        }
    };

    std::function<void(const std::vector<uint32_t>&, bool, const std::string&)> walk =
        [&](const std::vector<uint32_t>& chain, bool root16, const std::string& path) {
            std::vector<LfnPart> lfn, lfnDel;

            // strict checksum for alive entries; lenient for deleted ones (the SFN's first
            // byte was replaced by 0xE5 on delete, breaking the stored checksum)
            auto assembleName = [&](const uint8_t* sfn, const std::vector<LfnPart>& parts,
                                     bool strict) -> std::string {
                uint8_t ck = LfnChecksum(sfn);
                if (parts.empty()) return SfnToString(sfn);
                std::u16string u16;
                for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
                    if (strict && it->checksum != ck) return SfnToString(sfn);
                    for (int i = 0; i < 13; ++i) u16 += it->chars[i];
                }
                while (!u16.empty() && (u16.back() == 0 || u16.back() == 0xFFFF)) u16.pop_back();
                std::string s = U16ToU8(u16);
                return s.empty() ? SfnToString(sfn) : s;
            };

            forSector(chain, root16, [&](uint32_t clusterId, uint8_t* data) {
                if (((++reported) & 0xFF) == 0) {
                    prog.Report("Scanning files", 0.5);
                    prog.CheckCancel();
                }
                size_t n = size_t(bps_ / 32);
                for (size_t i = 0; i < n; ++i) {
                    const uint8_t* d = data + i * 32;
                    if (d[0] == 0x00) return;  // no more entries in this directory
                    uint8_t attr = d[11];
                    if (attr == 0x0F) {
                        LfnPart part;
                        part.checksum = Rd8(d + 13);
                        static const int offs[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
                        for (int k = 0; k < 13; ++k) part.chars[k] = Rd16(d + offs[k]);
                        if (d[0] == 0xE5)
                            lfnDel.push_back(part);
                        else
                            lfn.push_back(part);
                        continue;
                    }
                    bool deleted = d[0] == 0xE5;
                    bool isDir = (attr & 0x10) != 0;
                    if (!deleted && d[0] == '.') {  // . and ..
                        lfn.clear();
                        lfnDel.clear();
                        continue;
                    }
                    if ((attr & 0x08) && !isDir) {  // volume labels
                        lfn.clear();
                        lfnDel.clear();
                        continue;
                    }
                    if (deleted && !includeDeleted) {
                        lfn.clear();
                        lfnDel.clear();
                        continue;
                    }

                    uint8_t sfn[11];
                    std::memcpy(sfn, d, 11);
                    if (sfn[0] == 0x05) sfn[0] = 0xE5;
                    FatEntry e;
                    e.deleted = deleted;
                    e.name = assembleName(sfn, deleted ? lfnDel : lfn, !deleted);
                    if (deleted && !e.name.empty() && e.name[0] == '\xE5') e.name = "_" + e.name.substr(1);
                    e.isDir = isDir;
                    e.dosDate = Rd16(d + 24);
                    e.dosTime = Rd16(d + 22);
                    e.firstCluster = Rd16(d + 0x1A);
                    if (fatBits_ == 32) e.firstCluster |= uint32_t(Rd16(d + 0x14)) << 16;
                    e.size = Rd32(d + 0x1C);
                    e.path = path + "/" + e.name;
                    if (!deleted) {
                        bool intact = false;
                        ChainOf(e.firstCluster, &intact);
                        if (!intact) e.chainOrphaned = true;
                    }
                    out.push_back(e);
                    lfn.clear();
                    lfnDel.clear();
                    if (!deleted && isDir && !e.chainOrphaned && e.firstCluster >= 2)
                        walk(ChainOf(e.firstCluster), false, e.path);
                }
            });
        };

    if (fatBits_ == 32)
        walk(ChainOf(rootClus_), false, "");
    else
        walk({}, true, "");
    prog.Report("Scanning files", 1.0);
    return out;
}

void FatVolume::ReadFileData(const FatEntry& e, const std::function<void(const uint8_t*, size_t)>& sink) const {
    if (e.size == 0) return;
    bool intact = false;
    std::vector<uint32_t> chain;
    if (!e.deleted && e.firstCluster >= 2) chain = ChainOf(e.firstCluster, &intact);

    std::vector<uint8_t> buf((size_t(ClusterBytes())));
    uint64_t remaining = e.size;

    if (!e.deleted && intact) {
        for (uint32_t c : chain) {
            if (remaining == 0) break;
            ReadCluster(c, buf.data());
            uint64_t take = std::min<uint64_t>(remaining, ClusterBytes());
            sink(buf.data(), size_t(take));
            remaining -= take;
        }
    } else {
        // deleted (or broken chain): contiguous-cluster heuristic up to recorded size
        uint32_t c = e.firstCluster;
        while (remaining > 0 && c >= 2 && c < clusterCount_ + 2) {
            ReadCluster(c, buf.data());
            uint64_t take = std::min<uint64_t>(remaining, ClusterBytes());
            sink(buf.data(), size_t(take));
            remaining -= take;
            uint32_t v = FatGet(c);
            c = (v == 0 || IsEoc(fatBits_, v)) ? c + 1 : v;
        }
    }
}

// ---------------------------------------------------------------------------
// format
// ---------------------------------------------------------------------------
void FatVolume::FormatFat32(IDiskDevice& dev, Lba partStart, uint64_t partSectors,
                            const std::string& label, uint32_t clusterSize) {
    // A self-consistent FAT32 needs >= 65525 clusters; with 512-byte clusters that
    // requires ~67.6k sectors once the FAT itself is accounted for.
    if (partSectors < 67000) DF_THROW(InvalidParam, "FAT32 needs at least ~33 MiB");
    uint32_t spc = 1;
    if (clusterSize) {
        if (clusterSize < 512 || clusterSize > 65536 || !IsPow2(clusterSize))
            DF_THROW(InvalidParam, "invalid cluster size");
        spc = clusterSize / 512;
    } else {
        uint64_t bytes = partSectors * 512;
        if (bytes < 256 * kMiB) spc = 1;
        else if (bytes < 8 * kGiB) spc = 8;
        else if (bytes < 16 * kGiB) spc = 16;
        else if (bytes < 32 * kGiB) spc = 32;
        else spc = 64;
    }
    const uint32_t rsvd = 32;
    const uint32_t nfats = 2;
    uint32_t fatSz = 1;
    uint64_t clusters = 0;
    for (int i = 0; i < 24; ++i) {
        clusters = (partSectors - rsvd - uint64_t(nfats) * fatSz) / spc;
        if (clusters < 65525) DF_THROW(InvalidParam, "volume too small for FAT32 with this cluster size");
        uint32_t next = uint32_t((clusters + 2) * 4 / 512 + 1);
        if (next == fatSz) break;
        fatSz = next;
    }
    clusters = (partSectors - rsvd - uint64_t(nfats) * fatSz) / spc;

    Buffer boot(kSectorSize);
    uint8_t* b = boot.data();
    b[0] = 0xEB; b[1] = 0x58; b[2] = 0x90;
    std::memcpy(b + 3, "MSDOS5.0", 8);
    Wr16(b + 11, 512);
    b[13] = uint8_t(spc);
    Wr16(b + 14, rsvd);
    b[16] = nfats;
    Wr16(b + 17, 0);
    Wr16(b + 19, 0);
    b[21] = 0xF8;
    Wr16(b + 22, 0);
    Wr16(b + 24, 63);
    Wr16(b + 26, 255);
    Wr32(b + 28, uint32_t(partStart));
    Wr32(b + 32, uint32_t(partSectors));
    Wr32(b + 36, fatSz);
    Wr16(b + 40, 0);
    Wr16(b + 42, 0);
    Wr32(b + 44, 2);
    Wr16(b + 48, 1);
    Wr16(b + 50, 6);
    b[64] = 0x80;
    b[66] = 0x29;
    uint32_t serial = uint32_t(std::chrono::steady_clock::now().time_since_epoch().count());
    Wr32(b + 67, serial);
    {
        std::string lab = label.size() > 11 ? label.substr(0, 11) : label;
        while (lab.size() < 11) lab += ' ';
        std::memcpy(b + 71, lab.c_str(), 11);
    }
    std::memcpy(b + 82, "FAT32   ", 8);
    Wr16(b + 510, 0xAA55);

    uint64_t fatRegion = partStart + rsvd;
    // zero both FATs
    Buffer zeros(512 * 64, 0);
    for (uint64_t off = 0; off < uint64_t(nfats) * fatSz; off += 64) {
        uint64_t n = std::min<uint64_t>(64, uint64_t(nfats) * fatSz - off);
        dev.Write(fatRegion + off, zeros.data(), size_t(n));
    }
    {
        Buffer first(512, 0);
        Wr32(first.data() + 0, 0xFFFFFFF8);
        Wr32(first.data() + 4, 0xFFFFFFFF);
        Wr32(first.data() + 8, 0x0FFFFFFF);
        for (uint32_t f = 0; f < nfats; ++f) dev.Write(fatRegion + uint64_t(f) * fatSz, first.data(), 1);
    }

    uint64_t dataStart = partStart + rsvd + uint64_t(nfats) * fatSz;
    {
        Buffer root(size_t(spc) * 512, 0);
        if (!label.empty()) {
            uint8_t* d = root.data();
            std::string lab = label.size() > 11 ? label.substr(0, 11) : label;
            while (lab.size() < 11) lab += ' ';
            std::memcpy(d, lab.c_str(), 11);
            d[11] = 0x08;
            uint16_t dt, tm;
            DosNow(&dt, &tm);
            Wr16(d + 22, tm);
            Wr16(d + 24, dt);
        }
        dev.Write(dataStart, root.data(), spc);
    }

    auto writeFsInfo = [&](Lba where) {
        Buffer fsi(kSectorSize, 0);
        Wr32(fsi.data(), 0x41615252);
        Wr32(fsi.data() + 484, 0x61417272);
        Wr32(fsi.data() + 488, uint32_t(clusters - 1));
        Wr32(fsi.data() + 492, 3);
        Wr32(fsi.data() + 508, 0xAA550000);
        dev.Write(where, fsi.data(), 1);
    };
    writeFsInfo(partStart + 1);
    writeFsInfo(partStart + 7);

    dev.Write(partStart, boot.data(), 1);
    dev.Write(partStart + 6, boot.data(), 1);
    dev.Flush();
}

// ---------------------------------------------------------------------------
// directory helpers
// ---------------------------------------------------------------------------
std::vector<uint32_t> FatVolume::DirChainOf(const std::string& path) const {
    std::vector<uint32_t> chain;
    if (fatBits_ == 32) chain = ChainOf(rootClus_);
    std::string rest = path;
    if (!rest.empty() && rest[0] == '/') rest = rest.substr(1);
    while (!rest.empty()) {
        std::string comp = rest;
        size_t slash = rest.find('/');
        if (slash != std::string::npos) {
            comp = rest.substr(0, slash);
            rest = rest.substr(slash + 1);
        } else {
            rest.clear();
        }
        if (comp.empty()) continue;
        bool found = false;

        std::vector<uint32_t> dirChain = chain;
        if (fatBits_ != 32) {
            // walk root sectors
            Buffer sec(kSectorSize);
            for (uint64_t s = 0; s < rootDirSectors_ && !found; ++s) {
                dev_->Read(partStart_ + rsvd_ + uint64_t(nfats_) * fatSz_ + s, sec.data(), 1);
                for (size_t i = 0; i < bps_ / 32; ++i) {
                    const uint8_t* d = sec.data() + i * 32;
                    if (d[0] == 0x00) break;
                    if (d[0] == 0xE5 || (d[11] & 0x0F) == 0x0F || d[0] == '.') continue;
                    if (!(d[11] & 0x10)) continue;
                    if (LowerCopy(SfnToString(d)) == LowerCopy(comp)) {
                        uint32_t c = Rd16(d + 0x1A);
                        chain = ChainOf(c);
                        found = true;
                        break;
                    }
                }
            }
        } else {
            Buffer cluster((size_t(ClusterBytes())));
            for (uint32_t c : dirChain) {
                ReadCluster(c, cluster.data());
                for (size_t i = 0; i < ClusterBytes() / 32; ++i) {
                    const uint8_t* d = cluster.data() + i * 32;
                    if (d[0] == 0x00) break;
                    if (d[0] == 0xE5 || (d[11] & 0x0F) == 0x0F || d[0] == '.') continue;
                    if (!(d[11] & 0x10)) continue;
                    if (LowerCopy(SfnToString(d)) == LowerCopy(comp)) {
                        uint32_t cc = Rd16(d + 0x1A) | (uint32_t(Rd16(d + 0x14)) << 16);
                        chain = ChainOf(cc);
                        found = true;
                        break;
                    }
                }
                if (found) break;
            }
        }
        if (!found) return {};
    }
    return chain;
}

uint32_t FatVolume::AllocChain(uint64_t bytes, std::vector<uint32_t>& chain) {
    chain.clear();
    uint64_t need = (bytes + ClusterBytes() - 1) / ClusterBytes();
    if (need == 0) return 0;
    uint32_t prev = 0, head = 0;
    for (uint64_t c = 2; c < clusterCount_ + 2 && need > 0; ++c) {
        if (FatGet(uint32_t(c)) != 0) continue;
        if (prev == 0)
            head = uint32_t(c);
        else
            FatSet(prev, uint32_t(c));
        FatSet(uint32_t(c), fatBits_ == 32 ? 0x0FFFFFFF : (fatBits_ == 16 ? 0xFFFF : 0xFFF));
        chain.push_back(uint32_t(c));
        prev = uint32_t(c);
        --need;
    }
    if (need > 0) DF_THROW(NoSpace, "volume full");
    return head;
}

void FatVolume::FreeChain(uint32_t head) {
    for (uint32_t c : ChainOf(head)) FatSet(c, 0);
}

// Find `slots` consecutive free dirents; returns {cluster, byteOffsetOfLastSlot}
// cluster==0 means FAT12/16 root (offset is sector-encoded: s*bps + inSec).
FatVolume::DirPos FatVolume::AllocDirEntry(const std::vector<uint32_t>& chain, int slots) {
    auto findRun = [&](const uint8_t* data, size_t n) -> int {
        int run = 0;
        for (size_t i = 0; i < n; ++i) {
            uint8_t b0 = data[i * 32];
            run = (b0 == 0x00 || b0 == 0xE5) ? run + 1 : 0;
            if (run >= slots) return int(i) - slots + 1;
        }
        return -1;
    };
    if (chain.empty()) {
        if (fatBits_ == 32) DF_THROW(InvalidParam, "no directory chain for FAT32");
        Buffer sec(kSectorSize);
        for (uint64_t s = 0; s < rootDirSectors_; ++s) {
            dev_->Read(partStart_ + rsvd_ + uint64_t(nfats_) * fatSz_ + s, sec.data(), 1);
            int r = findRun(sec.data(), bps_ / 32);
            if (r >= 0) return {0, uint32_t(s * bps_ + size_t(r + slots - 1) * 32)};
        }
        DF_THROW(NoSpace, "root directory full");
    }
    Buffer cluster((size_t(ClusterBytes())));
    for (uint32_t c : chain) {
        ReadCluster(c, cluster.data());
        int r = findRun(cluster.data(), size_t(ClusterBytes() / 32));
        if (r >= 0) return {c, uint32_t(size_t(r + slots - 1) * 32)};
    }
    // extend chain
    std::vector<uint32_t> dummy;
    uint32_t nc = AllocChain(ClusterBytes(), dummy);
    FatSet(chain.back(), nc);
    Buffer zeros(size_t(ClusterBytes()), 0);
    WriteCluster(nc, zeros.data());
    return {nc, uint32_t(size_t(slots - 1) * 32)};
}

void FatVolume::ReadDirBuf(const DirPos& pos, uint8_t* buf, uint64_t bufLen) {
    if (pos.dirCluster == 0) {
        uint64_t s = pos.offset / bps_;
        dev_->Read(partStart_ + rsvd_ + uint64_t(nfats_) * fatSz_ + s, buf, 1);
    } else {
        ReadCluster(pos.dirCluster, buf);
    }
    (void)bufLen;
}
void FatVolume::WriteDirBuf(const DirPos& pos, const uint8_t* buf) {
    if (pos.dirCluster == 0) {
        uint64_t s = pos.offset / bps_;
        dev_->Write(partStart_ + rsvd_ + uint64_t(nfats_) * fatSz_ + s, buf, 1);
    } else {
        WriteCluster(pos.dirCluster, buf);
    }
}

void FatVolume::AddDir(const std::string& parentPath, const std::string& name) {
    auto chain = DirChainOf(parentPath);
    if (chain.empty() && fatBits_ == 32) DF_THROW(NotFound, "parent directory not found: " + parentPath);
    std::vector<uint32_t> newChain;
    uint32_t cluster = AllocChain(ClusterBytes(), newChain);

    uint8_t sfn[11];
    SfnFromName(name, sfn);
    DirPos pos = AllocDirEntry(chain, 1);
    Buffer buf(size_t(ClusterBytes()), 0);
    ReadDirBuf(pos, buf.data(), buf.size());
    uint32_t inBuf = pos.dirCluster ? pos.offset : (pos.offset % bps_);
    uint8_t* d = buf.data() + inBuf;
    std::memset(d, 0, 32);
    std::memcpy(d, sfn, 11);
    d[11] = 0x10;
    Wr16(d + 0x1A, uint16_t(cluster & 0xFFFF));
    if (fatBits_ == 32) Wr16(d + 0x14, uint16_t(cluster >> 16));
    uint16_t dt, tm;
    DosNow(&dt, &tm);
    Wr16(d + 22, tm);
    Wr16(d + 24, dt);
    WriteDirBuf(pos, buf.data());

    // '.' and '..' in the new directory cluster
    Buffer dcb(size_t(ClusterBytes()), 0);
    uint8_t* dot = dcb.data();
    std::memcpy(dot, ".          ", 11);
    dot[11] = 0x10;
    Wr16(dot + 0x1A, uint16_t(cluster & 0xFFFF));
    if (fatBits_ == 32) Wr16(dot + 0x14, uint16_t(cluster >> 16));
    uint8_t* dd = dcb.data() + 32;
    std::memcpy(dd, "..         ", 11);
    dd[11] = 0x10;
    uint32_t parentHead = (parentPath.empty() || parentPath == "/") ? rootClus_
                          : (fatBits_ == 32 ? chain.front() : 0);
    Wr16(dd + 0x1A, uint16_t(parentHead & 0xFFFF));
    if (fatBits_ == 32) Wr16(dd + 0x14, uint16_t(parentHead >> 16));
    WriteCluster(cluster, dcb.data());
    FlushFat();
    WriteFsInfo(FreeClusterCount(), NextFreeCluster());
}

void FatVolume::AddFile(const std::string& parentPath, const std::string& name, const uint8_t* data, size_t len) {
    auto chain = DirChainOf(parentPath);
    if (chain.empty() && fatBits_ == 32) DF_THROW(NotFound, "parent directory not found: " + parentPath);

    std::vector<uint32_t> fileChain;
    uint32_t head = AllocChain(len, fileChain);
    {
        std::vector<uint8_t> cb(size_t(ClusterBytes()), 0);
        uint64_t off = 0;
        for (uint32_t c : fileChain) {
            uint64_t take = std::min<uint64_t>(ClusterBytes(), len - off);
            std::memset(cb.data(), 0, cb.size());
            if (take) std::memcpy(cb.data(), data + off, size_t(take));
            WriteCluster(c, cb.data());
            off += take;
        }
    }

    uint8_t sfn[11];
    SfnFromName(name, sfn);
    bool needLfn = LowerCopy(name) != LowerCopy(SfnToString(sfn));
    int lfnCount = 0;
    if (needLfn) lfnCount = int((U8ToU16(name).size() + 12) / 13);

    DirPos pos = AllocDirEntry(chain, lfnCount + 1);
    uint32_t firstSlotOffset = pos.offset - uint32_t(lfnCount * 32);
    DirPos firstPos{pos.dirCluster, firstSlotOffset};

    auto writeAt = [&](uint32_t slotFromEnd, const uint8_t* entry) {
        // slot 0 = first (LFN) slot; write into same buffer unit
        DirPos p{pos.dirCluster, firstSlotOffset + slotFromEnd * 32};
        uint32_t unit = p.dirCluster ? size_t(ClusterBytes()) : bps_;
        Buffer buf(unit, 0);
        ReadDirBuf(p, buf.data(), buf.size());
        uint32_t inBuf = p.dirCluster ? p.offset : (p.offset % bps_);
        std::memcpy(buf.data() + inBuf, entry, 32);
        WriteDirBuf(p, buf.data());
    };

    uint8_t ck = LfnChecksum(sfn);
    if (needLfn) {
        std::u16string u16 = U8ToU16(name);
        for (int i = 0; i < lfnCount; ++i) {
            uint8_t ent[32];
            std::memset(ent, 0, 32);
            uint8_t seq = uint8_t(i + 1);
            if (i == lfnCount - 1) seq |= 0x40;
            ent[0] = seq;
            ent[11] = 0x0F;
            ent[13] = ck;
            static const int offs[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
            for (int k = 0; k < 13; ++k) {
                uint16_t ch = 0xFFFF;
                if (size_t(i * 13 + k) < u16.size()) ch = uint16_t(u16[size_t(i * 13 + k)]);
                else if (size_t(i * 13 + k) == u16.size()) ch = 0;
                Wr16(ent + offs[k], ch);
            }
            // spec order: highest sequence number first (closest to the SFN)
            writeAt(uint32_t(lfnCount - 1 - i), ent);
        }
    }
    {
        uint8_t ent[32];
        std::memset(ent, 0, 32);
        std::memcpy(ent, sfn, 11);
        if (ent[0] == 0xE5) ent[0] = 0x05;
        ent[11] = 0x20;
        Wr16(ent + 0x1A, uint16_t(head & 0xFFFF));
        if (fatBits_ == 32) Wr16(ent + 0x14, uint16_t(head >> 16));
        Wr32(ent + 0x1C, uint32_t(len));
        uint16_t dt, tm;
        DosNow(&dt, &tm);
        Wr16(ent + 22, tm);
        Wr16(ent + 24, dt);
        writeAt(uint32_t(lfnCount), ent);  // SFN is the last slot
    }
    FlushFat();
    WriteFsInfo(FreeClusterCount(), NextFreeCluster());
}

void FatVolume::DeleteFile(const std::string& path) {
    std::string dir = "/", name = path;
    size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        dir = path.substr(0, slash);
        if (dir.empty()) dir = "/";
        name = path.substr(slash + 1);
    }
    auto chain = DirChainOf(dir);
    if (chain.empty() && fatBits_ == 32) DF_THROW(NotFound, "directory not found: " + dir);
    uint8_t wantSfn[11];
    SfnFromName(name, wantSfn);  // compare SFN-to-SFN so long names also match
    Buffer cluster((size_t(ClusterBytes())));
    for (uint32_t c : chain) {
        ReadCluster(c, cluster.data());
        for (size_t i = 0; i < ClusterBytes() / 32; ++i) {
            uint8_t* d = cluster.data() + i * 32;
            if (d[0] == 0x00) break;
            if (d[0] == 0xE5) continue;
            uint8_t attr = d[11];
            if ((attr & 0x0F) == 0x0F) continue;
            if (std::memcmp(d, wantSfn, 11) != 0) continue;
            {
                d[0] = 0xE5;
                for (size_t j = i; j > 0; --j) {
                    uint8_t* p = cluster.data() + (j - 1) * 32;
                    if ((p[11] & 0x3F) == 0x0F)
                        p[0] = 0xE5;
                    else
                        break;
                }
                WriteCluster(c, cluster.data());
                uint32_t first = Rd16(d + 0x1A);
                if (fatBits_ == 32) first |= uint32_t(Rd16(d + 0x14)) << 16;
                FreeChain(first);
                FlushFat();
                WriteFsInfo(FreeClusterCount(), NextFreeCluster());
                return;
            }
        }
    }
    DF_THROW(NotFound, "file not found: " + path);
}

void FatVolume::SetLabel(const std::string& label) {
    std::string lab = label.size() > 11 ? label.substr(0, 11) : label;
    while (lab.size() < 11) lab += ' ';
    Buffer b(kSectorSize);
    dev_->Read(partStart_, b.data(), 1);
    std::memcpy(b.data() + (fatBits_ == 32 ? 71 : 43), lab.c_str(), 11);
    dev_->Write(partStart_, b.data(), 1);
    if (fatBits_ == 32 && bkBootSec_) dev_->Write(partStart_ + bkBootSec_, b.data(), 1);
    label_ = label;
    // refresh root VOL entry
    if (fatBits_ == 32) {
        auto root = ChainOf(rootClus_);
        if (!root.empty()) {
            Buffer cl((size_t(ClusterBytes())));
            ReadCluster(root.front(), cl.data());
            uint8_t* d = cl.data();
            if ((d[11] & 0x08) && !(d[11] & 0x10) && (d[11] & 0x3F) != 0x0F && d[0] != 0xE5 && d[0] != 0x00) {
                std::string lab11 = lab;
                std::memcpy(d, lab11.c_str(), 11);
                WriteCluster(root.front(), cl.data());
            }
        }
    }
}

void FatVolume::WriteFsInfo(uint64_t freeCount, uint32_t nextFree) {
    if (fatBits_ != 32 || !fsInfoSec_) return;
    auto write = [&](Lba where) {
        Buffer fsi(kSectorSize, 0);
        Wr32(fsi.data(), 0x41615252);
        Wr32(fsi.data() + 484, 0x61417272);
        Wr32(fsi.data() + 488, uint32_t(freeCount));
        Wr32(fsi.data() + 492, nextFree);
        Wr32(fsi.data() + 508, 0xAA550000);
        dev_->Write(where, fsi.data(), 1);
    };
    write(partStart_ + fsInfoSec_);
    if (bkBootSec_ && fsInfoSec_ == 1) write(partStart_ + bkBootSec_ + 1);
}

// ---------------------------------------------------------------------------
// resize / compaction (FAT32)
// ---------------------------------------------------------------------------
uint64_t FatVolume::MinSectors() const {
    if (!IsFat32()) DF_THROW(NotSupported, "resize only supported for FAT32");
    int64_t maxAlloc = -1;
    for (int64_t c = int64_t(clusterCount_) + 1; c >= 2; --c)
        if (FatGet(uint32_t(c)) != 0) {
            maxAlloc = c;
            break;
        }
    if (maxAlloc < 2) return rsvd_ + 2 * fatSz_ + spc_;
    uint64_t neededClusters = uint64_t(maxAlloc) - 2 + 1;
    uint64_t fatSz2 = ((neededClusters + 2) * 4 + 511) / 512 + 4;
    return rsvd_ + uint64_t(nfats_) * fatSz2 + neededClusters * spc_;
}

void FatVolume::Resize(uint64_t newSectors, const Progress& prog) {
    if (!IsFat32()) DF_THROW(NotSupported, "resize only supported for FAT32");
    newSectors = RoundDown(newSectors, spc_);
    if (newSectors < MinSectors()) DF_THROW(NoSpace, "target size smaller than used data");
    if (newSectors == totalSectors_) return;

    prog.Report("Computing resize plan", 0.05);
    uint32_t newFatsz = 1;
    uint64_t newClusters = 0;
    for (int i = 0; i < 24; ++i) {
        newClusters = (newSectors - rsvd_ - uint64_t(nfats_) * newFatsz) / spc_;
        uint32_t next = uint32_t((newClusters + 2) * 4 / 512 + 1);
        if (next == newFatsz) break;
        newFatsz = next;
    }
    newClusters = (newSectors - rsvd_ - uint64_t(nfats_) * newFatsz) / spc_;

    if (newClusters < clusterCount_) CompactTo(newClusters, prog);

    uint64_t dataSectors = clusterCount_ * spc_;  // clusterCount_ still OLD after CompactTo
    uint64_t dataStartOld = rsvd_ + uint64_t(nfats_) * fatSz_;
    uint64_t dataStartNew = rsvd_ + uint64_t(nfats_) * newFatsz;

    prog.Report("Saving FAT snapshot", 0.2);
    Buffer fatSnap(size_t(fatSz_) * 512);
    {
        uint64_t remaining = uint64_t(fatSz_);
        uint64_t off = 0;
        while (remaining) {
            uint64_t n = std::min<uint64_t>(256, remaining);
            dev_->Read(partStart_ + rsvd_ + off, fatSnap.data() + off * 512, size_t(n));
            off += n;
            remaining -= n;
        }
    }

    prog.Report("Moving data region", 0.35);
    if (dataStartNew != dataStartOld)
        CopySectors(*dev_, partStart_ + dataStartOld, partStart_ + dataStartNew, dataSectors, prog);

    prog.Report("Writing new FAT", 0.7);
    {
        Buffer zeros(512, 0);
        for (uint64_t i = 0; i < newFatsz; ++i) {
            const uint8_t* src = (i < fatSz_) ? fatSnap.data() + size_t(i * 512) : zeros.data();
            for (uint8_t f = 0; f < nfats_; ++f)
                dev_->Write(partStart_ + rsvd_ + uint64_t(f) * newFatsz + i, src, 1);
        }
    }

    fatSz_ = newFatsz;
    clusterCount_ = newClusters;
    totalSectors_ = newSectors;
    dataStartSector_ = dataStartNew;
    fatCache_.clear();
    chainEpoch_.clear();
    {
        Buffer b(kSectorSize);
        dev_->Read(partStart_, b.data(), 1);
        Wr32(b.data() + 32, uint32_t(newSectors));
        Wr32(b.data() + 36, newFatsz);
        dev_->Write(partStart_, b.data(), 1);
        if (bkBootSec_) dev_->Write(partStart_ + bkBootSec_, b.data(), 1);
    }
    WriteFsInfo(FreeClusterCount(), NextFreeCluster());
    prog.Report("Finalizing", 1.0);
}

void FatVolume::CompactTo(uint64_t newClusterCount, const Progress& prog) {
    if (!IsFat32()) DF_THROW(NotSupported, "compaction only supported for FAT32");
    if (newClusterCount >= clusterCount_) return;
    if (newClusterCount < 2) DF_THROW(InvalidParam, "cluster limit too small");

    prog.Report("Analyzing cluster usage", 0.05);

    // ---- 1. walk tree: mark reachable clusters, record dirent patches ----
    std::vector<bool> reachable(size_t(clusterCount_) + 2, false);
    std::vector<bool> isDirCluster(size_t(clusterCount_) + 2, false);
    struct Patch {
        uint32_t offsetInCluster;
        uint32_t oldStart;
    };
    std::unordered_map<uint32_t, std::vector<Patch>> patches;

    std::function<void(const std::vector<uint32_t>&)> markDir = [&](const std::vector<uint32_t>& chain) {
        for (uint32_t c : chain) {
            reachable[c] = true;
            isDirCluster[c] = true;
        }
        Buffer cl((size_t(ClusterBytes())));
        for (uint32_t c : chain) {
            ReadCluster(c, cl.data());
            size_t n = size_t(ClusterBytes() / 32);
            for (size_t i = 0; i < n; ++i) {
                const uint8_t* d = cl.data() + i * 32;
                if (d[0] == 0x00) break;
                if (d[0] == 0xE5) continue;
                uint8_t attr = d[11];
                if ((attr & 0x0F) == 0x0F) continue;
                if ((attr & 0x08) && !(attr & 0x10)) continue;
                if (d[0] == '.') {
                    uint32_t target = Rd16(d + 0x1A) | (uint32_t(Rd16(d + 0x14)) << 16);
                    patches[c].push_back({uint32_t(i * 32), target});
                    continue;
                }
                uint32_t first = Rd16(d + 0x1A) | (uint32_t(Rd16(d + 0x14)) << 16);
                patches[c].push_back({uint32_t(i * 32), first});
                bool isDir = (attr & 0x10) != 0;
                if (first >= 2 && first < clusterCount_ + 2) {
                    if (isDir)
                        markDir(ChainOf(first));
                    else
                        for (uint32_t c2 : ChainOf(first)) reachable[c2] = true;
                }
            }
        }
    };
    markDir(ChainOf(rootClus_));

    // ---- 2. monotone map (new indices are contiguous => FAT output is a stream) ----
    std::vector<uint32_t> clusterMap(size_t(clusterCount_) + 2, 0);
    uint32_t nextNew = 2;
    uint64_t reachableCount = 0;
    for (uint64_t c = 2; c < clusterCount_ + 2; ++c)
        if (reachable[c]) {
            clusterMap[c] = nextNew++;
            ++reachableCount;
        }
    if (clusterMap[rootClus_] != 2) DF_THROW(Internal, "root cluster must stay at 2 after compaction");
    if (nextNew - 1 >= newClusterCount) DF_THROW(NoSpace, "cannot compact below used cluster count");

    // ---- 3. move clusters (ascending old order; writes always at/below reads) ----
    prog.Report("Moving clusters", 0.15);
    {
        Buffer cl((size_t(ClusterBytes())));
        for (uint64_t c = 2; c < clusterCount_ + 2; ++c) {
            prog.CheckCancel();
            if ((c & 0xFF) == 0) prog.Report("Moving clusters", 0.15 + 0.55 * double(c) / double(clusterCount_));
            if (!reachable[c]) continue;
            uint32_t nc = clusterMap[c];
            if (nc == c) continue;
            ReadCluster(uint32_t(c), cl.data());
            WriteCluster(nc, cl.data());
        }
    }

    // ---- 3b. patch dirents in ALL directory clusters (at their final locations) ----
    prog.Report("Updating directory entries", 0.72);
    {
        Buffer cl((size_t(ClusterBytes())));
        for (auto& kv : patches) {
            uint32_t newLoc = clusterMap[kv.first];
            ReadCluster(newLoc, cl.data());
            for (auto& pt : kv.second) {
                uint8_t* d = cl.data() + pt.offsetInCluster;
                uint32_t newStart = (pt.oldStart >= 2 && pt.oldStart < clusterCount_ + 2 && reachable[pt.oldStart])
                                        ? clusterMap[pt.oldStart]
                                        : pt.oldStart;
                Wr16(d + 0x1A, uint16_t(newStart & 0xFFFF));
                Wr16(d + 0x14, uint16_t(newStart >> 16));
            }
            WriteCluster(newLoc, cl.data());
        }
    }

    // ---- 4. rebuild FAT: append-only stream of entries 0,1,2.. (writes below reads) ----
    prog.Report("Rebuilding FAT", 0.8);
    {
        const uint64_t kWindow = 65536;  // entries per window
        Buffer oldVals(size_t(kWindow) * 4 + kSectorSize);
        std::vector<uint8_t> outSec(512, 0);
        uint64_t flushedEntries = 0;  // entries already written
        uint32_t inSec = 0;           // next free slot in outSec

        auto flushSector = [&]() {
            for (uint8_t f = 0; f < nfats_; ++f)
                dev_->Write(partStart_ + rsvd_ + uint64_t(f) * fatSz_ + flushedEntries / 128, outSec.data(), 1);
            std::memset(outSec.data(), 0, outSec.size());
            flushedEntries += 128;
            inSec = 0;
        };
        auto put = [&](uint32_t value) {
            Wr32(outSec.data() + inSec * 4, value);
            if (++inSec == 128) flushSector();
        };

        put(0xFFFFFFF8);
        put(0xFFFFFFFF);
        for (uint64_t base = 2; base < clusterCount_ + 2; base += kWindow) {
            uint64_t n = std::min<uint64_t>(kWindow, clusterCount_ + 2 - base);
            // window covers FAT bytes [base*4, (base+n)*4); the containing sectors may
            // start up to 511 bytes earlier — track the misalignment explicitly.
            const uint64_t mis = (base * 4) % kSectorSize;
            const uint64_t startSec = (base * 4) / kSectorSize;
            const uint64_t sectors = (mis + n * 4 + kSectorSize - 1) / kSectorSize;
            for (uint64_t s = 0; s < sectors; ++s)
                dev_->Read(partStart_ + rsvd_ + startSec + s, oldVals.data() + s * kSectorSize, 1);
            for (uint64_t k = 0; k < n; ++k) {
                uint64_t c = base + k;
                if (!reachable[c]) continue;
                uint32_t v = Rd32(oldVals.data() + mis + size_t(k) * 4) & 0x0FFFFFFFu;
                uint32_t nv;
                if (v >= 0x0FFFFFF8)
                    nv = 0x0FFFFFFF;
                else if (v >= 2 && v < clusterCount_ + 2 && reachable[v])
                    nv = clusterMap[v];
                else
                    DF_THROW(Corrupt, "chain references unreachable cluster");
                put(nv);
            }
        }
        // zero the tail beyond reachable clusters so stale entries don't read as allocated
        while (flushedEntries + inSec < clusterCount_ + 2) put(0);
        if (inSec) flushSector();
    }

    prog.Report("Updating FSInfo", 0.95);
    fatCache_.clear();
    chainEpoch_.clear();
    // NOTE: clusterCount_ stays at the OLD value here; Resize() applies the new
    // BPB values after the region move. FSInfo free count uses old cluster space.
    WriteFsInfo(clusterCount_ - 2 - reachableCount, 3);
    prog.Report("Compaction complete", 1.0);
}

}  // namespace df
