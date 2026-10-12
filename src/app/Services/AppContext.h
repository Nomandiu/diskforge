// AppContext: engine <-> UI bridge (background jobs, progress, logging, settings)
#pragma once

#include <deque>
#include <fstream>
#include <optional>

#include "df/carve.h"
#include "df/ops.h"

namespace dfapp {

enum class JobState { Pending, Running, Done, Failed, Canceled };

struct JobProgress {
    std::wstring stage;
    double fraction = 0;
};

// One queued/committed operation (for the operations page)
struct OpRecord {
    std::wstring title;
    std::wstring target;
    std::wstring detail;
    JobState state = JobState::Pending;
};

struct LogRecord {
    std::chrono::system_clock::time_point time;
    int level;  // 0 info 1 warning 2 error
    std::wstring message;
};

struct Settings {
    int theme = 0;              // 0 system, 1 light, 2 dark
    bool readOnlyMode = false;  // global safety switch
    bool confirmDestructive = true;
    uint64_t copyChunkMiB = 8;
};

class AppContext {
public:
    static AppContext& Get();

    // ---- disks ----
    void RefreshDisks();
    const std::vector<df::ops::DiskInfo>& Disks() const { return disks_; }
    std::optional<df::ops::DiskInfo> Disk(int number) const;
    // light refresh (fs/label/letters only) used after operations
    void RefreshDisk(int number);

    // ---- jobs ----
    using UiThreadFn = std::function<void()>;
    void SetUiDispatcher(std::function<void(UiThreadFn)> post);
    void PostToUi(UiThreadFn fn) const;

    // Run `job` on a worker thread. Progress is marshaled to the UI thread.
    // completion (may be null) is marshaled too. Catches DfError -> error string.
    void RunJob(std::wstring title,
                std::function<void(df::ops::Progress&)> job,
                std::function<void(std::optional<std::wstring> error)> completion = nullptr);

    void CancelCurrent();
    bool JobRunning() const { return running_; }

    // ---- logging ----
    void Log(int level, const std::wstring& message);
    void Log(int level, const std::string& message);
    std::vector<LogRecord> LogSnapshot(int minLevel = 0) const;
    std::wstring LogFilePath() const;
    bool ExportLog(const std::wstring& destPath);

    // ---- settings ----
    Settings& SettingsRef() { return settings_; }
    void LoadSettings();
    void SaveSettings();

    // ---- misc helpers ----
    static std::wstring AppDataDir();
    static std::string WToA(const std::wstring& w);
    static std::wstring AToW(const std::string& s);
    static std::string WToU8(const std::wstring& w);
    static std::wstring U8ToW(const std::string& s);

private:
    AppContext();
    ~AppContext();
    AppContext(const AppContext&) = delete;

    std::vector<df::ops::DiskInfo> disks_;
    std::deque<LogRecord> log_;
    mutable std::mutex logMutex_;
    std::mutex jobMutex_;
    std::shared_ptr<std::atomic<bool>> cancelFlag_ = std::make_shared<std::atomic<bool>>(false);
    bool running_ = false;
    std::function<void(UiThreadFn)> postToUi_;
    Settings settings_;
    std::wstring logFilePath_;
    std::ofstream logFile_;
    std::mutex fileMutex_;
};

inline AppContext& Ctx() { return AppContext::Get(); }

}  // namespace dfapp
