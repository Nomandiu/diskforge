#include "units.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace df {

std::string FormatSize(uint64_t bytes) {
    char buf[64];
    double b = double(bytes);
    if (bytes >= kGiB)
        std::snprintf(buf, sizeof(buf), "%.2f GiB", b / kGiB);
    else if (bytes >= kMiB)
        std::snprintf(buf, sizeof(buf), "%.2f MiB", b / kMiB);
    else if (bytes >= 1024)
        std::snprintf(buf, sizeof(buf), "%.2f KiB", b / 1024);
    else
        std::snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)bytes);
    return buf;
}

std::string FormatSectors(uint64_t sectors) { return FormatSize(sectors * kSectorSize); }

bool ParseSize(const std::string& text, uint64_t* outBytes) {
    if (text.empty()) return false;
    size_t i = 0;
    while (i < text.size() && std::isspace((unsigned char)text[i])) ++i;
    std::string num;
    while (i < text.size() && (std::isdigit((unsigned char)text[i]) || text[i] == '.')) num += text[i++];
    while (i < text.size() && std::isspace((unsigned char)text[i])) ++i;   // allow "1.5 TiB"
    std::string unit;
    while (i < text.size() && (std::isalpha((unsigned char)text[i]) || text[i] == 'i')) unit += char(std::tolower(text[i++]));
    while (i < text.size() && std::isspace((unsigned char)text[i])) ++i;
    if (!num.empty() && i != text.size()) return false;
    if (num.empty() || num == ".") return false;

    char* end = nullptr;
    double v = std::strtod(num.c_str(), &end);
    if (!end || *end != 0 || v < 0) return false;

    uint64_t mult = 1;
    if (unit == "b" || unit.empty()) mult = 1;
    else if (unit == "k" || unit == "kb" || unit == "kib") mult = 1024;
    else if (unit == "m" || unit == "mb" || unit == "mib") mult = kMiB;
    else if (unit == "g" || unit == "gb" || unit == "gib") mult = kGiB;
    else if (unit == "t" || unit == "tb" || unit == "tib") mult = kGiB * 1024ull;
    else return false;

    *outBytes = uint64_t(v * mult + 0.5);
    return true;
}

void RateMeter::Add(uint64_t bytes) {
    if (!started_) {
        start_ = std::chrono::steady_clock::now();
        started_ = true;
    }
    total_ += bytes;
}

double RateMeter::MibPerSec() const {
    if (!started_) return 0;
    auto sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    if (sec <= 0) return 0;
    return double(total_) / (kMiB * sec);
}

}  // namespace df
