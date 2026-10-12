#include "crc32.h"

namespace df {

static uint32_t Table[256];
static bool InitTable() {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        Table[i] = c;
    }
    return true;
}
static const bool kInited = InitTable();

void Crc32::Update(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t c = value_;
    for (size_t i = 0; i < len; ++i) c = Table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    value_ = c;
}

}  // namespace df
