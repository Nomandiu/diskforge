// CRC32 (IEEE 802.3, zlib polynomial 0xEDB88320)
#pragma once

#include <cstddef>
#include <cstdint>

namespace df {

class Crc32 {
public:
    Crc32() : value_(0xFFFFFFFFu) {}
    void Update(const void* data, size_t len);
    uint32_t Final() const { return ~value_; }
    static uint32_t Compute(const void* data, size_t len) {
        Crc32 c; c.Update(data, len); return c.Final();
    }

private:
    uint32_t value_;
};

}  // namespace df
