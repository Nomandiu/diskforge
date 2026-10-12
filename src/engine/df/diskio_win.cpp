// Windows \\.\PhysicalDriveN device implementation (compiled only on Windows)
#ifdef _WIN32

#include "diskio.h"

#include <windows.h>
#include <winioctl.h>
#include <ntddstor.h>

#include <vector>

#pragma warning(push)
#pragma warning(disable : 26432)  // win32 handle style rules relaxed here

namespace df {

WinPhysicalDrive::WinPhysicalDrive(int diskNumber, bool writable)
    : diskNumber_(diskNumber), writable_(writable) {
    std::string path = "\\\\.\\PhysicalDrive" + std::to_string(diskNumber);
    HANDLE h = CreateFileA(path.c_str(),
                           writable ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_NO_BUFFERING, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        DF_THROW(Permission, "cannot open " + path + " (try running as Administrator)");
    handle_ = h;

    GET_LENGTH_INFORMATION len{};
    DWORD br = 0;
    if (!DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &len, sizeof(len), &br, nullptr))
        DF_THROW(Io, "IOCTL_DISK_GET_LENGTH_INFO failed on " + path);
    sizeSectors_ = Lba(len.Length.QuadPart) / kSectorSize;

    DISK_GEOMETRY_EX geo{};
    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geo, sizeof(geo), &br, nullptr) && br >= sizeof(geo))
        sectorSize_ = geo.Geometry.BytesPerSector ? geo.Geometry.BytesPerSector : kSectorSize;
}

WinPhysicalDrive::~WinPhysicalDrive() {
    if (handle_) CloseHandle((HANDLE)handle_);
}

void WinPhysicalDrive::Read(Lba lba, void* buf, size_t sectors) {
    if (lba + sectors > sizeSectors_) DF_THROW(Io, "read beyond end of disk");
    // NO_BUFFERING requires sector-aligned addresses; buffers from engine are heap
    // 512-aligned via aligned allocations when needed.
    OVERLAPPED ov{};
    ov.Offset = DWORD(lba * kSectorSize);
    ov.OffsetHigh = DWORD((lba * kSectorSize) >> 32);
    DWORD total = 0;
    while (total < sectors * kSectorSize) {
        DWORD n = 0;
        if (!ReadFile((HANDLE)handle_, (uint8_t*)buf + total, DWORD(sectors * kSectorSize - total), &n, &ov))
            DF_THROW(Io, "disk read failed (physical drive " + std::to_string(diskNumber_) + ")");
        if (n == 0) DF_THROW(Io, "disk read returned 0 bytes");
        total += n;
        ov.Offset += n;
        if (ov.Offset < n) ++ov.OffsetHigh;
    }
}

void WinPhysicalDrive::Write(Lba lba, const void* buf, size_t sectors) {
    if (!writable_) DF_THROW(Permission, "device opened read-only");
    if (lba + sectors > sizeSectors_) DF_THROW(Io, "write beyond end of disk");
    OVERLAPPED ov{};
    ov.Offset = DWORD(lba * kSectorSize);
    ov.OffsetHigh = DWORD((lba * kSectorSize) >> 32);
    DWORD total = 0;
    while (total < sectors * kSectorSize) {
        DWORD n = 0;
        if (!WriteFile((HANDLE)handle_, (const uint8_t*)buf + total, DWORD(sectors * kSectorSize - total), &n, &ov))
            DF_THROW(Io, "disk write failed (physical drive " + std::to_string(diskNumber_) + ")");
        if (n == 0) DF_THROW(Io, "disk write returned 0 bytes");
        total += n;
        ov.Offset += n;
        if (ov.Offset < n) ++ov.OffsetHigh;
    }
}

void WinPhysicalDrive::Flush() { FlushFileBuffers((HANDLE)handle_); }

Lba WinPhysicalDrive::SizeSectors() const { return sizeSectors_; }
uint32_t WinPhysicalDrive::SectorSize() const { return sectorSize_; }
std::string WinPhysicalDrive::Name() const { return "\\\\.\\PhysicalDrive" + std::to_string(diskNumber_); }
bool WinPhysicalDrive::IsReadOnly() const { return !writable_; }

void WinPhysicalDrive::UpdateProperties() {
    DWORD br = 0;
    if (!DeviceIoControl((HANDLE)handle_, IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0, &br, nullptr))
        DF_THROW(Io, "IOCTL_DISK_UPDATE_PROPERTIES failed");
}

void WinPhysicalDrive::DismountVolumes() {
    // Volumes are enumerated and dismounted by the ops layer which owns volume info;
    // this is a convenience best-effort path using volume letters is NOT possible here
    // (letters belong to volume manager). See ops_win.cpp DismountVolumesOnDisk.
    (void)0;
}

}  // namespace df

#pragma warning(pop)

#endif  // _WIN32
