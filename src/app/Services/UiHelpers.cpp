#include "pch.h"

#include "UiHelpers.h"

#include <shobjidl.h>

#include "Services/AppContext.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace winrt::DiskForge::VMs;
using dfapp::AppContext;

namespace dfui {

Brush Solid(uint8_t r, uint8_t g, uint8_t b) {
    winrt::Microsoft::UI::Color c{ 255, r, g, b };
    return SolidColorBrush(c);
}

Brush PartitionBrush(const df::PartInfo& p) {
    const std::string& fs = p.fs;
    if (p.Kind() == df::PartKind::Esp) return Solid(0xC2, 0x39, 0xB3);
    if (p.Kind() == df::PartKind::Msr) return Solid(0x8A, 0x8A, 0x8A);
    if (p.Kind() == df::PartKind::WindowsRe) return Solid(0xD1, 0x34, 0x38);
    if (p.Kind() == df::PartKind::Linux || p.Kind() == df::PartKind::LinuxSwap) return Solid(0xE2, 0xA3, 0x3C);
    if (p.Kind() == df::PartKind::BiosBoot) return Solid(0x76, 0x76, 0x76);
    if (fs == "NTFS") return Solid(0x00, 0x78, 0xD4);
    if (fs == "FAT32" || fs == "FAT16" || fs == "FAT12") return Solid(0x00, 0xB7, 0xC3);
    if (fs == "exFAT") return Solid(0x87, 0x64, 0xB8);
    if (!fs.empty()) return Solid(0x00, 0x78, 0xD4);
    return Solid(0x54, 0x54, 0x54);  // raw / unknown
}

std::wstring SizeStr(uint64_t bytes) { return AppContext::U8ToW(df::FormatSize(bytes)); }

DiskVM MakeDiskVM(const df::ops::DiskInfo& d) {
    auto vm = make<DiskVM>();
    vm.Number(d.number);
    std::wstring model = AppContext::U8ToW(d.model.empty() ? "Disk" : d.model);
    vm.Title(L"Disk " + std::to_wstring(d.number) + L"  —  " + model);
    vm.SizeText(SizeStr(d.sizeBytes));
    std::wstring bus = AppContext::U8ToW(d.busType);
    if (d.isSsd) bus += L" (SSD)";
    else if (!d.busType.empty()) bus += L" (HDD)";
    vm.BusText(bus);
    vm.StyleText(d.layout.style == df::DiskStyle::Gpt
                     ? L"GPT"
                     : (d.layout.style == df::DiskStyle::Mbr ? L"MBR" : L"Uninitialized"));
    int parts = 0;
    for (auto& p : d.layout.parts)
        if (p.sectors && !p.isLogical) ++parts;
    vm.PartitionCount(parts);
    vm.ModelText(model);
    vm.SerialText(AppContext::U8ToW(d.serial));
    vm.IsSsd(d.isSsd);
    vm.IsRemovable(d.isRemovable);
    vm.IsReadOnly(d.readOnly);
    bool system = false;
    for (auto& v : d.volumes)
        if (v.isSystem) system = true;
    vm.IsSystemDisk(system);
    std::wstring detail = vm.SizeText() + L"  •  " + vm.BusText() + L"  •  " + vm.StyleText() + L"  •  " +
                          std::to_wstring(parts) + L" partition(s)";
    if (system) detail += L"  •  System";
    if (d.readOnly) detail += L"  •  Read-only";
    vm.Detail(detail);
    return vm;
}

PartitionVM MakePartitionVM(const df::PartInfo& p) {
    auto vm = make<PartitionVM>();
    vm.StartLba(int64_t(p.start));
    vm.Sectors(int64_t(p.sectors));
    std::wstring label = AppContext::U8ToW(p.label);
    if (p.letter) {
        vm.HasLetter(true);
        vm.LetterText(std::wstring(L"(") + wchar_t(p.letter) + L":)");
        vm.Title((label.empty() ? L"Local Disk" : label) + L" " + vm.LetterText());
    } else {
        std::wstring kindName = AppContext::U8ToW(p.KindName());
        vm.Title(label.empty() ? kindName : label);
    }
    vm.FsText(p.fs.empty() ? L"RAW" : AppContext::U8ToW(p.fs));
    vm.SizeText(SizeStr(p.sectors * 512));
    vm.StartText(L"Offset " + SizeStr(p.start * 512));
    vm.KindText(AppContext::U8ToW(p.KindName()));
    double used = 0;
    if (p.usedBytes || p.freeBytes) {
        uint64_t total = p.usedBytes + p.freeBytes;
        if (total) used = double(p.usedBytes) / double(total);
    }
    vm.UsedFraction(used);
    vm.UsedText(used > 0 ? SizeStr(p.usedBytes) + L" used" : L"");
    vm.FillBrush(PartitionBrush(p));
    vm.IsEsp(p.Kind() == df::PartKind::Esp);
    vm.Subtitle(vm.FsText() + L" — " + vm.SizeText() +
                (used > 0 ? L"  (" + SizeStr(p.usedBytes) + L" used)" : L""));
    return vm;
}

FileVM MakeFileVM(const df::ops::RecoveryHit& h, int index) {
    auto vm = make<FileVM>();
    vm.Name(AppContext::U8ToW(h.name));
    vm.Path(AppContext::U8ToW(h.path));
    vm.SizeText(SizeStr(h.size) + (h.sizeEstimated ? L" (est.)" : L""));
    vm.Category(AppContext::U8ToW(h.category));
    vm.TypeName(AppContext::U8ToW(h.typeName));
    vm.Deleted(h.deleted);
    vm.Carved(h.carved);
    vm.Orphaned(h.orphaned);
    vm.SizeEstimated(h.sizeEstimated);
    vm.Index(index);
    // category glyph (Segoe Fluent Icons)
    const wchar_t* glyph = L"\xE7C3";  // generic file
    if (h.category == "Photo") glyph = L"\xEB9F";
    else if (h.category == "Video") glyph = L"\xE8B2";
    else if (h.category == "Audio") glyph = L"\xE8D6";
    else if (h.category == "Document") glyph = L"\xE8A5";
    else if (h.category == "Archive") glyph = L"\xE8E1";
    vm.Glyph(glyph);
    return vm;
}

LostPartitionVM MakeLostVM(const df::LostPartition& lp) {
    auto vm = make<LostPartitionVM>();
    vm.PositionText(L"Offset " + SizeStr(lp.start * 512));
    vm.SizeText(SizeStr(lp.sectors * 512));
    vm.FsText(AppContext::U8ToW(lp.fs));
    vm.LabelText(AppContext::U8ToW(lp.label.empty() ? lp.fs : lp.label));
    vm.EvidenceText(AppContext::U8ToW(lp.evidence));
    vm.Confidence(lp.confidence);
    wchar_t buf[32];
    swprintf_s(buf, L"%.0f%% match", lp.confidence * 100.0);
    vm.ConfidenceText(buf);
    vm.Overlaps(lp.overlapsExisting);
    vm.StartLba(int64_t(lp.start));
    vm.Sectors(int64_t(lp.sectors));
    return vm;
}

LogEntryVM MakeLogVM(const dfapp::LogRecord& r) {
    auto vm = make<LogEntryVM>();
    auto t = std::chrono::system_clock::to_time_t(r.time);
    std::tm tm{};
    localtime_s(&tm, &t);
    wchar_t buf[32];
    wcsftime(buf, 32, L"%H:%M:%S", &tm);
    vm.TimeText(buf);
    vm.LevelText(r.level >= 2 ? L"ERROR" : r.level == 1 ? L"WARN" : L"INFO");
    vm.Level(r.level);
    vm.Message(r.message);
    return vm;
}

// ---- async dialogs -----------------------------------------------------------
void TextInputDialog(XamlRoot root, const std::wstring& title, const std::wstring& prompt,
                     const std::wstring& initial, std::function<void(std::optional<std::wstring>)> done) {
    TextBlock tbPrompt;
    tbPrompt.Text(prompt);
    tbPrompt.TextWrapping(TextWrapping::Wrap);
    TextBox box;
    box.Text(initial);
    box.MinWidth(340);
    try {
        box.SelectAll();
    } catch (...) {
    }
    StackPanel sp;
    sp.Spacing(10);
    sp.Children().Append(tbPrompt);
    sp.Children().Append(box);

    ContentDialog dlg;
    dlg.Title(winrt::box_value(title));
    dlg.Content(sp);
    dlg.PrimaryButtonText(L"OK");
    dlg.CloseButtonText(L"Cancel");
    dlg.DefaultButton(ContentDialogButton::Primary);
    dlg.XamlRoot(root);
    auto op = dlg.ShowAsync();
    op.Completed([box, done](auto&& o, auto&&) {
        if (o.GetResults() == ContentDialogResult::Primary)
            done(std::wstring(box.Text()));
        else
            done(std::nullopt);
    });
}

void ConfirmDialog(XamlRoot root, const std::wstring& title, const std::wstring& body, bool danger,
                   std::function<void(int)> done) {
    TextBlock tb;
    tb.Text(body);
    tb.TextWrapping(TextWrapping::Wrap);
    tb.IsTextSelectionEnabled(true);

    ContentDialog dlg;
    dlg.Title(winrt::box_value(title));
    dlg.Content(tb);
    dlg.PrimaryButtonText(danger ? L"Yes, continue" : L"Yes");
    dlg.SecondaryButtonText(L"No");
    dlg.CloseButtonText(L"Cancel");
    dlg.DefaultButton(ContentDialogButton::Secondary);
    dlg.XamlRoot(root);
    auto op = dlg.ShowAsync();
    op.Completed([done](auto&& o, auto&&) {
        auto r = o.GetResults();
        done(r == ContentDialogResult::Primary ? 0 : (r == ContentDialogResult::Secondary ? 1 : 2));
    });
}

void InfoDialog(XamlRoot root, const std::wstring& title, const std::wstring& body) {
    TextBlock tb;
    tb.Text(body);
    tb.TextWrapping(TextWrapping::Wrap);
    tb.IsTextSelectionEnabled(true);
    ContentDialog dlg;
    dlg.Title(winrt::box_value(title));
    dlg.Content(tb);
    dlg.CloseButtonText(L"Close");
    dlg.XamlRoot(root);
    dlg.ShowAsync();
}

std::wstring PickFolderDialog(const std::wstring& title) {
    std::wstring out;
    IFileDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileDialog,
                                  (void**)&dlg);
    if (SUCCEEDED(hr)) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dlg->SetTitle(title.c_str());
        if (SUCCEEDED(dlg->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    out = path;
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    return out;
}

void MessageBoxWin(const std::wstring& title, const std::wstring& text, bool error) {
    MessageBoxW(nullptr, text.c_str(), title.c_str(), MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION));
}

}  // namespace dfui
