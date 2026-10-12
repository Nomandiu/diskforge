#include "diskio.h"

#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#endif

namespace df {

// ---------------------------------------------------------------------------
// FileImageDevice
// ---------------------------------------------------------------------------
FileImageDevice::FileImageDevice(const std::string& path, bool writable) : path_(path), writable_(writable) {
#ifdef _WIN32
    HANDLE h = CreateFileA(path.c_str(),
                           writable ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ,
                           FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) DF_THROW(Permission, "cannot open image '" + path + "'");
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); DF_THROW(Io, "cannot get image size"); }
    file_ = h;
    sizeSectors_ = Lba(sz.QuadPart) / kSectorSize;
#else
    FILE* f = std::fopen(path.c_str(), writable ? "r+b" : "rb");
    if (!f) {
        if (writable) f = std::fopen(path.c_str(), "w+b");  // create
    }
    if (!f) DF_THROW(Permission, "cannot open image '" + path + "'");
    std::fseek(f, 0, SEEK_END);
    long long sz = 0;
#ifdef _WIN32
    sz = _ftelli64(f);
#else
    sz = ftello(f);
#endif
    if (sz < 0) sz = 0;
    file_ = f;
    sizeSectors_ = Lba(sz) / kSectorSize;
#endif
}

FileImageDevice::~FileImageDevice() {
    if (!file_) return;
#ifdef _WIN32
    CloseHandle((HANDLE)file_);
#else
    std::fclose((FILE*)file_);
#endif
}

static void SeekFile(void* f, Lba lba) {
#ifdef _WIN32
    LARGE_INTEGER pos{};
    pos.QuadPart = LONGLONG(lba * kSectorSize);
    if (!SetFilePointerEx((HANDLE)f, pos, nullptr, FILE_BEGIN))
        DF_THROW(Io, "seek failed in disk image");
#else
    if (fseeko((FILE*)f, off_t(lba * kSectorSize), SEEK_SET) != 0)
        DF_THROW(Io, "seek failed in disk image");
#endif
}

void FileImageDevice::Read(Lba lba, void* buf, size_t sectors) {
    if (lba + sectors > sizeSectors_) DF_THROW(Io, "read beyond end of image");
    SeekFile(file_, lba);
#ifdef _WIN32
    DWORD total = 0;
    while (total < sectors * kSectorSize) {
        DWORD n = 0;
        if (!ReadFile((HANDLE)file_, (uint8_t*)buf + total, DWORD(sectors * kSectorSize - total), &n, nullptr) || n == 0)
            DF_THROW(Io, "read failed in disk image");
        total += n;
    }
#else
    size_t need = sectors * kSectorSize;
    size_t got = std::fread(buf, 1, need, (FILE*)file_);
    if (got != need) {
        // Sparse file region never written: fread of unwritten sparse bytes returns zeros from OS,
        // so a short read means a genuine problem OR file is shorter than sizeSectors_ (created via seek).
        // Support that case: fill remainder with zeros.
        if (std::feof((FILE*)file_)) {
            std::memset((uint8_t*)buf + got, 0, need - got);
        } else {
            DF_THROW(Io, "read failed in disk image");
        }
    }
#endif
}

void FileImageDevice::Write(Lba lba, const void* buf, size_t sectors) {
    if (!writable_) DF_THROW(Permission, "image opened read-only");
    if (lba + sectors > sizeSectors_) DF_THROW(Io, "write beyond end of image");
    SeekFile(file_, lba);
#ifdef _WIN32
    DWORD total = 0;
    while (total < sectors * kSectorSize) {
        DWORD n = 0;
        if (!WriteFile((HANDLE)file_, (const uint8_t*)buf + total, DWORD(sectors * kSectorSize - total), &n, nullptr) || n == 0)
            DF_THROW(Io, "write failed in disk image");
        total += n;
    }
#else
    size_t need = sectors * kSectorSize;
    if (std::fwrite(buf, 1, need, (FILE*)file_) != need) DF_THROW(Io, "write failed in disk image");
#endif
    Lba end = lba + sectors;
    if (end > sizeSectors_) sizeSectors_ = end;  // defensive; normally pre-sized
}

void FileImageDevice::Flush() {
#ifdef _WIN32
    FlushFileBuffers((HANDLE)file_);
#else
    std::fflush((FILE*)file_);
#endif
}

// ---------------------------------------------------------------------------
// Copy / fill engine
// ---------------------------------------------------------------------------
static void CopyChunks(IDiskDevice& dev, Lba src, Lba dst, uint64_t sectors,
                       const Progress& prog, uint64_t chunkSectors, bool forward) {
    Buffer buf(size_t(chunkSectors) * kSectorSize);
    uint64_t done = 0;
    if (forward) {
        for (Lba off = 0; off < sectors; off += chunkSectors) {
            prog.CheckCancel();
            uint64_t n = std::min<uint64_t>(chunkSectors, sectors - off);
            dev.Read(src + off, buf.data(), size_t(n));
            dev.Write(dst + off, buf.data(), size_t(n));
            done += n;
            prog.Report("Copying data", double(done) / double(sectors ? sectors : 1));
        }
    } else {
        uint64_t off = (sectors / chunkSectors) * chunkSectors;
        for (;; off = off >= chunkSectors ? off - chunkSectors : 0) {
            prog.CheckCancel();
            uint64_t n = std::min<uint64_t>(chunkSectors, sectors - off);
            dev.Read(src + off, buf.data(), size_t(n));
            dev.Write(dst + off, buf.data(), size_t(n));
            done += n;
            prog.Report("Copying data", double(done) / double(sectors ? sectors : 1));
            if (off == 0) break;
        }
    }
}

void CopySectors(IDiskDevice& dev, Lba src, Lba dst, uint64_t sectors, const Progress& prog, uint64_t chunkSectors) {
    if (chunkSectors == 0) chunkSectors = 8192;
    if (sectors == 0) return;
    bool overlap = (src < dst + sectors) && (dst < src + sectors);
    // If ranges overlap and destination is higher, copy backward to avoid
    // overwriting source data before it is read.
    bool backward = overlap && dst > src;
    CopyChunks(dev, src, dst, sectors, prog, chunkSectors, !backward);
}

void ZeroSectors(IDiskDevice& dev, Lba start, uint64_t sectors, const Progress& prog, uint64_t chunkSectors) {
    if (chunkSectors == 0) chunkSectors = 8192;
    Buffer buf(size_t(chunkSectors) * kSectorSize, 0);
    uint64_t done = 0;
    for (Lba off = 0; off < sectors; off += chunkSectors) {
        prog.CheckCancel();
        uint64_t n = std::min<uint64_t>(chunkSectors, sectors - off);
        dev.Write(start + off, buf.data(), size_t(n));
        done += n;
        prog.Report("Zero filling", double(done) / double(sectors ? sectors : 1));
    }
}

}  // namespace df
