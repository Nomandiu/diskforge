// Human-readable byte formatting + size parsing + progress plumbing
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <string>

#include "common.h"

namespace df {

std::string FormatSize(uint64_t bytes);            // e.g. "128.00 GiB"
std::string FormatSectors(uint64_t sectors);       // sectors -> bytes -> FormatSize
// Parse "10G", "512MB", "1.5 TiB", "4096" (bytes) -> bytes. Returns false on failure.
bool ParseSize(const std::string& text, uint64_t* outBytes);

// ---- progress / cancellation ------------------------------------------------
struct Progress {
    std::function<void(const std::string& stage, double fraction)> fn;  // fraction 0..1
    std::shared_ptr<std::atomic<bool>> cancelFlag = std::make_shared<std::atomic<bool>>(false);

    void SetCancelFlag(std::shared_ptr<std::atomic<bool>> f) { cancelFlag = f; }
    void Report(const std::string& stage, double fraction) const {
        if (fn) fn(stage, fraction < 0 ? 0 : (fraction > 1 ? 1 : fraction));
    }
    void CheckCancel() const {
        if (cancelFlag && cancelFlag->load(std::memory_order_relaxed))
            DF_THROW(Canceled, "operation canceled by user");
    }
};

// Simple rate counter used for UI speed readouts.
struct RateMeter {
    void Add(uint64_t bytes);
    double MibPerSec() const;

private:
    uint64_t total_ = 0;
    std::chrono::steady_clock::time_point start_{};
    bool started_ = false;
};

}  // namespace df
