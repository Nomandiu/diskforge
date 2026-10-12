// Shared test fixtures: sparse temp images, deterministic data
#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "df/common.h"
#include "df/diskio.h"

struct TempImage {
    std::string path;
    df::FileImageDevice* dev = nullptr;

    TempImage(uint64_t sizeBytes, const char* name) {
        static int counter = 0;
        path = std::string("/tmp/dfimg_") + name + "_" + std::to_string(++counter) + ".img";
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) throw std::runtime_error("cannot create temp image");
        if (sizeBytes) {
            std::fseek(f, long(sizeBytes - 1), SEEK_SET);
            std::fputc(0, f);
        }
        std::fclose(f);
        dev = new df::FileImageDevice(path, true);
    }
    ~TempImage() {
        delete dev;
        std::remove(path.c_str());
    }
    TempImage(const TempImage&) = delete;
    TempImage& operator=(const TempImage&) = delete;
};

// deterministic pattern generator
inline void FillPattern(uint8_t* p, size_t n, uint32_t seed) {
    uint32_t x = seed ? seed : 1;
    for (size_t i = 0; i < n; ++i) {
        x = x * 1103515245 + 12345;
        p[i] = uint8_t((x >> 16) & 0xFF);
    }
}

inline std::vector<uint8_t> MakePattern(size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    FillPattern(v.data(), n, seed);
    return v;
}
