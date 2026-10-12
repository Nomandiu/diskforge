#include "layout.h"

#include <algorithm>

#include "diskio.h"

namespace df {

PartKind PartInfo::Kind() const {
    if (isLogical) return PartKind::Logical;
    if (mbrType == 0xEE) return PartKind::Unknown;
    switch (mbrType) {
        case 0x0B: case 0x0C: case 0x0E: case 0x06: case 0x04: case 0x01:
        case 0x07: case 0x17: case 0x1B: case 0x1C: case 0x16: case 0x11:
        case 0x83: case 0x8E:
            return PartKind::Basic;
        case 0x82: return PartKind::LinuxSwap;
        case 0x27: return PartKind::WindowsRe;
        default: break;
    }
    if (mbrType == 0x05 || mbrType == 0x0F || mbrType == 0x85) return PartKind::Extended;
    // GPT
    if (gptType == Guid::GptSystemEsp()) return PartKind::Esp;
    if (gptType == Guid::GptMsr()) return PartKind::Msr;
    if (gptType == Guid::GptWindowsRe()) return PartKind::WindowsRe;
    if (gptType == Guid::GptBiosBoot()) return PartKind::BiosBoot;
    if (gptType == Guid::GptLinuxFs() || gptType == Guid::GptLinuxLvm()) return PartKind::Linux;
    if (gptType == Guid::GptLinuxSwap()) return PartKind::LinuxSwap;
    if (!gptType.IsZero()) return PartKind::Basic;
    if (fs == "NTFS" || fs == "FAT32" || fs == "FAT16" || fs == "exFAT") return PartKind::Basic;
    return PartKind::Unknown;
}

const char* PartInfo::KindName() const {
    switch (Kind()) {
        case PartKind::Basic: return "Basic data";
        case PartKind::Esp: return "EFI System";
        case PartKind::Msr: return "Microsoft Reserved";
        case PartKind::Linux: return "Linux";
        case PartKind::LinuxSwap: return "Linux Swap";
        case PartKind::WindowsRe: return "Windows Recovery";
        case PartKind::Extended: return "Extended";
        case PartKind::Logical: return "Logical";
        case PartKind::BiosBoot: return "BIOS Boot";
        default: return "Unknown";
    }
}

std::pair<Lba, uint64_t> AlignRange(Lba start, uint64_t sectors, uint64_t align) {
    Lba s = RoundUp(start, align);
    uint64_t e = RoundDown(start + sectors, align);
    if (e <= s) return {s, 0};
    return {s, e - s};
}

std::vector<FreeExtent> DiskLayout::FreeExtents(uint64_t alignSectors) const {
    std::vector<FreeExtent> out;
    if (totalSectors == 0) return out;

    // Determine usable bounds.
    Lba first, lastEnd;
    if (style == DiskStyle::Gpt) {
        // default GPT: [34, total-34)
        first = 34;
        lastEnd = totalSectors >= 34 ? totalSectors - 34 : 0;
    } else {
        first = 1;
        lastEnd = totalSectors;
    }

    // Collect occupied ranges (extended container counts as its own range; logicals inside
    // are considered occupied via the container span).
    struct R { Lba a, b; };
    std::vector<R> used;
    for (auto& p : parts) {
        R r{p.start, p.start + p.sectors};
        if (r.b <= r.a) continue;
        // GPT spans the whole entry; MBR logical partitions are inside extended container:
        // the container entry is in `parts` as well, so union covers it.
        used.push_back(r);
    }
    std::sort(used.begin(), used.end(), [](const R& x, const R& y) { return x.a < y.a; });

    Lba cur = first;
    for (auto& r : used) {
        if (r.a > cur) {
            auto [s, n] = AlignRange(cur, r.a - cur, alignSectors);
            if (n > 0) out.push_back(FreeExtent{s, n});
        }
        cur = std::max<Lba>(cur, r.b);
    }
    if (lastEnd > cur) {
        auto [s, n] = AlignRange(cur, lastEnd - cur, alignSectors);
        if (n > 0) out.push_back(FreeExtent{s, n});
    }
    return out;
}

std::optional<FreeExtent> DiskLayout::FindSlot(uint64_t wantSectors, Lba afterLba,
                                               uint64_t alignSectors) const {
    for (auto& e : FreeExtents(alignSectors)) {
        if (e.start < afterLba) {
            Lba s = RoundUp(std::max(e.start, afterLba), alignSectors);
            if (s + wantSectors <= e.End()) return FreeExtent{s, wantSectors};
            continue;
        }
        if (e.sectors >= wantSectors) return FreeExtent{e.start, wantSectors};
    }
    return std::nullopt;
}

bool DiskLayout::OverlapsAny(Lba start, uint64_t sectors) const {
    for (auto& p : parts)
        if (start < p.start + p.sectors && p.start < start + sectors) return true;
    return false;
}

const PartInfo* DiskLayout::FindByStart(Lba start) const {
    for (auto& p : parts)
        if (p.start == start) return &p;
    return nullptr;
}

DiskLayout DiskLayout::ReadFromTables(IDiskDevice& dev) {
    DiskLayout L;
    L.totalSectors = dev.SizeSectors();
    Buffer sec(kSectorSize);
    dev.Read(0, sec.data(), 1);
    MbrTable mbr = MbrTable::Parse(sec.data());

    // GPT?
    GptTable gpt = GptTable::Read(dev);
    if (mbr.IsProtective() || gpt.primaryValid || gpt.backupValid) {
        L.style = DiskStyle::Gpt;
        // Use whichever side parsed + validated.
        GptHeader h = gpt.primaryValid ? gpt.primary : gpt.backup;
        L.diskGuid = h.diskGuid;
        int num = 0;
        for (auto& e : gpt.entries) {
            if (e.IsUnused()) continue;
            if (e.firstLba >= L.totalSectors || e.Sectors() == 0) continue;
            if (e.firstLba < 34 || e.lastLba >= L.totalSectors) continue;
            PartInfo p;
            p.start = e.firstLba;
            p.sectors = e.Sectors();
            p.number = ++num;
            p.gptType = e.type;
            p.gptId = e.id;
            p.gptAttrs = e.attrs;
            p.gptName = e.name;
            L.parts.push_back(p);
        }
        std::sort(L.parts.begin(), L.parts.end(), [](const PartInfo& a, const PartInfo& b) { return a.start < b.start; });
        return L;
    }

    if (!mbr.signatureOk) {
        L.style = DiskStyle::Blank;
        return L;
    }

    L.style = DiskStyle::Mbr;
    L.mbrSignature = Rd32(sec.data() + 440);
    int num = 0;
    for (auto& e : mbr.entries) {
        if (e.IsUnused()) continue;
        if (e.startLba == 0 || e.sectors == 0) continue;
        PartInfo p;
        p.start = e.startLba;
        p.sectors = e.sectors;
        p.mbrType = e.type;
        p.boot = e.IsBootable();
        if (e.IsExtended()) {
            p.number = 0;  // extended container gets no number
            p.isLogical = false;
            L.extendedLba = e.startLba;
            L.parts.push_back(p);
            continue;
        }
        p.number = ++num;
        L.parts.push_back(p);
    }
    // Logical partitions inside extended
    if (L.extendedLba) {
        for (auto& lp : WalkEbrChain(dev, L.extendedLba)) {
            PartInfo p;
            p.start = lp.entry.startLba;
            p.sectors = lp.entry.sectors;
            p.mbrType = lp.entry.type;
            p.isLogical = true;
            p.ebrLba = lp.ebrLba;
            p.number = ++num;
            L.parts.push_back(p);
        }
    }
    std::sort(L.parts.begin(), L.parts.end(), [](const PartInfo& a, const PartInfo& b) { return a.start < b.start; });
    return L;
}

void DiskLayout::WriteToTables(IDiskDevice& dev) const {
    if (style == DiskStyle::Gpt) {
        GptTable t = GptTable::CreateEmpty(dev.SizeSectors());
        t.primary.diskGuid = diskGuid;
        t.entries.assign(kGptEntriesDefault, GptEntry{});
        size_t i = 0;
        for (auto& p : parts) {
            if (p.sectors == 0) continue;
            GptEntry& e = t.entries[i++];
            e.type = p.gptType;
            e.id = p.gptId.IsZero() ? Guid::Random() : p.gptId;
            e.firstLba = p.start;
            e.lastLba = p.start + p.sectors - 1;
            e.attrs = p.gptAttrs;
            e.name = p.gptName.empty() ? std::u16string() : p.gptName;
        }
        t.Write(dev, dev.SizeSectors());
        // protective MBR
        MbrTable prot = MbrTable::MakeProtective(uint32_t(std::min<uint64_t>(dev.SizeSectors(), 0xFFFFFFFFull)));
        Buffer b(kSectorSize);
        prot.Write(b.data());
        dev.Write(0, b.data(), 1);
        return;
    }

    if (style == DiskStyle::Mbr) {
        MbrTable t = MbrTable::MakeEmpty();
        int primaryIdx = 0;
        std::vector<LogicalPart> links;
        for (auto& p : parts) {
            if (p.sectors == 0) continue;
            if (p.isLogical) {
                LogicalPart lp;
                lp.ebrLba = p.ebrLba;
                lp.entry.type = p.mbrType;
                lp.entry.startLba = uint32_t(p.start);
                lp.entry.sectors = uint32_t(p.sectors);
                links.push_back(lp);
                continue;
            }
            if (primaryIdx >= 4) DF_THROW(NoSpace, "MBR supports at most 4 primary entries");
            t.entries[primaryIdx].status = p.boot ? 0x80 : 0x00;
            t.entries[primaryIdx].type = p.mbrType;
            t.entries[primaryIdx].startLba = uint32_t(p.start);
            t.entries[primaryIdx].sectors = uint32_t(p.sectors);
            ++primaryIdx;
        }
        Buffer b(kSectorSize);
        t.Write(b.data());
        Wr32(b.data() + 440, mbrSignature);
        dev.Write(0, b.data(), 1);
        if (extendedLba && !links.empty()) RewriteEbrChain(dev, extendedLba, links);
        return;
    }

    DF_THROW(InvalidParam, "cannot write tables to a blank disk (initialize it first)");
}

}  // namespace df
