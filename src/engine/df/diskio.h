// Sector-level disk I/O abstraction + high-performance copy engine
#pragma once

#include <memory>
#include <string>

#include "common.h"
#include "units.h"

namespace df {

// Abstract block device addressed in 512-byte sectors.
class IDiskDevice {
public:
    virtual ~IDiskDevice() = default;
    virtual void Read(Lba lba, void* buf, size_t sectors) = 0;          // throws DfError
    virtual void Write(Lba lba, const void* buf, size_t sectors) = 0;   // throws DfError
    virtual Lba SizeSectors() const = 0;
    virtual uint32_t SectorSize() const { return kSectorSize; }
    virtual void Flush() {}
    virtual std::string Name() const = 0;
    virtual bool IsReadOnly() const { return false; }
};

// Flat file backed device (sparse friendly). Used by tests and images.
class FileImageDevice : public IDiskDevice {
public:
    FileImageDevice(const std::string& path, bool writable);
    ~FileImageDevice() override;
    void Read(Lba lba, void* buf, size_t sectors) override;
    void Write(Lba lba, const void* buf, size_t sectors) override;
    Lba SizeSectors() const override { return sizeSectors_; }
    void Flush() override;
    std::string Name() const override { return path_; }
    bool IsReadOnly() const override { return !writable_; }

private:
    void* file_ = nullptr;      // FILE* on POSIX / HANDLE on Win32 (impl detail in cpp)
    std::string path_;
    bool writable_ = false;
    Lba sizeSectors_ = 0;
};

#ifdef _WIN32
// \\.\PhysicalDriveN device with unbuffered IO + control operations.
class WinPhysicalDrive : public IDiskDevice {
public:
    explicit WinPhysicalDrive(int diskNumber, bool writable = true);
    ~WinPhysicalDrive() override;
    void Read(Lba lba, void* buf, size_t sectors) override;
    void Write(Lba lba, const void* buf, size_t sectors) override;
    Lba SizeSectors() const override;
    uint32_t SectorSize() const override;
    void Flush() override;
    std::string Name() const override;
    bool IsReadOnly() const override;

    void* Handle() const { return handle_; }
    void UpdateProperties();          // IOCTL_DISK_UPDATE_PROPERTIES
    void DismountVolumes();           // lock+dismount all volumes on this disk

private:
    void* handle_ = nullptr;  // HANDLE
    int diskNumber_ = 0;
    Lba sizeSectors_ = 0;
    uint32_t sectorSize_ = kSectorSize;
    bool writable_ = true;
};
#endif

// Copy `sectors` from src to dst handling overlap (direction chosen automatically).
// Reports progress (stage, fraction) and honors cancellation. Chunked at `chunkSectors`.
void CopySectors(IDiskDevice& dev, Lba src, Lba dst, uint64_t sectors,
                 const Progress& prog, uint64_t chunkSectors = 8192 /* 4 MiB */);

// Fill region with zero sectors.
void ZeroSectors(IDiskDevice& dev, Lba start, uint64_t sectors,
                 const Progress& prog, uint64_t chunkSectors = 8192);

}  // namespace df
