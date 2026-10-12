// DiskForge engine — common types, errors, little-endian helpers
#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <memory>

namespace df {

using Lba = uint64_t;          // logical block (sector) number, 512-byte units
using Bytes = uint64_t;

constexpr uint32_t kSectorSize = 512;
constexpr uint64_t kKiB = 1024ull;
constexpr uint64_t kMiB = 1024ull * 1024ull;
constexpr uint64_t kGiB = 1024ull * kMiB;
constexpr uint64_t kAlignSectors = 2048;  // 1 MiB alignment in 512B sectors

enum class ErrorCode {
    None = 0,
    Io,
    Parse,
    InvalidParam,
    NoSpace,
    NotSupported,
    Canceled,
    NotFound,
    Corrupt,
    Permission,
    FsError,
    Busy,
    Internal,
};

inline const char* ErrorCodeName(ErrorCode c) {
    switch (c) {
        case ErrorCode::None: return "OK";
        case ErrorCode::Io: return "I/O error";
        case ErrorCode::Parse: return "parse error";
        case ErrorCode::InvalidParam: return "invalid parameter";
        case ErrorCode::NoSpace: return "no space";
        case ErrorCode::NotSupported: return "not supported";
        case ErrorCode::Canceled: return "canceled";
        case ErrorCode::NotFound: return "not found";
        case ErrorCode::Corrupt: return "corrupt";
        case ErrorCode::Permission: return "permission denied";
        case ErrorCode::FsError: return "file system error";
        case ErrorCode::Busy: return "resource busy";
        case ErrorCode::Internal: return "internal error";
    }
    return "?";
}

struct DfError : std::runtime_error {
    ErrorCode code;
    explicit DfError(ErrorCode c, std::string msg) : std::runtime_error(std::move(msg)), code(c) {}
};

[[noreturn]] inline void Throw(ErrorCode c, std::string msg) {
    throw DfError(c, std::move(msg));
}

#define DF_THROW(code, msg) ::df::Throw(::df::ErrorCode::code, (msg))

// ---- little-endian readers / writers -------------------------------------
inline uint8_t Rd8(const uint8_t* p) { return p[0]; }
inline uint16_t Rd16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
inline uint32_t Rd32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline uint64_t Rd64(const uint8_t* p) {
    return uint64_t(Rd32(p)) | (uint64_t(Rd32(p + 4)) << 32);
}
inline void Wr16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void Wr32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
}
inline void Wr64(uint8_t* p, uint64_t v) {
    Wr32(p, uint32_t(v)); Wr32(p + 4, uint32_t(v >> 32));
}

// ---- string helpers --------------------------------------------------------
std::string U16ToU8(const std::u16string& s);       // UTF-16 -> UTF-8
std::u16string U8ToU16(const std::string& s);       // UTF-8 -> UTF-16
std::string LowerCopy(std::string s);

inline bool IsPow2(uint64_t v) { return v && !(v & (v - 1)); }
inline uint64_t RoundUp(uint64_t v, uint64_t align) { return align ? ((v + align - 1) / align) * align : v; }
inline uint64_t RoundDown(uint64_t v, uint64_t align) { return align ? (v / align) * align : v; }

struct Buffer : std::vector<uint8_t> {
    Buffer() = default;
    using std::vector<uint8_t>::vector;
};

}  // namespace df
