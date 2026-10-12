#include "pch.h"

#include "AppContext.h"

#include <shlobj.h>

namespace dfapp {

AppContext& AppContext::Get() {
    static AppContext inst;
    return inst;
}

AppContext::AppContext() {
    wchar_t* roam = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roam))) {
        std::wstring dir = std::wstring(roam) + L"\\DiskForge";
        std::filesystem::create_directories(dir);
        logFilePath_ = dir + L"\\diskforge.log";
        CoTaskMemFree(roam);
        // rotate if > 2 MiB
        std::error_code ec;
        if (std::filesystem::file_size(logFilePath_, ec) > 2 * 1024 * 1024)
            std::filesystem::rename(logFilePath_, logFilePath_ + L".old", ec);
        logFile_.open(logFilePath_, std::ios::app);
    }
    LoadSettings();
}

AppContext::~AppContext() { SaveSettings(); }

std::wstring AppContext::AppDataDir() {
    wchar_t* roam = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roam))) {
        dir = std::wstring(roam) + L"\\DiskForge";
        CoTaskMemFree(roam);
        std::filesystem::create_directories(dir);
    }
    return dir;
}

void AppContext::SetUiDispatcher(std::function<void(UiThreadFn)> post) { postToUi_ = std::move(post); }

void AppContext::PostToUi(UiThreadFn fn) const {
    if (postToUi_) postToUi_(std::move(fn));
}

void AppContext::RefreshDisks() {
    try {
        disks_ = df::ops::EnumerateDisks();
        Log(0, L"Enumerated " + std::to_wstring(disks_.size()) + L" disk(s)");
    } catch (const df::DfError& e) {
        Log(2, L"Disk enumeration failed: " + U8ToW(e.what()));
        disks_.clear();
    }
}

std::optional<df::ops::DiskInfo> AppContext::Disk(int number) const {
    for (auto& d : disks_)
        if (d.number == number) return d;
    return std::nullopt;
}

void AppContext::RefreshDisk(int number) {
    try {
        df::ops::DiskInfo di = df::ops::GetDisk(number);
        bool replaced = false;
        for (auto& d : disks_)
            if (d.number == number) {
                d = di;
                replaced = true;
            }
        if (!replaced) disks_.push_back(di);
    } catch (const df::DfError& e) {
        Log(1, L"Disk refresh failed: " + U8ToW(e.what()));
    }
}

void AppContext::RunJob(std::wstring title,
                        std::function<void(df::ops::Progress&)> job,
                        std::function<void(std::optional<std::wstring> error)> completion) {
    {
        std::lock_guard<std::mutex> lk(jobMutex_);
        if (running_)
            throw std::runtime_error("another operation is already running");
        running_ = true;
    }
    cancelFlag_ = std::make_shared<std::atomic<bool>>(false);
    auto cancel = cancelFlag_;
    auto titleCopy = title;

    Log(0, L"Started: " + title);

    std::thread([this, titleCopy, job, completion, cancel]() {
        std::optional<std::wstring> error;
        df::ops::Progress prog;
        prog.SetCancelFlag(cancel);
        prog.fn = [this](const std::string& stage, double frac) {
            PostToUi([this, stage, frac]() {
                (void)stage;
                (void)frac;
            });
        };
        try {
            job(prog);
        } catch (const df::DfError& e) {
            error = U8ToW(e.what());
        } catch (const std::exception& e) {
            error = U8ToW(e.what() ? e.what() : "unknown error");
        } catch (...) {
            error = L"unknown error";
        }
        Log(error ? 2 : 0,
            error ? (titleCopy + L" FAILED: " + *error) : (titleCopy + L" completed"));
        PostToUi([this, titleCopy, completion, error]() {
            {
                std::lock_guard<std::mutex> lk(jobMutex_);
                running_ = false;
            }
            if (completion) completion(error);
        });
    }).detach();
}

void AppContext::CancelCurrent() {
    if (cancelFlag_) cancelFlag_->store(true);
    Log(1, L"Cancel requested");
}

void AppContext::Log(int level, const std::wstring& message) {
    LogRecord rec;
    rec.time = std::chrono::system_clock::now();
    rec.level = level;
    rec.message = message;
    {
        std::lock_guard<std::mutex> lk(logMutex_);
        log_.push_back(rec);
        while (log_.size() > 4000) log_.pop_front();
    }
    {
        std::lock_guard<std::mutex> lk(fileMutex_);
        if (logFile_.is_open()) {
            auto t = std::chrono::system_clock::to_time_t(rec.time);
            std::tm tm{};
            localtime_s(&tm, &t);
            wchar_t line[64];
            wcsftime(line, 64, L"%Y-%m-%d %H:%M:%S", &tm);
            const wchar_t* lv = level >= 2 ? L"ERROR" : (level == 1 ? L"WARN " : L"INFO ");
            logFile_ << WToA(line) << " " << WToA(lv) << " " << WToA(message) << "\n";
            logFile_.flush();
        }
    }
}

void AppContext::Log(int level, const std::string& message) { Log(level, U8ToW(message)); }

std::vector<LogRecord> AppContext::LogSnapshot(int minLevel) const {
    std::lock_guard<std::mutex> lk(logMutex_);
    std::vector<LogRecord> out;
    for (auto& r : log_)
        if (r.level >= minLevel) out.push_back(r);
    return out;
}

std::wstring AppContext::LogFilePath() const { return logFilePath_; }

bool AppContext::ExportLog(const std::wstring& destPath) {
    std::wofstream out(destPath);
    if (!out.is_open()) return false;
    for (auto& r : LogSnapshot()) {
        auto t = std::chrono::system_clock::to_time_t(r.time);
        std::tm tm{};
        localtime_s(&tm, &t);
        wchar_t line[64];
        wcsftime(line, 64, L"%Y-%m-%d %H:%M:%S", &tm);
        out << line << L"\t" << (r.level >= 2 ? L"ERROR" : r.level == 1 ? L"WARN" : L"INFO") << L"\t" << r.message
            << L"\n";
    }
    return true;
}

void AppContext::LoadSettings() {
    try {
        std::wstring path = AppDataDir() + L"\\settings.txt";
        std::wifstream in(path);
        if (!in.is_open()) return;
        std::wstring key;
        int iv = 0;
        while (in >> key) {
            if (key == L"theme=") { in >> iv; settings_.theme = iv; }
            else if (key == L"readOnly=") { in >> iv; settings_.readOnlyMode = iv != 0; }
            else if (key == L"confirm=") { in >> iv; settings_.confirmDestructive = iv != 0; }
            else if (key == L"chunkMiB=") { in >> iv; settings_.copyChunkMiB = uint64_t(iv ? iv : 8); }
            else { in >> iv; }
        }
    } catch (...) {
    }
}

void AppContext::SaveSettings() {
    try {
        std::wstring path = AppDataDir() + L"\\settings.txt";
        std::wofstream out(path);
        if (!out.is_open()) return;
        out << L"theme= " << settings_.theme << L"\n";
        out << L"readOnly= " << (settings_.readOnlyMode ? 1 : 0) << L"\n";
        out << L"confirm= " << (settings_.confirmDestructive ? 1 : 0) << L"\n";
        out << L"chunkMiB= " << settings_.copyChunkMiB << L"\n";
    } catch (...) {
    }
}

// ---- string conversions ----------------------------------------------------
std::string AppContext::WToA(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring AppContext::AToW(const std::string& s) { return U8ToW(s); }

std::string AppContext::WToU8(const std::wstring& w) { return WToA(w); }

std::wstring AppContext::U8ToW(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(size_t(n), 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

}  // namespace dfapp
