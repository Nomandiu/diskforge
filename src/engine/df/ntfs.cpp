#include "ntfs.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace df {

// ---------------------------------------------------------------------------
// attribute type constants
// ---------------------------------------------------------------------------
constexpr uint32_t kAttrStandardInfo = 0x10;
constexpr uint32_t kAttrFileName = 0x30;
constexpr uint32_t kAttrVolumeName = 0x60;
constexpr uint32_t kAttrVolumeInfo = 0x70;
constexpr uint32_t kAttrData = 0x80;
constexpr uint32_t kAttrIndexRoot = 0x90;
constexpr uint32_t kAttrIndexAlloc = 0xA0;
constexpr uint32_t kAttrBitmap = 0xB0;
constexpr uint32_t kAttrEnd = 0xFFFFFFFF;

// ---------------------------------------------------------------------------
// runlist decode
// ---------------------------------------------------------------------------
bool NtfsVolume::DecodeRunlist(const uint8_t* p, size_t len, std::vector<Run>& runs) {
    runs.clear();
    size_t i = 0;
    int64_t lcn = 0;
    uint64_t vcn = 0;
    while (i < len) {
        uint8_t hdr = p[i];
        if (hdr == 0x00) break;  // end
        int lenBytes = hdr & 0x0F;
        int offBytes = (hdr >> 4) & 0x0F;
        if (lenBytes == 0 || lenBytes > 8 || offBytes > 8) return false;
        ++i;
        if (i + size_t(lenBytes + offBytes) > len) return false;
        uint64_t count = 0;
        for (int b = 0; b < lenBytes; ++b) count |= uint64_t(p[i + b]) << (8 * b);
        i += size_t(lenBytes);
        if (count == 0) return false;
        if (offBytes == 0) {
            runs.push_back({vcn, UINT64_MAX, count});  // sparse
        } else {
            int64_t delta = 0;
            for (int b = 0; b < offBytes; ++b) delta |= int64_t(uint64_t(p[i + b]) << (8 * b));
            int shift = 64 - 8 * offBytes;
            delta = (delta << shift) >> shift;  // sign extend
            i += size_t(offBytes);
            lcn += delta;
            if (lcn < 0) return false;
            runs.push_back({vcn, uint64_t(lcn), count});
        }
        vcn += count;
    }
    return true;
}

// ---------------------------------------------------------------------------
// open
// ---------------------------------------------------------------------------
NtfsVolume NtfsVolume::Open(IDiskDevice* dev, Lba partStart, uint64_t partSectors) {
    NtfsVolume v;
    v.dev_ = dev;
    v.partStart_ = partStart;
    Buffer s(kSectorSize);
    dev->Read(partStart, s.data(), 1);
    if (std::memcmp(s.data() + 3, "NTFS    ", 8) != 0)
        DF_THROW(Parse, "not an NTFS volume at LBA " + std::to_string(partStart));

    v.bps_ = Rd16(s.data() + 11);
    v.spc_ = s[13];
    if (v.bps_ == 0 || v.spc_ == 0 || (v.bps_ & (v.bps_ - 1))) DF_THROW(Parse, "bad NTFS BPB");
    v.totalSectors_ = Rd64(s.data() + 0x28);
    if (v.totalSectors_ == 0) v.totalSectors_ = Rd32(s.data() + 32);
    if (partSectors && v.totalSectors_ > partSectors) v.totalSectors_ = partSectors;
    v.mftLcn_ = Rd64(s.data() + 0x30);
    v.mft2Lcn_ = Rd64(s.data() + 0x38);
    int8_t recCode = int8_t(s[0x40]);
    if (recCode < 0)
        v.recordSize_ = 1u << (-recCode);
    else
        v.recordSize_ = uint32_t(recCode) * v.spc_ * v.bps_;
    v.serial_ = Rd32(s.data() + 0x48);
    if (v.recordSize_ < 512 || v.recordSize_ > 65536 || (v.recordSize_ & (v.recordSize_ - 1)))
        DF_THROW(Parse, "bad NTFS MFT record size");

    // read $MFT record 0 for its $DATA runs
    Lba mftLba = partStart + v.mftLcn_ * v.spc_;
    Buffer rec(v.recordSize_);
    dev->Read(mftLba, rec.data(), v.recordSize_ / kSectorSize);
    if (!v.ApplyFixups(rec)) DF_THROW(Parse, "cannot apply fixups to $MFT record");

    uint32_t used = Rd32(rec.data() + 0x18);
    uint32_t off = Rd16(rec.data() + 0x14);
    bool found = false;
    while (off + 8 <= used && off + 8 <= v.recordSize_) {
        uint32_t type = Rd32(rec.data() + off);
        if (type == kAttrEnd) break;
        uint32_t attrLen = Rd32(rec.data() + off + 4);
        if (attrLen < 0x18 || off + attrLen > v.recordSize_) break;
        if (type == kAttrData && rec[off + 8] == 1 && rec[off + 9] == 0) {
            // non-resident unnamed $DATA: the $MFT data runlist
            uint16_t runOff = Rd16(rec.data() + off + 0x20);
            uint64_t realSize = Rd64(rec.data() + off + 0x30);
            if (!DecodeRunlist(rec.data() + off + runOff, attrLen - runOff, v.mftRuns_))
                DF_THROW(Parse, "malformed $MFT runlist");
            v.mftRecordCount_ = realSize / v.recordSize_;
            found = true;
            break;
        }
        off += attrLen;
    }
    if (!found) DF_THROW(NotSupported, "$MFT with resident $DATA is not supported");
    return v;
}

bool NtfsVolume::Detect(IDiskDevice& dev, Lba partStart) {
    try {
        Buffer s(kSectorSize);
        dev.Read(partStart, s.data(), 1);
        return std::memcmp(s.data() + 3, "NTFS    ", 8) == 0;
    } catch (const DfError&) {
        return false;
    }
}

bool NtfsVolume::ApplyFixups(Buffer& rec) const {
    if (rec.size() < 512 || (rec.size() % kSectorSize)) return false;
    if (std::memcmp(rec.data(), "FILE", 4) != 0) return false;
    uint16_t usaOfs = Rd16(rec.data() + 4);
    uint16_t usaCount = Rd16(rec.data() + 6);
    if (usaCount < 1 || usaOfs < 0x30 || size_t(usaOfs) + size_t(usaCount) * 2 > rec.size()) return false;
    if (size_t(usaCount) - 1 != rec.size() / kSectorSize) return false;
    uint16_t usn = Rd16(rec.data() + usaOfs);
    size_t sectors = rec.size() / kSectorSize;
    for (size_t i = 0; i < sectors; ++i) {
        uint8_t* secEnd = rec.data() + i * kSectorSize + kSectorSize - 2;
        if (Rd16(secEnd) != usn) return false;
        Wr16(secEnd, Rd16(rec.data() + usaOfs + 2 + uint32_t(i) * 2));
    }
    return true;
}

bool NtfsVolume::ReadMftCluster(uint64_t vcn) const {
    if (cachedVcn_ == vcn && !cachedCluster_.empty()) return true;
    for (auto& r : mftRuns_) {
        if (vcn < r.vcn || vcn >= r.vcn + r.count) continue;
        if (r.lcn == UINT64_MAX) return false;  // sparse
        cachedCluster_.resize(size_t(spc_) * bps_);
        try {
            dev_->Read(partStart_ + (r.lcn + (vcn - r.vcn)) * spc_, cachedCluster_.data(), spc_);
        } catch (const DfError&) {
            cachedCluster_.clear();
            return false;
        }
        cachedVcn_ = vcn;
        return true;
    }
    return false;
}

bool NtfsVolume::ReadMftRecord(uint64_t index, Buffer& out) const {
    if (mftRecordCount_ && index >= mftRecordCount_) return false;
    out.assign(recordSize_, 0);
    if (mftRuns_.empty()) return false;
    uint64_t cb = ClusterBytes();
    uint64_t startByte = index * recordSize_;
    uint64_t endByte = startByte + recordSize_;
    for (uint64_t v = startByte / cb; v < (endByte + cb - 1) / cb; ++v) {
        if (!ReadMftCluster(v)) return false;
        uint64_t clusterStart = v * cb;
        uint64_t inStart = std::max<uint64_t>(clusterStart, startByte) - clusterStart;
        uint64_t inEnd = std::min<uint64_t>(clusterStart + cb, endByte) - clusterStart;
        std::memcpy(out.data() + (clusterStart + inStart - startByte),
                    cachedCluster_.data() + inStart, size_t(inEnd - inStart));
    }
    if (std::memcmp(out.data(), "FILE", 4) != 0) return false;
    return ApplyFixups(out);
}

// ---------------------------------------------------------------------------
// bitmap / label
// ---------------------------------------------------------------------------
uint64_t NtfsVolume::UsedBytes() const {
    Buffer rec;
    if (!ReadMftRecord(6, rec)) return 0;
    uint32_t used = Rd32(rec.data() + 0x18);
    uint32_t off = Rd16(rec.data() + 0x14);
    uint64_t usedClusters = 0;
    while (off + 8 <= used && off + 8 <= recordSize_) {
        uint32_t type = Rd32(rec.data() + off);
        if (type == kAttrEnd) break;
        uint32_t attrLen = Rd32(rec.data() + off + 4);
        if (attrLen < 0x18 || off + attrLen > recordSize_) break;
        if (type == kAttrBitmap && rec[off + 9] == 0) {
            if (rec[off + 0x18]) {
                // non-resident bitmap
                uint16_t runOff = Rd16(rec.data() + off + 0x20);
                uint64_t allocSize = Rd64(rec.data() + off + 0x28);
                std::vector<Run> runs;
                if (!DecodeRunlist(rec.data() + off + runOff, attrLen - runOff, runs)) break;
                Buffer data(size_t(allocSize), 0);
                uint64_t got = 0;
                for (auto& r : runs) {
                    if (got >= allocSize) break;
                    uint64_t bytes = r.count * spc_ * bps_;
                    if (r.lcn == UINT64_MAX) {
                        got += bytes;
                        continue;
                    }
                    uint64_t want = std::min<uint64_t>(bytes, allocSize - got);
                    dev_->Read(partStart_ + r.lcn * spc_, data.data() + got, size_t(want / kSectorSize));
                    got += bytes;
                }
                for (uint64_t i = 0; i < allocSize; ++i)
                    for (int b = 0; b < 8; ++b)
                        if (data[size_t(i)] & (1 << b)) ++usedClusters;
            } else {
                uint32_t vlen = Rd32(rec.data() + off + 0x10);
                uint16_t voff = Rd16(rec.data() + off + 0x14);
                for (uint32_t i = 0; i + off + voff < recordSize_ && i < vlen; ++i)
                    for (int b = 0; b < 8; ++b)
                        if (rec[off + voff + i] & (1 << b)) ++usedClusters;
            }
            break;
        }
        off += attrLen;
    }
    return usedClusters * ClusterBytes();
}

uint64_t NtfsVolume::FreeBytes() const {
    uint64_t total = totalSectors_ * bps_;
    uint64_t used = UsedBytes();
    return used < total ? total - used : 0;
}

std::string NtfsVolume::Label() const {
    if (!label_.empty()) return label_;
    Buffer rec;
    if (!ReadMftRecord(3, rec)) return label_;
    uint32_t used = Rd32(rec.data() + 0x18);
    uint32_t off = Rd16(rec.data() + 0x14);
    while (off + 8 <= used && off + 8 <= recordSize_) {
        uint32_t type = Rd32(rec.data() + off);
        if (type == kAttrEnd) break;
        uint32_t attrLen = Rd32(rec.data() + off + 4);
        if (attrLen < 0x18 || off + attrLen > recordSize_) break;
        if (type == kAttrVolumeName && rec[off + 8] == 0) {
            uint32_t vlen = Rd32(rec.data() + off + 0x10);
            uint16_t voff = Rd16(rec.data() + off + 0x14);
            std::u16string u16;
            for (uint32_t i = 0; i + 1 < vlen && size_t(voff) + i + 1 < recordSize_; i += 2)
                u16 += char16_t(Rd16(rec.data() + off + voff + i));
            label_ = U16ToU8(u16);
            break;
        }
        off += attrLen;
    }
    return label_;
}

// ---------------------------------------------------------------------------
// enumeration
// ---------------------------------------------------------------------------
std::vector<NtfsEntry> NtfsVolume::Enumerate(bool includeDeleted, const Progress& prog) const {
    struct RawEntry {
        NtfsEntry e;
        bool alive = false;
    };
    std::unordered_map<uint64_t, RawEntry> byRef;

    // label from $Volume (record 3)
    {
        Buffer rec;
        if (ReadMftRecord(3, rec)) {
            uint32_t used = Rd32(rec.data() + 0x18);
            uint32_t off = Rd16(rec.data() + 0x14);
            while (off + 8 <= used && off + 8 <= recordSize_) {
                uint32_t type = Rd32(rec.data() + off);
                if (type == kAttrEnd) break;
                uint32_t attrLen = Rd32(rec.data() + off + 4);
                if (attrLen < 0x18 || off + attrLen > recordSize_) break;
                if (type == kAttrVolumeName && rec[off + 8] == 0) {
                    uint32_t vlen = Rd32(rec.data() + off + 0x10);
                    uint16_t voff = Rd16(rec.data() + off + 0x14);
                    std::u16string u16;
                    for (uint32_t i = 0; i + 1 < vlen && size_t(voff) + i + 1 < recordSize_; i += 2)
                        u16 += char16_t(Rd16(rec.data() + off + voff + i));
                    label_ = U16ToU8(u16);
                    break;
                }
                off += attrLen;
            }
        }
    }

    for (uint64_t idx = 0; idx < mftRecordCount_; ++idx) {
        if ((idx & 0x3F) == 0) {
            prog.Report("Scanning MFT", mftRecordCount_ ? double(idx) / double(mftRecordCount_) : 0.0);
            prog.CheckCancel();
        }
        Buffer rec;
        if (!ReadMftRecord(idx, rec)) continue;
        uint16_t flags = Rd16(rec.data() + 0x16);
        bool inUse = (flags & 0x01) != 0;
        bool isDir = (flags & 0x02) != 0;
        if (!inUse && !includeDeleted) continue;

        RawEntry re;
        re.alive = inUse;
        re.e.mftRef = idx;
        re.e.deleted = !inUse;
        re.e.isDir = isDir;
        if (Rd64(rec.data() + 0x20) != 0) continue;  // extension record

        uint32_t used = Rd32(rec.data() + 0x18);
        uint32_t off = Rd16(rec.data() + 0x14);
        bool haveName = false;
        bool haveData = false;
        while (off + 8 <= used && off + 8 <= recordSize_) {
            uint32_t type = Rd32(rec.data() + off);
            if (type == kAttrEnd) break;
            uint32_t attrLen = Rd32(rec.data() + off + 4);
            if (attrLen < 0x18 || off + attrLen > recordSize_) break;
            uint8_t nonRes = rec[off + 8];
            uint8_t nameLen = rec[off + 9];
            uint16_t attrFlags = Rd16(rec.data() + off + 0x0C);

            if (type == kAttrFileName && !nonRes) {
                uint32_t vlen = Rd32(rec.data() + off + 0x10);
                uint16_t voff = Rd16(rec.data() + off + 0x14);
                const uint8_t* fn = rec.data() + off + voff;
                if (vlen >= 0x42 && size_t(voff) + vlen <= recordSize_) {
                    uint8_t nameType = fn[0x41];
                    uint8_t nameLen2 = fn[0x40];
                    bool prefer = nameType == 0x2 || nameType == 0x1 || !haveName;
                    if (prefer && nameLen2) {
                        re.e.parentRef = Rd64(fn) & 0xFFFFFFFFFFFFull;
                        std::u16string u16;
                        for (int i = 0; i < nameLen2; ++i) u16 += char16_t(Rd16(fn + 0x42 + i * 2));
                        re.e.name = U16ToU8(u16);
                        haveName = true;
                    }
                }
            } else if (type == kAttrData && nameLen == 0) {
                haveData = true;
                if (nonRes) {
                    re.e.resident = false;
                    re.e.allocSize = Rd64(rec.data() + off + 0x28);
                    re.e.size = Rd64(rec.data() + off + 0x30);
                    re.e.compressed = (attrFlags & 0x0001) != 0;
                    re.e.sparse = (attrFlags & 0x8000) != 0;
                } else {
                    re.e.resident = true;
                    re.e.size = Rd32(rec.data() + off + 0x10);
                    re.e.allocSize = re.e.size;
                }
            }
            off += attrLen;
        }

        if (!haveName || re.e.name.empty()) continue;
        re.e.hasData = haveData;
        byRef[idx] = re;
    }

    // ---- path resolution (parents via $FILE_NAME; root = record 5) ----
    std::unordered_map<uint64_t, std::string> memo;
    std::unordered_set<uint64_t> visiting;
    // resolve the parent chain of `ref`; deleted records may still resolve when their
    // ancestors are alive (deleted files keep their original full path for display)
    std::function<bool(uint64_t, std::string&, int)> resolve = [&](uint64_t ref, std::string& outPath, int depth) -> bool {
        if (ref == 5) {
            outPath = "";
            return true;
        }
        auto it = byRef.find(ref);
        if (it == byRef.end()) return false;
        auto m = memo.find(ref);
        if (m != memo.end()) {
            outPath = m->second;
            return true;
        }
        if (depth > 64 || visiting.count(ref)) return false;  // cycle/depth guard
        visiting.insert(ref);
        std::string pp;
        bool ok = resolve(it->second.e.parentRef, pp, depth + 1);
        visiting.erase(ref);
        if (!ok) return false;
        std::string p = pp + "/" + it->second.e.name;
        memo[ref] = p;
        outPath = p;
        return true;
    };

    std::vector<NtfsEntry> out;
    out.reserve(byRef.size());
    for (auto& [ref, re] : byRef) {
        if (ref == 5) continue;  // root itself
        std::string path;
        if (resolve(ref, path, 0)) {
            // hide NTFS metadata ($MFT etc. live at root or under $-prefixed ancestors)
            if (path.find("/$") != std::string::npos) continue;
            re.e.path = path;
        } else {
            re.e.orphaned = true;
            re.e.path = "/Lost Files/" + re.e.name;
        }
        out.push_back(re.e);
    }
    prog.Report("Scanning MFT", 1.0);
    return out;
}

// ---------------------------------------------------------------------------
// data extraction
// ---------------------------------------------------------------------------
void NtfsVolume::ReadFileData(const NtfsEntry& e, const std::function<void(const uint8_t*, size_t)>& sink) const {
    Buffer rec;
    if (!ReadMftRecord(e.mftRef, rec)) DF_THROW(Corrupt, "cannot read MFT record " + std::to_string(e.mftRef));

    uint32_t used = Rd32(rec.data() + 0x18);
    uint32_t off = Rd16(rec.data() + 0x14);
    while (off + 8 <= used && off + 8 <= recordSize_) {
        uint32_t type = Rd32(rec.data() + off);
        if (type == kAttrEnd) break;
        uint32_t attrLen = Rd32(rec.data() + off + 4);
        if (attrLen < 0x18 || off + attrLen > recordSize_) break;
        uint8_t nonRes = rec[off + 8];
        uint8_t nameLen = rec[off + 9];

        if (type == kAttrData && nameLen == 0) {
            if (nonRes == 0) {
                uint32_t vlen = Rd32(rec.data() + off + 0x10);
                uint16_t voff = Rd16(rec.data() + off + 0x14);
                uint64_t take = std::min<uint64_t>(vlen, e.size);
                sink(rec.data() + off + voff, size_t(take));
            } else {
                uint16_t runOff = Rd16(rec.data() + off + 0x20);
                std::vector<Run> runs;
                if (!DecodeRunlist(rec.data() + off + runOff, attrLen - runOff, runs))
                    DF_THROW(Corrupt, "malformed runlist in record " + std::to_string(e.mftRef));
                uint64_t remaining = e.size;
                uint64_t clusterBytes = ClusterBytes();
                std::vector<uint8_t> buf((size_t(clusterBytes)));
                for (auto& r : runs) {
                    if (remaining == 0) break;
                    for (uint64_t c = 0; c < r.count && remaining > 0; ++c) {
                        uint64_t take = std::min<uint64_t>(clusterBytes, remaining);
                        if (r.lcn == UINT64_MAX) {
                            std::memset(buf.data(), 0, size_t(clusterBytes));
                        } else {
                            try {
                                dev_->Read(partStart_ + (r.lcn + c) * spc_, buf.data(), spc_);
                            } catch (const DfError&) {
                                std::memset(buf.data(), 0, size_t(clusterBytes));
                            }
                        }
                        sink(buf.data(), size_t(take));
                        remaining -= take;
                    }
                }
            }
            return;
        }
        off += attrLen;
    }
    DF_THROW(NotFound, "no $DATA attribute in record " + std::to_string(e.mftRef));
}

void NtfsVolume::FixupBootHiddenSectors(uint32_t newHiddenSectors) {
    Buffer s(kSectorSize);
    dev_->Read(partStart_, s.data(), 1);
    Wr32(s.data() + 28, newHiddenSectors);
    dev_->Write(partStart_, s.data(), 1);
    if (totalSectors_ >= 2) dev_->Write(partStart_ + totalSectors_ - 1, s.data(), 1);
}

}  // namespace df
