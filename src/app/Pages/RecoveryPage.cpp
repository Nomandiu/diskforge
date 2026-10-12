#include "pch.h"

#include "RecoveryPage.h"

#include "../Services/AppContext.h"
#include "../Services/UiHelpers.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using dfapp::AppContext;
using dfapp::Ctx;

namespace winrt::DiskForge::Pages::implementation {

RecoveryPage::RecoveryPage() { InitializeComponent(); }

void RecoveryPage::OnNavigatedTo(Navigation::NavigationEventArgs const& e) {
    m_preselectStart = winrt::unbox_value_or<uint64_t>(e.Parameter(), UINT64_MAX);
    LoadSources();
}

void RecoveryPage::LoadSources() {
    m_sources.clear();
    SourceCombo().Items().Clear();
    auto disks = Ctx().Disks();
    if (disks.empty()) {
        Ctx().RunJob(L"Enumerate disks", [](df::ops::Progress&) { Ctx().RefreshDisks(); },
                     [this](std::optional<std::wstring>) { LoadSources(); });
        return;
    }
    for (auto& d : disks) {
        for (auto& p : d.layout.parts) {
            if (p.sectors == 0) continue;
            SourceEntry se;
            se.disk = d.number;
            se.partStart = p.start;
            se.wholeDisk = false;
            std::wstring name = p.letter ? std::wstring(1, wchar_t(p.letter)) + L": " : L"";
            se.title = L"Disk " + std::to_wstring(d.number) + L" — " + name +
                       AppContext::U8ToW(p.label.empty() ? p.KindName() : p.label) + L" (" +
                       AppContext::U8ToW(p.fs.empty() ? "RAW" : p.fs) + L", " + dfui::SizeStr(p.sectors * 512) + L")";
            m_sources.push_back(se);
            ComboBoxItem item;
            item.Content(box_value(se.title));
            SourceCombo().Items().Append(item);
        }
        if (!d.readOnly) {
            SourceEntry se;
            se.disk = d.number;
            se.wholeDisk = true;
            se.title = L"Disk " + std::to_wstring(d.number) + L" — ENTIRE DISK (all partitions + unallocated)";
            m_sources.push_back(se);
            ComboBoxItem item;
            item.Content(box_value(se.title));
            SourceCombo().Items().Append(item);
        }
    }
    if (!m_sources.empty()) SourceCombo().SelectedIndex(0);
}

void RecoveryPage::ScanButton_Click(IInspectable const&, RoutedEventArgs const&) {
    if (m_sources.empty() || SourceCombo().SelectedIndex() < 0) return;
    RunScan();
}

void RecoveryPage::RunScan() {
    auto& se = m_sources[size_t(SourceCombo().SelectedIndex())];
    bool includeDeleted = DeletedCheck().IsChecked().GetBool();
    bool doCarve = CarveCheck().IsChecked().GetBool();

    ScanRing().Visibility(Visibility::Visible);
    ScanButton().IsEnabled(false);
    ScanStatusText().Text(L"Scanning...");

    Ctx().RunJob(
        L"Recovery scan",
        [this, se, includeDeleted, doCarve](df::ops::Progress& prog) {
            df::ops::CarveOptions co;
            m_session = df::ops::RecoverySession::Start(se.disk, se.wholeDisk, se.partStart, includeDeleted, doCarve,
                                                        co, prog)
                            .release();
            m_hits = m_session->Hits();
        },
        [this](std::optional<std::wstring> err) {
            ScanRing().Visibility(Visibility::Collapsed);
            ScanButton().IsEnabled(true);
            if (err) {
                ScanStatusText().Text(L"Scan failed");
                dfui::InfoDialog(Content().XamlRoot(), L"Scan failed", *err);
                return;
            }
            ScanStatusText().Text(std::to_wstring(m_hits.size()) + L" items found");
            ApplyFilter();
        });
}

void RecoveryPage::ApplyFilter() {
    m_filter = std::wstring(SearchBox().Text());
    std::wstring category;
    if (CategoryCombo().SelectedIndex() > 0) {
        auto item = CategoryCombo().SelectedItem().as<ComboBoxItem>();
        category = std::wstring(item.Content().as<hstring>());
    }
    std::wstring filterLower = m_filter;
    for (auto& c : filterLower)
        c = towlower(c);

    FilesList().Items().Clear();
    int shown = 0;
    for (size_t i = 0; i < m_hits.size(); ++i) {
        auto& h = m_hits[i];
        if (!category.empty()) {
            if (category == L"Documents" && h.category != "Document") continue;
            if (category == L"Photos" && h.category != "Photo") continue;
            if (category == L"Videos" && h.category != "Video") continue;
            if (category == L"Audio" && h.category != "Audio") continue;
            if (category == L"Archives" && h.category != "Archive") continue;
            if (category == L"Other" && h.category != "Other") continue;
        }
        if (!filterLower.empty()) {
            std::wstring hay = AppContext::U8ToW(h.name) + L" " + AppContext::U8ToW(h.path);
            for (auto& c : hay) c = towlower(c);
            if (hay.find(filterLower) == std::wstring::npos) continue;
        }
        FilesList().Items().Append(dfui::MakeFileVM(h, int(i)));
        ++shown;
    }
    ResultCount().Text(std::to_wstring(shown) + L" shown / " + std::to_wstring(m_hits.size()) + L" found");
}

void RecoveryPage::SearchBox_QuerySubmitted(AutoSuggestBox const&, AutoSuggestBoxQuerySubmittedEventArgs const&) {
    ApplyFilter();
}

void RecoveryPage::SearchBox_TextChanged(AutoSuggestBox const&, AutoSuggestBoxTextChangedEventArgs const&) {
    ApplyFilter();
}

void RecoveryPage::RecoverButton_Click(IInspectable const&, RoutedEventArgs const&) {
    // collect selected indices
    std::vector<df::ops::RecoveryHit> wanted;
    for (auto&& item : FilesList().SelectedItems()) {
        if (auto vm = item.try_as<VMs::FileVM>()) {
            int idx = vm.Index();
            if (idx >= 0 && size_t(idx) < m_hits.size()) wanted.push_back(m_hits[size_t(idx)]);
        }
    }
    if (wanted.empty()) {
        dfui::InfoDialog(Content().XamlRoot(), L"Nothing selected",
                         L"Select the files to recover in the list first (checkboxes on the left).");
        return;
    }
    if (!m_session) return;

    std::wstring dest = dfui::PickFolderDialog(L"Choose a recovery destination folder (use a DIFFERENT disk)");
    if (dest.empty()) return;

    // warn when destination is on the source disk
    if (m_sources.empty() || SourceCombo().SelectedIndex() < 0) return;
    auto& se = m_sources[size_t(SourceCombo().SelectedIndex())];
    if (se.wholeDisk && dest.size() >= 2 && dest[1] == L':') {
        wchar_t letter = wchar_t(toupper(dest[0]));
        for (auto& d : Ctx().Disks())
            for (auto& v : d.volumes)
                if (v.letter == letter && v.disk == se.disk) {
                    dfui::ConfirmDialog(
                        Content().XamlRoot(), L"Destination on the same disk",
                        L"The destination folder is on the SAME physical disk being scanned. Recovering onto "
                        L"the same disk can overwrite the deleted data. Continue anyway?",
                        true, [this, dest](int r) {
                            if (r == 0) RunRecover(dest);
                        });
                    return;
                }
    }
    RunRecover(dest);
}

void RecoveryPage::RunRecover(const std::wstring& destDir) {
    std::vector<df::ops::RecoveryHit> wanted;
    for (auto&& item : FilesList().SelectedItems()) {
        if (auto vm = item.try_as<VMs::FileVM>()) {
            int idx = vm.Index();
            if (idx >= 0 && size_t(idx) < m_hits.size()) wanted.push_back(m_hits[size_t(idx)]);
        }
    }
    ScanRing().Visibility(Visibility::Visible);
    ScanButton().IsEnabled(false);
    ScanStatusText().Text(L"Recovering " + std::to_wstring(wanted.size()) + L" file(s)...");

    Ctx().RunJob(
        L"Recover files",
        [this, wanted, destDir](df::ops::Progress& prog) {
            if (m_session) m_session->Recover(wanted, AppContext::WToU8(destDir), prog);
        },
        [this, wanted](std::optional<std::wstring> err) {
            ScanRing().Visibility(Visibility::Collapsed);
            ScanButton().IsEnabled(true);
            if (err) {
                ScanStatusText().Text(L"Recovery finished with errors");
                dfui::InfoDialog(Content().XamlRoot(), L"Recovery error", *err);
                return;
            }
            ScanStatusText().Text(L"Recovery complete");
            dfui::InfoDialog(Content().XamlRoot(), L"Recovery complete",
                             std::to_wstring(wanted.size()) + L" file(s) recovered successfully.");
        });
}

}  // namespace winrt::DiskForge::Pages::implementation
