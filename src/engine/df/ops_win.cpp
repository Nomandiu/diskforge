// Windows ops core: device handles, drive layout ioctls, volumes, letters, format
#ifdef _WIN32

#include <windows.h>
#include <winioctl.h>
#include <ntddstor.h>
#include <objbase.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>

#include "fsprobe.h"
#include "gpt.h"
#include "ops.h"

namespace fs = std::filesystem;

namespace df::ops {

// Guid (text-order bytes) <-> Win32 GUID struct
static GUID GuidToWin(const Guid& g) {
    GUID w{};
    w.Data1 = (DWORD(g.b[0]) << 24) | (DWORD(g.b[1]) << 16) | (DWORD(g.b[2]) << 8) | g.b[3];
    w.Data2 = WORD((g.b[4] << 8) | g.b[5]);
    w.Data3 = WORD((g.b[6] << 8) | g.b[7]);
    for (int i = 0; i < 8; ++i) w.Data4[i] = g.b[8 + i];
    return w;
}

// Run a command line fully hidden; returns exit code (-1 on spawn failure)
static int RunHidden(const std::string& cmd) {
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::string cmdline = cmd;
    if (!CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi))
        return -1;
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, 10 * 60 * 1000);
    DWORD code = static_cast<DWORD>(-1);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return int(code);
}

// ---------------------------------------------------------------------------
// win32 helpers
// ---------------------------------------------------------------------------
std::wstring Utf8ToWide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(n ? n - 1 : 0), L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
std::string WideToUtf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n ? n - 1 : 0), '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

static std::string LastErrorText(const char* what) {
    DWORD e = GetLastError();
    char buf[256] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0, buf, sizeof(buf), nullptr);
    std::string out = what;
    out += " failed (";
    out += std::to_string(e);
    out += "): ";
    out += buf;
    return out;
}

struct WinHandle {
    HANDLE h = nullptr;
    ~WinHandle() {
        if (h) CloseHandle(h);
    }
    HANDLE get() const { return h; }
};

static HANDLE OpenDiskHandle(int disk, bool writable) {
    std::string path = "\\\\.\\PhysicalDrive" + std::to_string(disk);
    HANDLE h = CreateFileA(path.c_str(), writable ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) DF_THROW(Permission, LastErrorText("open physical drive"));
    return h;
}

static HANDLE OpenVolumeHandle(const std::wstring& volumeGuid, bool writable) {
    HANDLE h = CreateFileW(volumeGuid.c_str(), writable ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        // retry with 0 access (some volumes only allow queries)
        h = CreateFileW(volumeGuid.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) DF_THROW(Permission, LastErrorText("open volume"));
    }
    return h;
}

static HANDLE OpenVolumeByLetter(char letter, bool writable) {
    std::wstring path = std::wstring(1, wchar_t(letter)) + L":";
    HANDLE h = CreateFileW(path.c_str(), writable ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) DF_THROW(Permission, LastErrorText("open volume by letter"));
    return h;
}

// ---------------------------------------------------------------------------
// drive layout ioctls
// ---------------------------------------------------------------------------
struct IoPart {
    PARTITION_INFORMATION_EX info{};
    bool changed = false;
};

static std::vector<IoPart> GetLayoutEntries(HANDLE disk) {
    DWORD need = sizeof(DRIVE_LAYOUT_INFORMATION_EX) + 127 * sizeof(PARTITION_INFORMATION_EX);
    for (int attempt = 0; attempt < 4; ++attempt) {
        std::vector<uint8_t> buf(need);
        DWORD br = 0;
        if (!DeviceIoControl(disk, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0, buf.data(), DWORD(buf.size()), &br, nullptr)) {
            if (GetLastError() == ERROR_MORE_DATA || GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                need *= 2;
                continue;
            }
            DF_THROW(Io, LastErrorText("IOCTL_DISK_GET_DRIVE_LAYOUT_EX"));
        }
        auto* lay = reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX*>(buf.data());
        std::vector<IoPart> out;
        for (DWORD i = 0; i < lay->PartitionCount; ++i) {
            if (lay->PartitionEntry[i].PartitionStyle == PARTITION_STYLE_MBR &&
                lay->PartitionEntry[i].Mbr.PartitionType == PARTITION_ENTRY_UNUSED)
                continue;
            if (lay->PartitionEntry[i].PartitionLength.QuadPart == 0) continue;
            IoPart p;
            p.info = lay->PartitionEntry[i];
            out.push_back(p);
        }
        return out;
    }
    DF_THROW(Io, "drive layout too large");
}

static void SetLayoutEntries(HANDLE disk, DWORD style, std::vector<IoPart>& parts) {
    if (style == PARTITION_STYLE_MBR) {
        // SET_DRIVE_LAYOUT writes the primary table only; logical partitions are
        // managed through EBR chains (engine).
        parts.erase(std::remove_if(parts.begin(), parts.end(), [](const IoPart& p) {
                        return p.info.PartitionStyle == PARTITION_STYLE_MBR &&
                               p.info.PartitionNumber > 4 &&
                               p.info.Mbr.PartitionType != 0x05 &&
                               p.info.Mbr.PartitionType != 0x0F &&
                               p.info.Mbr.PartitionType != 0x85;
                    }),
                    parts.end());
    }
    size_t bytes = sizeof(DRIVE_LAYOUT_INFORMATION_EX) + (parts.size() ? parts.size() - 1 : 1) * sizeof(PARTITION_INFORMATION_EX);
    std::vector<uint8_t> buf(bytes, 0);
    auto* lay = reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX*>(buf.data());
    lay->PartitionStyle = style;
    lay->PartitionCount = DWORD(parts.size());
    for (size_t i = 0; i < parts.size(); ++i) {
        lay->PartitionEntry[i] = parts[i].info;
        lay->PartitionEntry[i].RewritePartition = parts[i].changed ? TRUE : FALSE;
    }
    DWORD br = 0;
    if (!DeviceIoControl(disk, IOCTL_DISK_SET_DRIVE_LAYOUT_EX, buf.data(), DWORD(bytes), nullptr, 0, &br, nullptr))
        DF_THROW(Io, LastErrorText("IOCTL_DISK_SET_DRIVE_LAYOUT_EX"));
}

static void UpdateProps(HANDLE disk) {
    DWORD br = 0;
    DeviceIoControl(disk, IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0, &br, nullptr);
}

static IoPart* FindByOffset(std::vector<IoPart>& parts, uint64_t partStartLba) {
    for (auto& p : parts)
        if (uint64_t(p.info.StartingOffset.QuadPart) / kSectorSize == partStartLba) return &p;
    return nullptr;
}

// ---------------------------------------------------------------------------
// volumes
// ---------------------------------------------------------------------------
static bool VolumeIsSystem(char letter) {
    std::wstring win = std::wstring(1, wchar_t(letter)) + L":\\Windows";
    return GetFileAttributesW(win.c_str()) != INVALID_FILE_ATTRIBUTES;
}
static bool VolumeIsEsp(char letter) {
    std::wstring efi = std::wstring(1, wchar_t(letter)) + L":\\EFI";
    return GetFileAttributesW(efi.c_str()) != INVALID_FILE_ATTRIBUTES;
}
static bool VolumeHasPagefile(char letter) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management",
                      0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return false;
    wchar_t buf[2048] = {};
    DWORD size = sizeof(buf);
    bool found = false;
    if (RegQueryValueExW(k, L"PagingFiles", nullptr, nullptr, (LPBYTE)buf, &size) == ERROR_SUCCESS) {
        for (const wchar_t* p = buf; *p; p += wcslen(p) + 1) {
            if ((p[0] == L'?' || towupper(p[0]) == towupper(wchar_t(letter))) && p[1] == L':')
                found = true;
        }
    }
    RegCloseKey(k);
    return found;
}

std::vector<VolumeInfo> EnumerateVolumes() {
    std::vector<VolumeInfo> out;
    wchar_t volName[MAX_PATH] = {};
    WinHandle finder{FindFirstVolumeW(volName, MAX_PATH)};
    if (finder.get() == INVALID_HANDLE_VALUE) return out;
    do {
        VolumeInfo v;
        v.volumeGuid = volName;
        // letters
        wchar_t letters[512] = {};
        DWORD ret = 0;
        if (GetVolumePathNamesForVolumeNameW(volName, letters, 512, &ret) && ret > 0) {
            for (const wchar_t* p = letters; *p; p += wcslen(p) + 1) {
                if (wcslen(p) >= 2 && p[1] == L':') {
                    v.letter = char(p[0]);
                    break;
                }
            }
        }
        // extents
        WinHandle vh{OpenVolumeHandle(volName, false)};
        VOLUME_DISK_EXTENTS ext{};
        DWORD br = 0;
        if (DeviceIoControl(vh.get(), IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, &ext, sizeof(ext), &br, nullptr) &&
            ext.NumberOfDiskExtents >= 1) {
            v.disk = int(ext.Extents[0].DiskNumber);
            v.startLba = uint64_t(ext.Extents[0].StartingOffset.QuadPart) / kSectorSize;
            v.sectors = uint64_t(ext.Extents[0].ExtentLength.QuadPart) / kSectorSize;
        } else {
            continue;  // not a disk volume (CD/network/etc)
        }
        if (v.letter) {
            std::wstring root = std::wstring(1, wchar_t(v.letter)) + L":\\";
            wchar_t label[64] = {}, fsName[32] = {};
            if (GetVolumeInformationW(root.c_str(), label, 64, nullptr, nullptr, nullptr, fsName, 32)) {
                v.label = WideToUtf8(label);
                v.fs = WideToUtf8(fsName);
            }
            ULARGE_INTEGER freeB{}, totalB{};
            if (GetDiskFreeSpaceExW(root.c_str(), &freeB, &totalB, nullptr)) {
                v.freeBytes = freeB.QuadPart;
                v.totalBytes = totalB.QuadPart;
            }
            v.isSystem = VolumeIsSystem(v.letter);
            v.isEsp = VolumeIsEsp(v.letter);
            v.isPageFile = VolumeHasPagefile(v.letter);
        }
        out.push_back(v);
    } while (FindNextVolumeW(finder.get(), volName, MAX_PATH));
    return out;
}

static char FindFreeLetter() {
    DWORD mask = GetLogicalDrives();
    for (char c = 'D'; c <= 'Z'; ++c) {
        if (!(mask & (1u << (c - 'A')))) return c;
    }
    for (char c = 'C'; c >= 'A'; --c) {
        if (!(mask & (1u << (c - 'A')))) return c;
    }
    return 0;
}

static void AssignLetter(const std::wstring& volumeGuid, char letter) {
    std::wstring mount = volumeGuid;
    if (mount.back() != L'\\') mount += L'\\';
    std::wstring drive = std::wstring(1, wchar_t(letter)) + L":\\";
    if (!SetVolumeMountPointW(drive.c_str(), mount.c_str()))
        DF_THROW(Io, LastErrorText("SetVolumeMountPoint"));
}

static void RemoveLetter(char letter) {
    std::wstring drive = std::wstring(1, wchar_t(letter)) + L":\\";
    DeleteVolumeMountPointW(drive.c_str());
}

static void DismountVolume(const std::wstring& volumeGuid) {
    WinHandle h{OpenVolumeHandle(volumeGuid, true)};
    if (!h.get()) return;
    DWORD br = 0;
    for (int i = 0; i < 20; ++i) {
        if (DeviceIoControl(h.get(), FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &br, nullptr)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    DeviceIoControl(h.get(), FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &br, nullptr);
    // handle held open keeps the volume dismounted; released by RAII on return
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
}

// wait for a volume to appear for (disk, startLba); returns its info
static VolumeInfo WaitForVolume(int disk, uint64_t startLba, int timeoutSec) {
    for (int t = 0; t < timeoutSec * 4; ++t) {
        for (auto& v : EnumerateVolumes())
            if (v.disk == disk && v.startLba == startLba) return v;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    DF_THROW(NotFound, "volume did not appear for new partition (disk " + std::to_string(disk) + ", LBA " +
                           std::to_string(startLba) + ")");
}

// ---------------------------------------------------------------------------
// disk enumeration
// ---------------------------------------------------------------------------
static const char* BusTypeName(STORAGE_BUS_TYPE t) {
    switch (t) {
        case BusTypeUsb: return "USB";
        case BusTypeSata: return "SATA";
        case BusTypeNvme: return "NVMe";
        case BusTypeAta: return "ATA";
        case BusTypeScsi: return "SCSI";
        case BusTypeRaid: return "RAID";
        case BusTypeSas: return "SAS";
        case BusTypeVirtual: return "Virtual";
        case BusTypeFileBackedVirtual: return "Virtual";
        case BusTypeMmc: return "MMC";
        case BusTypeSd: return "SD";
        default: return "Other";
    }
}

std::vector<std::string> SupportedFilesystems() { return {"NTFS", "FAT32", "exFAT"}; }

DiskInfo GetDisk(int diskNumber) {
    DiskInfo di;
    di.number = diskNumber;
    di.path = "\\\\.\\PhysicalDrive" + std::to_string(diskNumber);
    WinHandle disk{OpenDiskHandle(diskNumber, false)};
    di.readOnly = false;

    // size + geometry
    GET_LENGTH_INFORMATION len{};
    DWORD br = 0;
    if (!DeviceIoControl(disk.get(), IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &len, sizeof(len), &br, nullptr))
        DF_THROW(Io, LastErrorText("IOCTL_DISK_GET_LENGTH_INFO"));
    di.sizeBytes = uint64_t(len.Length.QuadPart);

    DISK_GEOMETRY_EX geo{};
    if (DeviceIoControl(disk.get(), IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geo, sizeof(geo), &br, nullptr) &&
        br >= sizeof(geo)) {
        di.sectorSize = geo.Geometry.BytesPerSector;
    } else {
        di.sectorSize = kSectorSize;
    }

    // storage descriptor
    {
        BYTE descBuf[1024] = {};
        STORAGE_PROPERTY_QUERY q{};
        q.PropertyId = StorageDeviceProperty;
        q.QueryType = PropertyStandardQuery;
        if (DeviceIoControl(disk.get(), IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), descBuf, sizeof(descBuf), &br, nullptr)) {
            auto* d = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(descBuf);
            auto strAt = [&](ULONG off) -> std::string {
                if (off == 0 || off >= sizeof(descBuf)) return {};
                return std::string((const char*)descBuf + off);
            };
            di.model = strAt(d->ProductIdOffset);
            std::string vendor = strAt(d->VendorIdOffset);
            if (!vendor.empty()) di.model = vendor + " " + di.model;
            di.serial = strAt(d->SerialNumberOffset);
            di.busType = BusTypeName(d->BusType);
            di.isRemovable = d->RemovableMedia != FALSE;
        }
    }
    // physical sector size
    {
        STORAGE_PROPERTY_QUERY q{};
        q.PropertyId = StorageAccessAlignmentProperty;
        q.QueryType = PropertyStandardQuery;
        BYTE buf[256] = {};
        if (DeviceIoControl(disk.get(), IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &br, nullptr) &&
            br >= sizeof(STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR)) {
            auto* a = reinterpret_cast<STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR*>(buf);
            if (a->BytesPerPhysicalSector) di.physicalSectorSize = a->BytesPerPhysicalSector;
        }
    }
    // SSD via TRIM support
    {
        STORAGE_PROPERTY_QUERY q{};
        q.PropertyId = StorageDeviceTrimProperty;
        q.QueryType = PropertyStandardQuery;
        DEVICE_TRIM_DESCRIPTOR trim{};
        if (DeviceIoControl(disk.get(), IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), &trim, sizeof(trim), &br, nullptr))
            di.isSsd = trim.TrimEnabled != FALSE;
    }
    DWORD wr = 0;
    di.readOnly = !DeviceIoControl(disk.get(), IOCTL_DISK_IS_WRITABLE, nullptr, 0, nullptr, 0, &wr, nullptr);

    // layout from tables + enrichment
    {
        WinPhysicalDrive dev(diskNumber, false);
        DiskLayout L = DiskLayout::ReadFromTables(dev);
        di.layout = L;
        di.layout.totalSectors = di.sizeBytes / kSectorSize;
    }
    // partition numbers from ioctl view
    {
        auto entries = GetLayoutEntries(disk.get());
        int num = 0;
        for (auto& e : entries) {
            if (e.info.PartitionStyle == PARTITION_STYLE_MBR &&
                (e.info.Mbr.PartitionType == 0x05 || e.info.Mbr.PartitionType == 0x0F || e.info.Mbr.PartitionType == 0x85))
                continue;
            uint64_t start = uint64_t(e.info.StartingOffset.QuadPart) / kSectorSize;
            for (auto& p : di.layout.parts)
                if (p.start == start) p.number = int(e.info.PartitionNumber);
        }
    }
    // fs probing + volumes
    di.volumes = EnumerateVolumes();
    {
        WinPhysicalDrive dev(diskNumber, false);
        for (auto& p : di.layout.parts) {
            if (p.sectors == 0) continue;
            FsProbeResult pr = ProbeFs(dev, p.start, p.sectors);
            p.fs = pr.fs;
            for (auto& v : di.volumes) {
                if (v.disk == diskNumber && v.startLba == p.start) {
                    p.letter = v.letter;
                    if (!v.label.empty()) p.label = v.label;
                    p.usedBytes = v.totalBytes > v.freeBytes ? v.totalBytes - v.freeBytes : 0;
                    p.freeBytes = v.freeBytes;
                    if (v.isSystem || v.isEsp) p.boot = p.boot || false;
                }
            }
            if (p.fs == "NTFS") {
                try {
                    NtfsVolume nv = NtfsVolume::Open(&dev, p.start, p.sectors);
                    if (!nv.Label().empty()) p.label = nv.Label();
                } catch (const DfError&) {
                }
            } else if (p.fs == "FAT32" || p.fs == "FAT16" || p.fs == "FAT12") {
                try {
                    FatVolume fv = FatVolume::Open(&dev, p.start, p.sectors);
                    if (!fv.Label().empty()) p.label = fv.Label();
                } catch (const DfError&) {
                }
            }
        }
    }
    return di;
}

std::vector<DiskInfo> EnumerateDisks() {
    std::vector<DiskInfo> out;
    for (int n = 0; n < 64; ++n) {
        std::string path = "\\\\.\\PhysicalDrive" + std::to_string(n);
        HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        CloseHandle(h);
        try {
            out.push_back(GetDisk(n));
        } catch (const DfError&) {
            // skip unreadable disks
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// basic operations
// ---------------------------------------------------------------------------
void InitializeDisk(int disk, DiskStyle style, const Progress& prog) {
    prog.Report("Initializing disk", 0.1);
    WinHandle h{OpenDiskHandle(disk, true)};
    CREATE_DISK cd{};
    if (style == DiskStyle::Gpt) {
        cd.PartitionStyle = PARTITION_STYLE_GPT;
        cd.Gpt.DiskId = [] {
            GUID g;
            CoCreateGuid(&g);
            return g;
        }();
    } else {
        cd.PartitionStyle = PARTITION_STYLE_MBR;
        cd.Mbr.Signature = GetTickCount();
    }
    DWORD br = 0;
    if (!DeviceIoControl(h.get(), IOCTL_DISK_CREATE_DISK, &cd, sizeof(cd), nullptr, 0, &br, nullptr))
        DF_THROW(Io, LastErrorText("IOCTL_DISK_CREATE_DISK"));
    // write minimal layout so GPT protective structures exist
    std::vector<IoPart> empty;
    SetLayoutEntries(h.get(), style == DiskStyle::Gpt ? PARTITION_STYLE_GPT : PARTITION_STYLE_MBR, empty);
    UpdateProps(h.get());
    prog.Report("Initializing disk", 1.0);
}

static uint8_t MbrTypeForFs(const std::string& fs) {
    if (fs == "FAT32") return 0x0C;
    if (fs == "FAT16") return 0x0E;
    if (fs.empty()) return 0x07;
    return 0x07;  // NTFS / exFAT / IFS
}

PartInfo CreatePartition(int disk, uint64_t startLba, uint64_t sectors, const std::string& fs,
                         const std::string& label, char letter, const Progress& prog) {
    if (sectors == 0) DF_THROW(InvalidParam, "partition size must be positive");
    if (startLba % kAlignSectors || sectors % kAlignSectors)
        DF_THROW(InvalidParam, "partition must be aligned to 1 MiB");

    WinHandle h{OpenDiskHandle(disk, true)};
    auto parts = GetLayoutEntries(h.get());
    DWORD style = parts.empty() ? DWORD(PARTITION_STYLE_GPT) : parts[0].info.PartitionStyle;
    if (parts.empty()) {
        // need style from tables
        WinPhysicalDrive dev(disk, true);
        DiskLayout L = DiskLayout::ReadFromTables(dev);
        style = L.style == DiskStyle::Mbr ? DWORD(PARTITION_STYLE_MBR) : DWORD(PARTITION_STYLE_GPT);
    }

    // overlap check
    for (auto& p : parts) {
        uint64_t s = uint64_t(p.info.StartingOffset.QuadPart) / kSectorSize;
        uint64_t n = uint64_t(p.info.PartitionLength.QuadPart) / kSectorSize;
        if (startLba < s + n && s < startLba + sectors)
            DF_THROW(InvalidParam, "new partition overlaps an existing one");
    }

    IoPart np;
    np.changed = true;
    np.info.PartitionStyle = style;
    np.info.StartingOffset.QuadPart = LONGLONG(startLba * kSectorSize);
    np.info.PartitionLength.QuadPart = LONGLONG(sectors * kSectorSize);
    np.info.PartitionNumber = 0;
    if (style == PARTITION_STYLE_GPT) {
        GUID type = GUID{0xEBD0A0A2, 0xB9E5, 0x4433, {0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7}};  // basic data
        GUID id{};
        CoCreateGuid(&id);
        np.info.Gpt.PartitionType = type;
        np.info.Gpt.PartitionId = id;
        np.info.Gpt.Attributes = 0;
        std::wstring name = Utf8ToWide(label.empty() ? "Basic data partition" : label);
        wcsncpy(np.info.Gpt.Name, name.c_str(), 35);
    } else {
        np.info.Mbr.PartitionType = MbrTypeForFs(fs);
        np.info.Mbr.BootIndicator = FALSE;
        np.info.Mbr.RecognizedPartition = TRUE;
        np.info.Mbr.HiddenSectors = 0;
    }
    parts.push_back(np);
    SetLayoutEntries(h.get(), style, parts);
    UpdateProps(h.get());
    prog.Report("Creating partition", 0.6);

    // wait for volume, then letter + format
    VolumeInfo v;
    try {
        v = WaitForVolume(disk, startLba, 12);
    } catch (const DfError&) {
        // raw partition without FS — no volume expected when fs empty
        PartInfo pi;
        pi.start = startLba;
        pi.sectors = sectors;
        pi.fs = "";
        return pi;
    }
    if (!v.letter && letter == 0) letter = FindFreeLetter();
    if (!v.letter && letter) {
        try {
            AssignLetter(v.volumeGuid, letter);
            v.letter = letter;
        } catch (const DfError&) {
        }
    }
    if (!fs.empty()) {
        FormatPartition(disk, startLba, fs, label, true, prog);
    }
    PartInfo pi;
    pi.start = startLba;
    pi.sectors = sectors;
    pi.letter = v.letter;
    pi.fs = fs;
    pi.label = label;
    return pi;
}

void DeletePartition(int disk, uint64_t partStart, const Progress& prog) {
    prog.Report("Deleting partition", 0.2);
    WinHandle h{OpenDiskHandle(disk, true)};

    // best effort: dismount volumes of this partition
    for (auto& v : EnumerateVolumes())
        if (v.disk == disk && v.startLba == partStart && !v.volumeGuid.empty()) DismountVolume(v.volumeGuid);

    // MBR logical partition? (identified via the EBR chain)
    {
        WinPhysicalDrive dev(disk, true);
        DiskLayout L = DiskLayout::ReadFromTables(dev);
        if (L.style == DiskStyle::Mbr && L.extendedLba) {
            auto links = WalkEbrChain(dev, L.extendedLba);
            for (auto& lp : links)
                if (lp.entry.startLba == partStart) {
                    links.erase(std::remove_if(links.begin(), links.end(), [&](const LogicalPart& l) {
                                    return l.entry.startLba == partStart;
                                }),
                                links.end());
                    RewriteEbrChain(dev, L.extendedLba, links);
                    UpdateProps(h.get());
                    prog.Report("Deleting partition", 1.0);
                    return;
                }
        }
    }

    auto parts = GetLayoutEntries(h.get());
    DWORD style = parts.empty() ? DWORD(PARTITION_STYLE_GPT) : parts[0].info.PartitionStyle;
    size_t before = parts.size();
    parts.erase(std::remove_if(parts.begin(), parts.end(), [&](const IoPart& p) {
                    return uint64_t(p.info.StartingOffset.QuadPart) / kSectorSize == partStart;
                }),
                parts.end());
    if (parts.size() == before)
        DF_THROW(NotFound, "partition not found at LBA " + std::to_string(partStart));
    SetLayoutEntries(h.get(), style, parts);
    UpdateProps(h.get());
    prog.Report("Deleting partition", 1.0);
}

void FormatPartition(int disk, uint64_t partStart, const std::string& fs, const std::string& label,
                     bool quick, const Progress& prog) {
    // find volume for this partition
    VolumeInfo v;
    bool found = false;
    for (int t = 0; t < 8 && !found; ++t) {
        for (auto& vi : EnumerateVolumes())
            if (vi.disk == disk && vi.startLba == partStart) {
                v = vi;
                found = true;
            }
        if (!found) std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (!found) DF_THROW(NotFound, "no volume found at partition LBA " + std::to_string(partStart));
    if (!v.letter) {
        char c = FindFreeLetter();
        if (!c) DF_THROW(NoSpace, "no free drive letter available");
        AssignLetter(v.volumeGuid, c);
        v.letter = c;
    }

    // clear current label to avoid the "enter current volume label" prompt
    SetVolumeLabelW((std::wstring(1, wchar_t(v.letter)) + L":\\").c_str(), L"");

    prog.Report("Formatting " + fs, 0.3);
    std::string labelArg = label.empty() ? "DiskForge" : label;
    for (auto& ch : labelArg)
        if (std::strchr("\"&|<>^", ch)) ch = '_';

    std::string cmd = "cmd.exe /c \"echo Y| format ";
    cmd += v.letter;
    cmd += ": /FS:" + fs;
    if (quick) cmd += " /Q";
    cmd += " /V:\"" + labelArg + "\"";
    cmd += " /X > NUL 2>&1\"";
    int rc = RunHidden(cmd);
    if (rc != 0) {
        // engine fallback for FAT32
        if (fs == "FAT32") {
            prog.Report("Formatting FAT32 (engine)", 0.5);
            DismountVolume(v.volumeGuid);
            WinPhysicalDrive dev(disk, true);
            FatVolume::FormatFat32(dev, partStart, 0, labelArg);
            UpdateProps(OpenDiskHandle(disk, true));
        } else {
            DF_THROW(FsError, "format failed (exit " + std::to_string(rc) + ")");
        }
    }
    prog.Report("Verifying", 0.9);
    // verify
    for (int t = 0; t < 12; ++t) {
        std::wstring root = std::wstring(1, wchar_t(v.letter)) + L":\\";
        wchar_t fsn[32] = {};
        if (GetVolumeInformationW(root.c_str(), nullptr, 0, nullptr, nullptr, nullptr, fsn, 32)) {
            std::string got = WideToUtf8(fsn);
            if (got.find(fs) != std::string::npos || fs.find(got) != std::string::npos) {
                prog.Report("Format complete", 1.0);
                return;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    DF_THROW(FsError, "format verification timed out");
}

void SetPartitionLabel(int disk, uint64_t partStart, const std::string& label) {
    for (auto& v : EnumerateVolumes()) {
        if (v.disk == disk && v.startLba == partStart && v.letter) {
            std::wstring root = std::wstring(1, wchar_t(v.letter)) + L":\\";
            if (!SetVolumeLabelW(root.c_str(), Utf8ToWide(label).c_str()))
                DF_THROW(Io, LastErrorText("SetVolumeLabel"));
            return;
        }
    }
    DF_THROW(NotFound, "no mounted volume at partition LBA " + std::to_string(partStart));
}

void SetDriveLetter(int disk, uint64_t partStart, char newLetter) {
    VolumeInfo target;
    bool found = false;
    for (auto& v : EnumerateVolumes())
        if (v.disk == disk && v.startLba == partStart) {
            target = v;
            found = true;
        }
    if (!found) DF_THROW(NotFound, "no volume at partition LBA " + std::to_string(partStart));

    if (target.letter) {
        if (newLetter == target.letter) return;
        RemoveLetter(target.letter);
    }
    if (newLetter) AssignLetter(target.volumeGuid, newLetter);
}

void SetActivePartition(int disk, uint64_t partStart) {
    WinHandle h{OpenDiskHandle(disk, true)};
    auto parts = GetLayoutEntries(h.get());
    if (parts.empty() || parts[0].info.PartitionStyle != PARTITION_STYLE_MBR)
        DF_THROW(NotSupported, "active partitions are an MBR concept (this disk is GPT)");
    bool any = false;
    for (auto& p : parts) {
        bool isTarget = uint64_t(p.info.StartingOffset.QuadPart) / kSectorSize == partStart;
        if (p.info.PartitionStyle == PARTITION_STYLE_MBR) {
            BOOL want = isTarget ? TRUE : FALSE;
            if (p.info.Mbr.BootIndicator != want) {
                p.info.Mbr.BootIndicator = want;
                p.changed = true;
            }
            if (isTarget) any = true;
        }
    }
    if (!any) DF_THROW(NotFound, "partition not found at LBA " + std::to_string(partStart));
    SetLayoutEntries(h.get(), PARTITION_STYLE_MBR, parts);
    UpdateProps(h.get());
}

static void SetGptHiddenBit(std::vector<IoPart>& parts, uint64_t partStart, bool hidden) {
    constexpr uint64_t kNoDriveLetterBit = 1ull << 63;
    bool any = false;
    for (auto& p : parts) {
        if (uint64_t(p.info.StartingOffset.QuadPart) / kSectorSize != partStart) continue;
        uint64_t attrs = p.info.Gpt.Attributes;
        uint64_t want = hidden ? (attrs | kNoDriveLetterBit) : (attrs & ~kNoDriveLetterBit);
        if (want != attrs) {
            p.info.Gpt.Attributes = want;
            p.changed = true;
        }
        any = true;
    }
    if (!any) DF_THROW(NotFound, "partition not found at LBA " + std::to_string(partStart));
}

void SetHidden(int disk, uint64_t partStart, bool hidden) {
    WinHandle h{OpenDiskHandle(disk, true)};
    auto parts = GetLayoutEntries(h.get());
    DWORD style = parts.empty() ? DWORD(PARTITION_STYLE_GPT) : parts[0].info.PartitionStyle;

    // dismount volumes first
    for (auto& v : EnumerateVolumes())
        if (v.disk == disk && v.startLba == partStart) {
            if (v.letter) RemoveLetter(v.letter);
            DismountVolume(v.volumeGuid);
        }

    if (style == PARTITION_STYLE_GPT) {
        SetGptHiddenBit(parts, partStart, hidden);
    } else {
        bool any = false;
        for (auto& p : parts) {
            if (uint64_t(p.info.StartingOffset.QuadPart) / kSectorSize != partStart) continue;
            uint8_t t = p.info.Mbr.PartitionType;
            uint8_t want = hidden ? mbr::HideType(t) : mbr::UnhideType(t);
            if (want != t) {
                p.info.Mbr.PartitionType = want;
                p.changed = true;
            }
            any = true;
        }
        if (!any) DF_THROW(NotFound, "partition not found at LBA " + std::to_string(partStart));
    }
    SetLayoutEntries(h.get(), style, parts);
    UpdateProps(h.get());
}

void ChangeMbrTypeId(int disk, uint64_t partStart, uint8_t typeId) {
    WinHandle h{OpenDiskHandle(disk, true)};
    auto parts = GetLayoutEntries(h.get());
    if (parts.empty() || parts[0].info.PartitionStyle != PARTITION_STYLE_MBR)
        DF_THROW(NotSupported, "MBR type IDs apply to MBR disks only");
    bool any = false;
    for (auto& p : parts) {
        if (uint64_t(p.info.StartingOffset.QuadPart) / kSectorSize != partStart) continue;
        p.info.Mbr.PartitionType = typeId;
        p.changed = true;
        any = true;
    }
    if (!any) DF_THROW(NotFound, "partition not found at LBA " + std::to_string(partStart));
    SetLayoutEntries(h.get(), PARTITION_STYLE_MBR, parts);
    UpdateProps(h.get());
}

void ChangeGptType(int disk, uint64_t partStart, const Guid& type) {
    WinHandle h{OpenDiskHandle(disk, true)};
    auto parts = GetLayoutEntries(h.get());
    if (parts.empty() || parts[0].info.PartitionStyle != PARTITION_STYLE_GPT)
        DF_THROW(NotSupported, "GPT type GUIDs apply to GPT disks only");
    bool any = false;
    for (auto& p : parts) {
        if (uint64_t(p.info.StartingOffset.QuadPart) / kSectorSize != partStart) continue;
        p.info.Gpt.PartitionType = GuidToWin(type);
        p.changed = true;
        any = true;
    }
    if (!any) DF_THROW(NotFound, "partition not found at LBA " + std::to_string(partStart));
    SetLayoutEntries(h.get(), PARTITION_STYLE_GPT, parts);
    UpdateProps(h.get());
}

// ---------------------------------------------------------------------------
// resize / move / composites
// ---------------------------------------------------------------------------
static VolumeInfo VolumeAt(int disk, uint64_t startLba) {
    for (int t = 0; t < 8; ++t) {
        for (auto& v : EnumerateVolumes())
            if (v.disk == disk && v.startLba == startLba) return v;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    DF_THROW(NotFound, "no volume at partition LBA " + std::to_string(startLba));
}

static std::string FsOfPartition(const DiskInfo& di, uint64_t startLba) {
    for (auto& p : di.layout.parts)
        if (p.start == startLba) return p.fs;
    return "";
}

static void GuardMovable(const DiskInfo& di, const PartInfo& p) {
    for (auto& v : di.volumes) {
        if (v.disk == di.number && v.startLba == p.start && v.isSystem)
            DF_THROW(NotSupported,
                     "moving the Windows system partition is not supported while Windows is running");
    }
}

void ShrinkPartition(int disk, uint64_t partStart, uint64_t newSectors, const Progress& prog) {
    newSectors = RoundDown(newSectors, kAlignSectors);
    if (newSectors == 0) DF_THROW(InvalidParam, "invalid target size");
    DiskInfo di = GetDisk(disk);
    std::string fs = FsOfPartition(di, partStart);
    prog.Report("Shrinking volume", 0.1);

    if (fs == "NTFS") {
        VolumeInfo v = VolumeAt(disk, partStart);
        WinHandle vh{OpenVolumeHandle(v.volumeGuid, true)};
        SHRINK_VOLUME_INFORMATION si{};
        si.RequestType = ShrinkPrepare;
        si.NewNumberOfSectors = LONGLONG(newSectors);
        DWORD br = 0;
        if (!DeviceIoControl(vh.get(), FSCTL_SHRINK_VOLUME, &si, sizeof(si), nullptr, 0, &br, nullptr)) {
            si.RequestType = ShrinkAbort;
            DeviceIoControl(vh.get(), FSCTL_SHRINK_VOLUME, &si, sizeof(si), nullptr, 0, &br, nullptr);
            DF_THROW(FsError, LastErrorText("shrink prepare (unmovable files may block shrinking)"));
        }
        si.RequestType = ShrinkCommit;
        if (!DeviceIoControl(vh.get(), FSCTL_SHRINK_VOLUME, &si, sizeof(si), nullptr, 0, &br, nullptr))
            DF_THROW(FsError, LastErrorText("shrink commit"));
        prog.Report("Updating partition table", 0.7);
        WinHandle dh{OpenDiskHandle(disk, true)};
        auto parts = GetLayoutEntries(dh.get());
        DWORD style = parts[0].info.PartitionStyle;
        bool any = false;
        for (auto& p : parts) {
            if (uint64_t(p.info.StartingOffset.QuadPart) / kSectorSize != partStart) continue;
            p.info.PartitionLength.QuadPart = LONGLONG(newSectors * kSectorSize);
            p.changed = true;
            any = true;
        }
        if (!any) DF_THROW(NotFound, "partition not found");
        SetLayoutEntries(dh.get(), style, parts);
        UpdateProps(dh.get());
    } else if (fs == "FAT32") {
        VolumeInfo v = VolumeAt(disk, partStart);
        DismountVolume(v.volumeGuid);
        WinHandle dh{OpenDiskHandle(disk, true)};
        {
            WinPhysicalDrive dev(disk, true);
            uint64_t oldSectors = 0;
            for (auto& p : di.layout.parts)
                if (p.start == partStart) oldSectors = p.sectors;
            FatVolume fv = FatVolume::Open(&dev, partStart, oldSectors);
            fv.Resize(newSectors, prog);
        }
        auto parts = GetLayoutEntries(dh.get());
        DWORD style = parts[0].info.PartitionStyle;
        for (auto& p : parts) {
            if (uint64_t(p.info.StartingOffset.QuadPart) / kSectorSize != partStart) continue;
            p.info.PartitionLength.QuadPart = LONGLONG(newSectors * kSectorSize);
            p.changed = true;
        }
        SetLayoutEntries(dh.get(), style, parts);
        UpdateProps(dh.get());
    } else if (fs.empty() || fs == "RAW") {
        WinHandle dh{OpenDiskHandle(disk, true)};
        auto parts = GetLayoutEntries(dh.get());
        DWORD style = parts[0].info.PartitionStyle;
        for (auto& p : parts) {
            if (uint64_t(p.info.StartingOffset.QuadPart) / kSectorSize != partStart) continue;
            p.info.PartitionLength.QuadPart = LONGLONG(newSectors * kSectorSize);
            p.changed = true;
        }
        SetLayoutEntries(dh.get(), style, parts);
        UpdateProps(dh.get());
    } else {
        DF_THROW(NotSupported, "shrinking " + fs + " volumes is not supported (NTFS and FAT32 only)");
    }
    prog.Report("Shrink complete", 1.0);
}

void ExtendPartition(int disk, uint64_t partStart, uint64_t newSectors, const Progress& prog) {
    newSectors = RoundDown(newSectors, kAlignSectors);
    DiskInfo di = GetDisk(disk);
    std::string fs = FsOfPartition(di, partStart);
    uint64_t oldSectors = 0;
    for (auto& p : di.layout.parts)
        if (p.start == partStart) oldSectors = p.sectors;
    if (newSectors <= oldSectors) DF_THROW(InvalidParam, "extend requires a larger size");
    // free space validation
    {
        bool ok = false;
        for (auto& e : di.layout.FreeExtents())
            if (e.start == partStart + oldSectors && e.sectors >= newSectors - oldSectors) ok = true;
        if (!ok)
            DF_THROW(NoSpace,
                     "no adjacent unallocated space after this partition (move partitions or use "
                     "Allocate Free Space first)");
    }

    prog.Report("Extending partition", 0.3);
    {
        WinHandle dh{OpenDiskHandle(disk, true)};
        auto parts = GetLayoutEntries(dh.get());
        DWORD style = parts[0].info.PartitionStyle;
        for (auto& p : parts) {
            if (uint64_t(p.info.StartingOffset.QuadPart) / kSectorSize != partStart) continue;
            p.info.PartitionLength.QuadPart = LONGLONG(newSectors * kSectorSize);
            p.changed = true;
        }
        SetLayoutEntries(dh.get(), style, parts);
        UpdateProps(dh.get());
    }

    auto rollback = [&] {
        WinHandle dh{OpenDiskHandle(disk, true)};
        auto parts = GetLayoutEntries(dh.get());
        DWORD style = parts[0].info.PartitionStyle;
        for (auto& p : parts) {
            if (uint64_t(p.info.StartingOffset.QuadPart) / kSectorSize != partStart) continue;
            p.info.PartitionLength.QuadPart = LONGLONG(oldSectors * kSectorSize);
            p.changed = true;
        }
        SetLayoutEntries(dh.get(), style, parts);
        UpdateProps(dh.get());
    };

    if (fs == "NTFS" || fs == "exFAT") {
        prog.Report("Extending file system", 0.6);
        VolumeInfo v = VolumeAt(disk, partStart);
        WinHandle vh{OpenVolumeHandle(v.volumeGuid, true)};
        LONGLONG total = LONGLONG(newSectors);
        DWORD br = 0;
        if (!DeviceIoControl(vh.get(), FSCTL_EXTEND_VOLUME, &total, sizeof(total), nullptr, 0, &br, nullptr)) {
            rollback();
            DF_THROW(FsError, LastErrorText("FSCTL_EXTEND_VOLUME"));
        }
    } else if (fs == "FAT32") {
        prog.Report("Extending FAT32 file system", 0.6);
        VolumeInfo v = VolumeAt(disk, partStart);
        DismountVolume(v.volumeGuid);
        try {
            WinPhysicalDrive dev(disk, true);
            FatVolume fv = FatVolume::Open(&dev, partStart, oldSectors);
            fv.Resize(newSectors, prog);
        } catch (const DfError&) {
            rollback();
            throw;
        }
    } else if (fs.empty() || fs == "RAW") {
        // no FS to update
    }
    prog.Report("Extend complete", 1.0);
}

void MovePartition(int disk, uint64_t partStart, uint64_t newStart, const Progress& prog) {
    if (newStart % kAlignSectors) DF_THROW(InvalidParam, "destination must be 1 MiB aligned");
    DiskInfo di = GetDisk(disk);
    const PartInfo* pp = nullptr;
    for (auto& p : di.layout.parts)
        if (p.start == partStart) pp = &p;
    if (!pp) DF_THROW(NotFound, "partition not found at LBA " + std::to_string(partStart));
    if (newStart + pp->sectors > di.layout.totalSectors)
        DF_THROW(InvalidParam, "destination beyond end of disk");
    for (auto& p : di.layout.parts) {
        if (p.start == partStart) continue;
        if (newStart < p.start + p.sectors && p.start < newStart + pp->sectors)
            DF_THROW(InvalidParam, "destination overlaps another partition");
    }
    if (newStart == partStart) return;

    GuardMovable(di, *pp);

    // remember mount identity
    std::wstring volGuid;
    char hadLetter = 0;
    for (auto& v : di.volumes) {
        if (v.disk == disk && v.startLba == partStart) {
            volGuid = v.volumeGuid;
            hadLetter = v.letter;
            DismountVolume(v.volumeGuid);
        }
    }

    prog.Report("Moving data", 0.1);
    {
        WinPhysicalDrive dev(disk, true);
        CopySectors(dev, partStart, newStart, pp->sectors, prog);
        prog.Report("Fixing boot sector", 0.92);
        PatchBootHiddenSectors(dev, newStart, pp->sectors, pp->fs, uint32_t(newStart));
    }
    prog.Report("Updating partition table", 0.95);
    {
        WinHandle dh{OpenDiskHandle(disk, true)};
        auto parts = GetLayoutEntries(dh.get());
        DWORD style = parts[0].info.PartitionStyle;
        for (auto& p : parts) {
            if (uint64_t(p.info.StartingOffset.QuadPart) / kSectorSize != partStart) continue;
            p.info.StartingOffset.QuadPart = LONGLONG(newStart * kSectorSize);
            p.changed = true;
        }
        SetLayoutEntries(dh.get(), style, parts);
        UpdateProps(dh.get());
    }
    // restore drive letter if the volume lost it
    if (hadLetter) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        bool hasLetter = false;
        for (auto& v : EnumerateVolumes())
            if (v.disk == disk && v.startLba == newStart && v.letter) hasLetter = true;
        if (!hasLetter && !volGuid.empty()) {
            try {
                AssignLetter(volGuid, hadLetter);
            } catch (const DfError&) {
            }
        }
    }
    prog.Report("Move complete", 1.0);
}

void SplitPartition(int disk, uint64_t partStart, uint64_t shrinkToSectors, const std::string& newFs,
                    const std::string& newLabel, const Progress& prog) {
    DiskInfo di = GetDisk(disk);
    const PartInfo* pp = nullptr;
    for (auto& p : di.layout.parts)
        if (p.start == partStart) pp = &p;
    if (!pp) DF_THROW(NotFound, "partition not found");
    shrinkToSectors = RoundDown(shrinkToSectors, kAlignSectors);
    if (shrinkToSectors >= pp->sectors) DF_THROW(InvalidParam, "split size must be smaller than the partition");
    ShrinkPartition(disk, partStart, shrinkToSectors, prog);
    prog.Report("Creating new partition", 0.8);
    CreatePartition(disk, partStart + shrinkToSectors, pp->sectors - shrinkToSectors, newFs, newLabel, 0, prog);
    prog.Report("Split complete", 1.0);
}

// ---- merge: file-level copy, then absorb space ----
static uint64_t CopyTree(const std::wstring& src, const std::wstring& dst, const Progress& prog,
                         std::string* error) {
    // returns bytes copied; sets error on failure
    namespace fsys = std::filesystem;
    std::error_code ec;
    uint64_t bytes = 0;
    // count first
    uint64_t total = 0, files = 0;
    for (auto it = fsys::recursive_directory_iterator(src, fsys::directory_options::skip_permission_denied, ec);
         it != fsys::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        std::error_code ec2;
        if (it->is_regular_file(ec2)) {
            total += uint64_t(it->file_size(ec2));
            ++files;
        }
    }
    uint64_t done = 0;
    auto copyDir = [&](auto&& self, const fsys::path& from, const fsys::path& to) -> bool {
        std::error_code e1;
        fsys::create_directories(to, e1);
        for (auto it = fsys::directory_iterator(from, e1); it != fsys::directory_iterator(); it.increment(e1)) {
            if (e1) {
                *error = WideToUtf8(from.wstring()) + ": " + e1.message();
                return false;
            }
            std::error_code el;
            if (it->is_symlink(el) || it->is_other(el)) continue;  // skip reparse/odd entries
            if (it->is_directory(el)) {
                if (!self(self, it->path(), to / it->path().filename())) return false;
            } else if (it->is_regular_file(el)) {
                fsys::path target = to / it->path().filename();
                // collision-safe name
                if (fsys::exists(target, el)) {
                    for (int n = 1;; ++n) {
                        auto stem = target.stem().wstring();
                        auto ext = target.extension().wstring();
                        std::wstring cand = stem + L" (" + std::to_wstring(n) + L")" + ext;
                        fsys::path t2 = target.parent_path() / cand;
                        if (!fsys::exists(t2, el)) {
                            target = t2;
                            break;
                        }
                    }
                }
                fsys::copy_file(it->path(), target, fsys::copy_options::overwrite_existing, el);
                if (el) {
                    *error = WideToUtf8(it->path().wstring()) + ": " + el.message();
                    return false;
                }
                done += uint64_t(it->file_size(el));
                bytes += uint64_t(it->file_size(el));
                prog.Report("Copying files", total ? double(done) / double(total) : 1.0);
            }
        }
        return true;
    };
    if (!copyDir(copyDir, src, dst)) return bytes;
    (void)files;
    return bytes;
}

void MergePartitions(int disk, uint64_t targetStart, uint64_t sourceStart, const Progress& prog) {
    if (targetStart == sourceStart) DF_THROW(InvalidParam, "cannot merge a partition into itself");
    DiskInfo di = GetDisk(disk);
    const PartInfo *T = nullptr, *S = nullptr;
    for (auto& p : di.layout.parts) {
        if (p.start == targetStart) T = &p;
        if (p.start == sourceStart) S = &p;
    }
    if (!T || !S) DF_THROW(NotFound, "partition not found");
    bool adjacent = (S->start == T->start + T->sectors) || (T->start == S->start + S->sectors);
    if (!adjacent)
        DF_THROW(InvalidParam,
                 "merge requires adjacent partitions; data will be merged but space only absorbed when adjacent");

    VolumeInfo tv, sv;
    bool gotT = false, gotS = false;
    for (auto& v : di.volumes) {
        if (v.disk == disk && v.startLba == targetStart) {
            tv = v;
            gotT = true;
        }
        if (v.disk == disk && v.startLba == sourceStart) {
            sv = v;
            gotS = true;
        }
    }
    if (!gotT || !gotS)
        DF_THROW(NotSupported, "both partitions must be formatted with a recognized file system to merge");
    if (!tv.letter) {
        char c = FindFreeLetter();
        if (!c) DF_THROW(NoSpace, "no free drive letter");
        AssignLetter(tv.volumeGuid, c);
        tv.letter = c;
    }
    if (!sv.letter) {
        char c = FindFreeLetter();
        if (!c) DF_THROW(NoSpace, "no free drive letter");
        AssignLetter(sv.volumeGuid, c);
        sv.letter = c;
    }
    // space check
    uint64_t srcUsed = sv.totalBytes > sv.freeBytes ? sv.totalBytes - sv.freeBytes : 0;
    if (tv.freeBytes < srcUsed + 16 * kMiB)
        DF_THROW(NoSpace, "target partition does not have enough free space for the merged data");

    std::string folderName = "Merged_" + (S->label.empty() ? ("Partition_" + std::to_string(S->start / kAlignSectors)) : S->label);
    for (auto& ch : folderName)
        if (std::strchr("\\/:*?\"<>|", ch)) ch = '_';
    std::wstring srcRoot = std::wstring(1, wchar_t(sv.letter)) + L":\\";
    std::wstring dstRoot = std::wstring(1, wchar_t(tv.letter)) + L":\\" + Utf8ToWide(folderName);

    prog.Report("Merging data", 0.1);
    std::string copyError;
    CopyTree(srcRoot, dstRoot, prog, &copyError);
    if (!copyError.empty())
        DF_THROW(Io, "merge aborted during copy (source untouched): " + copyError);

    prog.Report("Removing source partition", 0.9);
    DeletePartition(disk, sourceStart, prog);
    uint64_t newSectors = T->sectors + S->sectors;
    ExtendPartition(disk, targetStart, newSectors, prog);
    prog.Report("Merge complete", 1.0);
}

// slide a free extent until it sits immediately after the target partition.
// returns the target's (possibly updated) start LBA.
static uint64_t SlideFreeExtentNextToTarget(int disk, uint64_t Fstart, uint64_t Fsec, uint64_t targetStart,
                                            const Progress& prog) {
    int guard = 0;
    while (guard++ < 64) {
        DiskInfo di = GetDisk(disk);
        const PartInfo* T = nullptr;
        for (auto& p : di.layout.parts)
            if (p.start == targetStart) T = &p;
        if (!T) DF_THROW(Internal, "lost track of target partition");
        if (Fstart == T->start + T->sectors) return targetStart;

        if (Fstart > T->start + T->sectors) {
            // partition immediately left of the free extent slides right into it
            const PartInfo* P = nullptr;
            for (auto& p : di.layout.parts)
                if (p.start + p.sectors == Fstart) P = &p;
            if (!P) DF_THROW(Internal, "no partition found left of free extent");
            GuardMovable(di, *P);
            uint64_t PoldStart = P->start;
            MovePartition(disk, P->start, Fstart, prog);
            Fstart = PoldStart;
        } else {
            // free extent is left of target: partition immediately right of it slides left into it
            const PartInfo* P = nullptr;
            for (auto& p : di.layout.parts)
                if (p.start == Fstart + Fsec) P = &p;
            if (!P) DF_THROW(Internal, "free extent reaches the end of the disk");
            GuardMovable(di, *P);
            uint64_t PoldStart = P->start;
            MovePartition(disk, P->start, P->start - Fsec, prog);
            if (PoldStart == targetStart) targetStart = PoldStart - Fsec;
            Fstart = PoldStart + P->sectors - Fsec;
        }
    }
    DF_THROW(Internal, "free space sliding did not converge");
}

void AllocateFreeSpace(int disk, uint64_t donorStart, uint64_t targetStart, uint64_t bytes,
                       const Progress& prog) {
    DiskInfo di = GetDisk(disk);
    const PartInfo *D = nullptr, *T = nullptr;
    for (auto& p : di.layout.parts) {
        if (p.start == donorStart) D = &p;
        if (p.start == targetStart) T = &p;
    }
    if (!D || !T) DF_THROW(NotFound, "partition not found");
    if (D == T) DF_THROW(InvalidParam, "donor and target must differ");
    uint64_t X = RoundUp(bytes, kAlignSectors * kSectorSize) / kSectorSize;
    if (X == 0) DF_THROW(InvalidParam, "amount must be positive");
    if (D->sectors <= X + kAlignSectors) DF_THROW(NoSpace, "donor partition too small");

    prog.Report("Shrinking donor partition", 0.05);
    ShrinkPartition(disk, donorStart, D->sectors - X, prog);

    uint64_t Fstart = D->start + D->sectors - X;
    prog.Report("Moving free space into place", 0.3);
    targetStart = SlideFreeExtentNextToTarget(disk, Fstart, X, targetStart, prog);

    prog.Report("Extending target partition", 0.85);
    DiskInfo di2 = GetDisk(disk);
    for (auto& p : di2.layout.parts)
        if (p.start == targetStart) ExtendPartition(disk, targetStart, p.sectors + X, prog);
    prog.Report("Allocation complete", 1.0);
}

uint64_t OneClickExtend(int disk, uint64_t targetStart, uint64_t maxAddBytes, const Progress& prog) {
    DiskInfo di = GetDisk(disk);
    const PartInfo* T = nullptr;
    for (auto& p : di.layout.parts)
        if (p.start == targetStart) T = &p;
    if (!T) DF_THROW(NotFound, "partition not found");
    uint64_t maxAdd = maxAddBytes ? RoundUp(maxAddBytes, kAlignSectors * kSectorSize) / kSectorSize : UINT64_MAX;

    // 1) adjacent free space?
    for (auto& e : di.layout.FreeExtents()) {
        if (e.start == T->start + T->sectors) {
            uint64_t add = std::min<uint64_t>(e.sectors, maxAdd);
            prog.Report("Extending with adjacent free space", 0.2);
            ExtendPartition(disk, targetStart, T->sectors + add, prog);
            return add * kSectorSize;
        }
    }
    // 2) largest free extent anywhere -> slide it next to target
    uint64_t bestStart = 0, bestSectors = 0;
    for (auto& e : di.layout.FreeExtents()) {
        if (e.sectors > bestSectors) {
            bestSectors = e.sectors;
            bestStart = e.start;
        }
    }
    if (bestSectors == 0)
        DF_THROW(NoSpace, "no unallocated space on this disk — shrink a partition first");

    uint64_t X = std::min<uint64_t>(bestSectors, maxAdd);
    prog.Report("Moving free space into place", 0.3);
    targetStart = SlideFreeExtentNextToTarget(disk, bestStart, X, targetStart, prog);

    DiskInfo di2 = GetDisk(disk);
    for (auto& p : di2.layout.parts)
        if (p.start == targetStart) {
            prog.Report("Extending target partition", 0.85);
            ExtendPartition(disk, targetStart, p.sectors + X, prog);
        }
    return X * kSectorSize;
}

uint64_t PartitionMinSectors(int disk, uint64_t partStart) {
    DiskInfo di = GetDisk(disk);
    std::string fs = FsOfPartition(di, partStart);
    uint64_t sectors = 0;
    for (auto& p : di.layout.parts)
        if (p.start == partStart) sectors = p.sectors;
    if (fs == "FAT32") {
        try {
            WinPhysicalDrive dev(disk, false);
            FatVolume fv = FatVolume::Open(&dev, partStart, sectors);
            return RoundUp(fv.MinSectors(), kAlignSectors);
        } catch (const DfError&) {
            return sectors;
        }
    }
    if (fs == "NTFS") {
        // binary search on non-destructive ShrinkPrepare
        try {
            VolumeInfo v = VolumeAt(disk, partStart);
            WinHandle vh{OpenVolumeHandle(v.volumeGuid, false)};
            auto canShrink = [&](uint64_t n) {
                SHRINK_VOLUME_INFORMATION si{};
                si.RequestType = ShrinkPrepare;
                si.NewNumberOfSectors = LONGLONG(n);
                DWORD br = 0;
                BOOL ok = DeviceIoControl(vh.get(), FSCTL_SHRINK_VOLUME, &si, sizeof(si), nullptr, 0, &br, nullptr);
                si.RequestType = ShrinkAbort;
                DeviceIoControl(vh.get(), FSCTL_SHRINK_VOLUME, &si, sizeof(si), nullptr, 0, &br, nullptr);
                return ok != FALSE;
            };
            uint64_t usedSectors = 0;
            for (auto& p2 : di.volumes)
                if (p2.disk == disk && p2.startLba == partStart && p2.totalBytes)
                    usedSectors = (p2.totalBytes - p2.freeBytes) / kSectorSize;
            uint64_t lo = RoundUp(usedSectors + 4 * kMiB / kSectorSize, kAlignSectors);
            uint64_t hi = sectors;
            if (lo >= hi || canShrink(lo)) return std::min(lo, sectors);
            for (int i = 0; i < 14 && hi - lo > kAlignSectors; ++i) {
                uint64_t mid = RoundUp((lo + hi) / 2, kAlignSectors);
                if (mid >= hi) mid = hi - kAlignSectors;
                if (canShrink(mid))
                    hi = mid;
                else
                    lo = mid;
            }
            return hi;
        } catch (const DfError&) {
            return sectors;
        }
    }
    return sectors;
}

// ---------------------------------------------------------------------------
// lost partitions
// ---------------------------------------------------------------------------
ScanResult ScanLostPartitions(int disk, ScanMode mode, const Progress& prog) {
    WinPhysicalDrive dev(disk, false);
    DiskInfo di = GetDisk(disk);
    return df::ScanLostPartitions(dev, di.layout, mode, prog);
}

void RestoreLostPartition(int disk, const LostPartition& lp, const Progress& prog) {
    if (lp.overlapsExisting)
        DF_THROW(InvalidParam, "restored partition would overlap an existing partition");
    if (lp.start % kAlignSectors)
        DF_THROW(InvalidParam, "restored partition is not aligned");
    prog.Report("Restoring partition entry", 0.5);
    WinHandle dh{OpenDiskHandle(disk, true)};
    auto parts = GetLayoutEntries(dh.get());
    DWORD style = parts.empty() ? DWORD(PARTITION_STYLE_GPT) : parts[0].info.PartitionStyle;
    if (parts.empty()) {
        WinPhysicalDrive dev(disk, true);
        DiskLayout L = DiskLayout::ReadFromTables(dev);
        style = L.style == DiskStyle::Mbr ? DWORD(PARTITION_STYLE_MBR) : DWORD(PARTITION_STYLE_GPT);
    }
    IoPart np;
    np.changed = true;
    np.info.PartitionStyle = style;
    np.info.StartingOffset.QuadPart = LONGLONG(lp.start * kSectorSize);
    np.info.PartitionLength.QuadPart = LONGLONG(lp.sectors * kSectorSize);
    np.info.PartitionNumber = 0;
    if (style == PARTITION_STYLE_GPT) {
        np.info.Gpt.PartitionType = GuidToWin(Guid::GptBasicData());
        GUID id{};
        CoCreateGuid(&id);
        np.info.Gpt.PartitionId = id;
        std::wstring name = Utf8ToWide(lp.label.empty() ? ("Recovered " + lp.fs) : lp.label);
        for (size_t i = 0; i < name.size() && i < 35; ++i) np.info.Gpt.Name[i] = name[i];
        np.info.Gpt.Attributes = 0;
    } else {
        np.info.Mbr.PartitionType = lp.fs == "FAT32" ? 0x0C : (lp.fs == "FAT16" ? 0x0E : 0x07);
        np.info.Mbr.BootIndicator = FALSE;
        np.info.Mbr.RecognizedPartition = TRUE;
        np.info.Mbr.HiddenSectors = 0;
    }
    parts.push_back(np);
    SetLayoutEntries(dh.get(), style, parts);
    UpdateProps(dh.get());
    prog.Report("Restore complete", 1.0);
}

void RepairGpt(int disk, const Progress& prog) {
    WinPhysicalDrive dev(disk, true);
    GptTable t = GptTable::Read(dev);
    if (!t.primaryValid && !t.backupValid) DF_THROW(Corrupt, "both GPT copies are damaged");
    prog.Report("Rebuilding GPT structures", 0.6);
    // normalize header fields from the healthy copy
    if (t.primaryValid) {
        t.backup = t.primary;
    } else {
        GptHeader h = t.backup;      // healthy
        t.primary = h;
        t.backup = h;
        t.primary.diskGuid = h.diskGuid;
        t.primary.numEntries = h.numEntries;
        t.primary.entrySize = h.entrySize;
    }
    t.Write(dev, dev.SizeSectors());
    UpdateProps(OpenDiskHandle(disk, true));
    prog.Report("GPT repair complete", 1.0);
}

// ---------------------------------------------------------------------------
// file recovery
// ---------------------------------------------------------------------------
static std::string CategoryFor(const std::string& name) {
    std::string n = LowerCopy(name);
    auto ends = [&](const char* ext) {
        size_t l = strlen(ext);
        return n.size() >= l && n.compare(n.size() - l, l, ext) == 0;
    };
    if (ends(".jpg") || ends(".jpeg") || ends(".png") || ends(".gif") || ends(".bmp") ||
        ends(".tif") || ends(".tiff") || ends(".webp") || ends(".heic") || ends(".raw"))
        return "Photo";
    if (ends(".mp4") || ends(".mov") || ends(".avi") || ends(".mkv") || ends(".wmv") ||
        ends(".m4v") || ends(".mpg") || ends(".mpeg") || ends(".webm"))
        return "Video";
    if (ends(".mp3") || ends(".wav") || ends(".flac") || ends(".aac") || ends(".ogg") ||
        ends(".m4a") || ends(".wma"))
        return "Audio";
    if (ends(".pdf") || ends(".doc") || ends(".docx") || ends(".xls") || ends(".xlsx") ||
        ends(".ppt") || ends(".pptx") || ends(".txt") || ends(".rtf") || ends(".csv"))
        return "Document";
    if (ends(".zip") || ends(".rar") || ends(".7z") || ends(".tar") || ends(".gz") || ends(".bz2"))
        return "Archive";
    return "Other";
}

static void AddFsHits(std::vector<RecoveryHit>& hits, int kind, const std::string& typeName,
                      const std::function<void(RecoveryHit&)>& fill) {
    (void)typeName;
    RecoveryHit h;
    fill(h);
    h.sourceKind = kind;
    h.category = CategoryFor(h.name);
    hits.push_back(std::move(h));
}

std::unique_ptr<RecoverySession> RecoverySession::Start(int disk, bool wholeDisk, uint64_t partStart,
                                                        bool includeDeleted, bool doCarve,
                                                        const CarveOptions& carveOpt, const Progress& prog) {
    auto s = std::unique_ptr<RecoverySession>(new RecoverySession());
    s->disk_ = disk;
    s->device_ = std::make_unique<WinPhysicalDrive>(disk, false);
    DiskInfo di = GetDisk(disk);

    auto scanPartition = [&](uint64_t start, uint64_t sectors) {
        // NTFS
        try {
            s->ntfs_ = std::make_unique<NtfsVolume>(NtfsVolume::Open(s->device_.get(), start, sectors));
            s->hasNtfs_ = true;
            s->partStart_ = start;
            s->partSectors_ = sectors;
            auto entries = s->ntfs_->Enumerate(includeDeleted, prog);
            for (auto& e : entries) {
                if (!e.hasData || e.isDir) continue;
                AddFsHits(s->hits_, 1, "NTFS", [&](RecoveryHit& h) {
                    h.name = e.name;
                    h.path = e.path;
                    h.size = e.size;
                    h.deleted = e.deleted;
                    h.orphaned = e.orphaned;
                    h.ntfs = e;
                    h.ext = e.name.substr(e.name.find_last_of('.') + 1);
                });
            }
            return;
        } catch (const DfError&) {
        }
        // FAT
        try {
            s->fat_ = std::make_unique<FatVolume>(FatVolume::Open(s->device_.get(), start, sectors));
            s->hasFat_ = true;
            s->partStart_ = start;
            s->partSectors_ = sectors;
            auto entries = s->fat_->Enumerate(includeDeleted, prog);
            for (auto& e : entries) {
                if (e.isDir) continue;
                AddFsHits(s->hits_, 2, "FAT", [&](RecoveryHit& h) {
                    h.name = e.name;
                    h.path = e.path;
                    h.size = e.size;
                    h.deleted = e.deleted;
                    h.orphaned = e.chainOrphaned;
                    h.fat = e;
                    h.ext = e.name.substr(e.name.find_last_of('.') + 1);
                });
            }
        } catch (const DfError&) {
        }
    };

    if (wholeDisk) {
        for (auto& p : di.layout.parts) {
            if (p.sectors == 0) continue;
            scanPartition(p.start, p.sectors);
        }
        if (doCarve) {
            for (auto& e : di.layout.FreeExtents()) {
                auto carved = CarveRegion(*s->device_, e.start, e.sectors, carveOpt, prog);
                for (auto& c : carved) {
                    RecoveryHit h;
                    h.carved = true;
                    h.sourceKind = 3;
                    h.name = "carved_" + std::to_string(c.startLba) + "." + c.ext;
                    h.path = "(carved from unallocated space)";
                    h.size = c.sizeBytes;
                    h.sizeEstimated = !c.sizeExact;
                    h.typeName = c.typeName;
                    h.category = CategoryFor(h.name);
                    h.ext = c.ext;
                    h.carvedFile = c;
                    s->hits_.push_back(std::move(h));
                }
            }
        }
    } else {
        uint64_t sectors = 0;
        for (auto& p : di.layout.parts)
            if (p.start == partStart) sectors = p.sectors;
        if (sectors == 0) DF_THROW(NotFound, "partition not found at LBA " + std::to_string(partStart));
        scanPartition(partStart, sectors);
        if (doCarve) {
            auto carved = CarveRegion(*s->device_, partStart, sectors, carveOpt, prog);
            for (auto& c : carved) {
                RecoveryHit h;
                h.carved = true;
                h.sourceKind = 3;
                h.name = "carved_" + std::to_string(c.startLba) + "." + c.ext;
                h.path = "(carved)";
                h.size = c.sizeBytes;
                h.sizeEstimated = !c.sizeExact;
                h.typeName = c.typeName;
                h.category = CategoryFor(h.name);
                h.ext = c.ext;
                h.carvedFile = c;
                s->hits_.push_back(std::move(h));
            }
        }
    }
    return s;
}

static std::string SanitizeFileName(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (uint8_t(c) < 0x20 || std::strchr("\\/:*?\"<>|", c))
            out += '_';
        else
            out += c;
    }
    return out;
}

RecoverStats RecoverySession::Recover(const std::vector<RecoveryHit>& items, const std::string& destDir,
                                      const Progress& prog) {
    RecoverStats st;
    namespace fsys = std::filesystem;
    size_t idx = 0;
    for (auto& h : items) {
        prog.CheckCancel();
        prog.Report("Recovering files", double(idx) / double(items.size() ? items.size() : 1));
        ++idx;
        try {
            std::string rel = h.path.empty() ? h.name : h.path;
            // strip leading slash, drop "Lost Files" into a subfolder is fine
            while (!rel.empty() && (rel[0] == '/' || rel[0] == '\\')) rel = rel.substr(1);
            rel = SanitizeFileName(rel);
            if (rel.empty()) rel = "recovered_file";
            fsys::path dest = fsys::path(destDir) / rel;
            std::error_code ec;
            fsys::create_directories(dest.parent_path(), ec);
            if (ec) DF_THROW(Io, "cannot create directory: " + ec.message());
            // unique-ify
            if (fsys::exists(dest, ec)) {
                for (int n = 1;; ++n) {
                    fsys::path cand = dest.parent_path() /
                        (dest.stem().string() + " (" + std::to_string(n) + ")" + dest.extension().string());
                    if (!fsys::exists(cand, ec)) {
                        dest = cand;
                        break;
                    }
                }
            }
            std::string destStr = dest.string();
            if (destStr.size() > 240) destStr = "\\\\?\\" + destStr;
            FILE* f = nullptr;
#ifdef _MSC_VER
            fopen_s(&f, destStr.c_str(), "wb");
#else
            f = std::fopen(destStr.c_str(), "wb");
#endif
            if (!f) DF_THROW(Io, "cannot create output file: " + destStr);
            uint64_t written = 0;
            auto sink = [&](const uint8_t* p, size_t n) {
                if (std::fwrite(p, 1, n, f) != n) DF_THROW(Io, "write failed: " + destStr);
                written += n;
            };
            if (h.sourceKind == 1 && ntfs_)
                ntfs_->ReadFileData(h.ntfs, sink);
            else if (h.sourceKind == 2 && fat_)
                fat_->ReadFileData(h.fat, sink);
            else if (h.sourceKind == 3 && device_) {
                uint64_t remaining = h.carvedFile.sizeBytes;
                Buffer buf(64 * kKiB);
                uint64_t off = 0;
                while (remaining) {
                    uint64_t take = std::min<uint64_t>(buf.size(), remaining);
                    uint64_t lba = h.carvedFile.startLba + off / kSectorSize;
                    device_->Read(lba, buf.data(), size_t((take + kSectorSize - 1) / kSectorSize));
                    sink(buf.data(), size_t(take));
                    off += take;
                    remaining -= take;
                }
            } else {
                std::fclose(f);
                DF_THROW(Internal, "recovery source missing");
            }
            std::fclose(f);
            ++st.recovered;
            st.bytes += written;
        } catch (const DfError& e) {
            ++st.failed;
            st.errors.push_back(h.name + ": " + e.what());
        }
    }
    prog.Report("Recovery complete", 1.0);
    return st;
}

}  // namespace df::ops

#endif  // _WIN32
