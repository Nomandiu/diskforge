#include "gpt.h"

#include <cstring>

#include "crc32.h"
#include "diskio.h"

namespace df {

void GptEntry::Parse(const uint8_t* p) {
    type.ReadLE(p);
    id.ReadLE(p + 16);
    firstLba = Rd64(p + 32);
    lastLba = Rd64(p + 40);
    attrs = Rd64(p + 48);
    std::u16string n;
    n.reserve(36);
    for (int i = 0; i < 36; ++i) {
        char16_t c = char16_t(Rd16(p + 56 + i * 2));
        if (c == 0) break;
        n += c;
    }
    name = std::move(n);
}

void GptEntry::Write(uint8_t* p) const {
    std::memset(p, 0, kGptEntrySize);
    type.WriteLE(p);
    id.WriteLE(p + 16);
    Wr64(p + 32, firstLba);
    Wr64(p + 40, lastLba);
    Wr64(p + 48, attrs);
    for (size_t i = 0; i < name.size() && i < 36; ++i) Wr16(p + 56 + i * 2, uint16_t(name[i]));
}

void GptHeader::Parse(const uint8_t* p) {
    std::memcpy(signature, p, 8);
    revision = Rd32(p + 8);
    headerSize = Rd32(p + 12);
    crc = Rd32(p + 16);
    currentLba = Rd64(p + 24);
    backupLba = Rd64(p + 32);
    firstUsableLba = Rd64(p + 40);
    lastUsableLba = Rd64(p + 48);
    diskGuid.ReadLE(p + 56);
    entriesLba = Rd64(p + 72);
    numEntries = Rd32(p + 80);
    entrySize = Rd32(p + 84);
    entriesCrc = Rd32(p + 88);
}

void GptHeader::Write(uint8_t* p) const {
    std::memset(p, 0, kSectorSize);
    std::memcpy(p, "EFI PART", 8);
    Wr32(p + 8, revision);
    Wr32(p + 12, headerSize);
    // crc at 16 written by caller (must be computed over header with crc=0)
    Wr64(p + 24, currentLba);
    Wr64(p + 32, backupLba);
    Wr64(p + 40, firstUsableLba);
    Wr64(p + 48, lastUsableLba);
    diskGuid.WriteLE(p + 56);
    Wr64(p + 72, entriesLba);
    Wr32(p + 80, numEntries);
    Wr32(p + 84, entrySize);
    Wr32(p + 88, entriesCrc);
    // LBA1 is a GPT header, not an MBR-style boot sector: no 0xAA55 required.
}

bool GptHeader::ValidateSignature() const {
    return std::memcmp(signature, "EFI PART", 8) == 0;
}

static bool ValidateHeaderBlock(const uint8_t* p, const GptHeader& h) {
    if (!h.ValidateSignature()) return false;
    if (h.headerSize < kGptHeaderSize || h.headerSize > kSectorSize) return false;
    // CRC over headerSize bytes with crc field zeroed
    Buffer tmp(p, p + kSectorSize);
    Wr32(tmp.data() + 16, 0);
    if (Crc32::Compute(tmp.data(), h.headerSize) != h.crc) return false;
    return true;
}

static uint32_t ComputeEntriesCrc(const std::vector<GptEntry>& entries) {
    Crc32 c;
    Buffer one(kGptEntrySize);
    for (auto& e : entries) {
        e.Write(one.data());
        c.Update(one.data(), kGptEntrySize);
    }
    return c.Final();
}

static std::vector<GptEntry> ReadEntries(IDiskDevice& dev, const GptHeader& h) {
    std::vector<GptEntry> out;
    out.reserve(h.numEntries);
    uint64_t remaining = uint64_t(h.numEntries) * h.entrySize;
    uint64_t lba = h.entriesLba;
    Buffer sec(kSectorSize);
    std::vector<uint8_t> stream;
    stream.reserve(size_t(remaining > (64u << 20) ? (64u << 20) : remaining));
    uint64_t bytes = 0;
    while (bytes < remaining) {
        dev.Read(lba, sec.data(), 1);
        ++lba;
        size_t take = size_t(std::min<uint64_t>(kSectorSize, remaining - bytes));
        stream.insert(stream.end(), sec.data(), sec.data() + take);
        bytes += take;
    }
    for (uint32_t i = 0; i < h.numEntries; ++i) {
        GptEntry e;
        e.Parse(stream.data() + size_t(i) * h.entrySize);
        out.push_back(e);
    }
    return out;
}

void GptTable::RefreshCrc() {
    uint32_t crc = ComputeEntriesCrc(entries);
    primary.entriesCrc = crc;
    backup.entriesCrc = crc;
    // header CRC
    Buffer sec(kSectorSize);
    primary.Write(sec.data());
    Wr32(sec.data() + 16, 0);
    primary.crc = Crc32::Compute(sec.data(), primary.headerSize);
    backup.Write(sec.data());
    Wr32(sec.data() + 16, 0);
    backup.crc = Crc32::Compute(sec.data(), backup.headerSize);
}

GptTable GptTable::Read(IDiskDevice& dev) {
    GptTable t;
    uint64_t total = dev.SizeSectors();
    Buffer sec(kSectorSize);

    auto readSide = [&](GptHeader& h, uint64_t lba, bool& ok) {
        try {
            dev.Read(lba, sec.data(), 1);
        } catch (const DfError&) {
            ok = false;
            return;
        }
        h.Parse(sec.data());
        if (!ValidateHeaderBlock(sec.data(), h)) { ok = false; return; }
        if (h.numEntries == 0 || h.numEntries > 4096 || h.entrySize < 128 || h.entrySize > 1024) { ok = false; return; }
        try {
            t.entries = ReadEntries(dev, h);
        } catch (const DfError&) {
            ok = false;
            return;
        }
        ok = ComputeEntriesCrc(t.entries) == h.entriesCrc;
    };

    readSide(t.primary, 1, t.primaryValid);
    if (!t.primaryValid) {
        // Try backup to recover entries
        readSide(t.backup, total - 1, t.backupValid);
    } else {
        readSide(t.backup, t.primary.backupLba ? t.primary.backupLba : total - 1, t.backupValid);
        if (!t.backupValid) {
            // primary is fine; keep its parsed entries
            t.backup = t.primary;
        }
    }
    return t;
}

void GptTable::Write(IDiskDevice& dev, uint64_t totalSectors) {
    if (entries.size() != size_t(primary.numEntries)) {
        // adjust headers to entry vector
        primary.numEntries = uint32_t(entries.size());
        backup.numEntries = primary.numEntries;
    }
    primary.currentLba = 1;
    primary.backupLba = totalSectors - 1;
    primary.entriesLba = 2;
    primary.firstUsableLba = kGptFirstUsableLba;
    primary.lastUsableLba = totalSectors - 1 - (uint64_t(primary.numEntries) * primary.entrySize + kSectorSize - 1) / kSectorSize - 1;

    backup = primary;
    backup.currentLba = totalSectors - 1;
    backup.backupLba = 1;
    backup.entriesLba = totalSectors - 1 - (uint64_t(primary.numEntries) * primary.entrySize + kSectorSize - 1) / kSectorSize;

    RefreshCrc();

    // Entry array size in sectors
    uint64_t arraySectors = (uint64_t(primary.numEntries) * primary.entrySize + kSectorSize - 1) / kSectorSize;

    // Build full array image
    std::vector<uint8_t> arr(size_t(arraySectors) * kSectorSize, 0);
    for (size_t i = 0; i < entries.size(); ++i) entries[i].Write(arr.data() + i * primary.entrySize);

    // Write backup entries + header first (crash safety), then primary.
    dev.Write(backup.entriesLba, arr.data(), size_t(arraySectors));
    Buffer hdr(kSectorSize);
    backup.Write(hdr.data());
    Wr32(hdr.data() + 16, backup.crc);
    dev.Write(totalSectors - 1, hdr.data(), 1);

    dev.Write(primary.entriesLba, arr.data(), size_t(arraySectors));
    primary.Write(hdr.data());
    Wr32(hdr.data() + 16, primary.crc);
    dev.Write(1, hdr.data(), 1);
    dev.Flush();
}

GptTable GptTable::CreateEmpty(uint64_t totalSectors) {
    GptTable t;
    t.primary.signature[0] = 0;  // filled by Write
    t.entries.assign(kGptEntriesDefault, GptEntry{});
    t.primary.diskGuid = Guid::Random();
    t.primary.numEntries = kGptEntriesDefault;
    t.primary.entrySize = kGptEntrySize;
    t.primary.currentLba = 1;
    t.primary.backupLba = totalSectors - 1;
    t.primary.entriesLba = 2;
    t.primary.firstUsableLba = kGptFirstUsableLba;
    t.primary.lastUsableLba = totalSectors - 34;
    t.backup = t.primary;
    t.backup.currentLba = totalSectors - 1;
    t.backup.backupLba = 1;
    t.backup.entriesLba = totalSectors - 33;
    t.primaryValid = t.backupValid = true;
    return t;
}

const char* GptTypeName(const Guid& type) {
    if (type == Guid::GptSystemEsp()) return "EFI System Partition";
    if (type == Guid::GptBiosBoot()) return "BIOS Boot Partition";
    if (type == Guid::GptMsr()) return "Microsoft Reserved (MSR)";
    if (type == Guid::GptBasicData()) return "Basic Data Partition";
    if (type == Guid::GptLinuxFs()) return "Linux Filesystem";
    if (type == Guid::GptLinuxLvm()) return "Linux LVM";
    if (type == Guid::GptLinuxSwap()) return "Linux Swap";
    if (type == Guid::GptWindowsRe()) return "Windows Recovery";
    if (type == Guid::GptVmwareVmfs()) return "VMware VMFS";
    if (type == Guid::GptFreebsdBoot()) return "FreeBSD Boot";
    return "Unknown";
}

}  // namespace df
