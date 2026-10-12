#include "pch.h"

#include "PartitionRecoveryPage.h"

#include "../Services/AppContext.h"
#include "../Services/UiHelpers.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using dfapp::AppContext;
using dfapp::Ctx;

namespace winrt::DiskForge::Pages::implementation {

PartitionRecoveryPage::PartitionRecoveryPage() { InitializeComponent(); }

void PartitionRecoveryPage::OnNavigatedTo(Navigation::NavigationEventArgs const&) { LoadDisks(); }

void PartitionRecoveryPage::LoadDisks() {
    DiskCombo().Items().Clear();
    auto disks = Ctx().Disks();
    if (disks.empty()) {
        Ctx().RunJob(L"Enumerate disks", [](df::ops::Progress&) { Ctx().RefreshDisks(); },
                     [this](std::optional<std::wstring>) { LoadDisks(); });
        return;
    }
    for (auto& d : disks) {
        ComboBoxItem item;
        item.Content(box_value(L"Disk " + std::to_wstring(d.number) + L" — " +
                               AppContext::U8ToW(d.model.empty() ? "Disk" : d.model) + L" (" +
                               dfui::SizeStr(d.sizeBytes) + L")"));
        item.Tag(box_value(d.number));
        DiskCombo().Items().Append(item);
    }
    DiskCombo().SelectedIndex(0);
}

int PartitionRecoveryPage::SelectedDisk() const {
    if (auto item = DiskCombo().SelectedItem().try_as<ComboBoxItem>())
        return winrt::unbox_value_or<int32_t>(item.Tag(), -1);
    return -1;
}

void PartitionRecoveryPage::ScanButton_Click(IInspectable const&, RoutedEventArgs const&) {
    int disk = SelectedDisk();
    if (disk < 0) return;
    bool full = FullScanCheck().IsChecked().GetBool();
    auto mode = full ? df::ScanMode::Full : df::ScanMode::Quick;

    ScanRing().Visibility(Visibility::Visible);
    ScanButton().IsEnabled(false);
    ScanStatusText().Text(full ? L"Deep scanning (this can take a while)..." : L"Quick scanning...");

    Ctx().RunJob(
        L"Scan lost partitions",
        [disk, mode](df::ops::Progress& prog) {
            auto r = df::ops::ScanLostPartitions(disk, mode, prog);
            Ctx().Log(0, L"Lost partition scan: " + std::to_wstring(r.lost.size()) + L" found");
        },
        [this, disk, mode](std::optional<std::wstring> err) {
            ScanRing().Visibility(Visibility::Collapsed);
            ScanButton().IsEnabled(true);
            if (err) {
                ScanStatusText().Text(L"Scan failed");
                dfui::InfoDialog(Content().XamlRoot(), L"Scan failed", *err);
                return;
            }
            // re-run quickly (cached) to collect results for display
            auto r = df::ops::ScanLostPartitions(disk, mode, df::ops::Progress{});
            ShowResults(r);
        });
}

void PartitionRecoveryPage::ShowResults(const df::ScanResult& r) {
    m_results = r.lost;
    ResultsList().Items().Clear();
    for (auto& lp : r.lost) ResultsList().Items().Append(dfui::MakeLostVM(lp));
    ScanStatusText().Text(r.lost.empty() ? L"No lost partitions found"
                                         : std::to_wstring(r.lost.size()) + L" lost partition(s) found");
    if (r.gptPrimaryDamaged || r.gptBackupDamaged) {
        std::wstring msg = L"GPT health: ";
        msg += r.gptPrimaryDamaged ? L"primary header DAMAGED " : L"primary OK; ";
        msg += r.gptBackupDamaged ? L"backup DAMAGED. " : L"backup OK. ";
        msg += r.gptRestorePossible ? L"'Repair GPT structures' can rebuild the damaged copy." : L"";
        dfui::InfoDialog(Content().XamlRoot(), L"GPT health", msg);
    }
}

void PartitionRecoveryPage::RepairGptButton_Click(IInspectable const&, RoutedEventArgs const&) {
    int disk = SelectedDisk();
    if (disk < 0) return;
    dfui::ConfirmDialog(
        Content().XamlRoot(), L"Repair GPT structures",
        L"Rewrite both GPT header copies and the partition entry arrays from the healthiest available copy "
        L"on disk " + std::to_wstring(disk) + L"?",
        false, [this, disk](int r) {
            if (r != 0) return;
            Ctx().RunJob(
                L"Repair GPT",
                [disk](df::ops::Progress& p) { df::ops::RepairGpt(disk, p); },
                [this](std::optional<std::wstring> err) {
                    if (err)
                        dfui::InfoDialog(Content().XamlRoot(), L"Repair failed", *err);
                    else
                        dfui::InfoDialog(Content().XamlRoot(), L"Repair complete",
                                         L"GPT structures rebuilt successfully.");
                });
        });
}

void PartitionRecoveryPage::RestoreButton_Click(IInspectable const& sender, RoutedEventArgs const&) {
    auto btn = sender.as<Button>();
    auto vm = btn.DataContext().try_as<VMs::LostPartitionVM>();
    if (!vm) return;
    int disk = SelectedDisk();
    if (disk < 0) return;

    df::LostPartition lp;
    lp.start = uint64_t(vm.StartLba());
    lp.sectors = uint64_t(vm.Sectors());
    lp.overlapsExisting = vm.Overlaps();

    dfui::ConfirmDialog(
        Content().XamlRoot(), L"Restore partition",
        L"Add this partition back to the disk's partition table?\n" + vm.PositionText() + L"\n" + vm.SizeText() +
            L" (" + vm.FsText() + L")",
        false, [this, disk, lp](int r) {
            if (r != 0) return;
            Ctx().RunJob(
                L"Restore lost partition",
                [disk, lp](df::ops::Progress& p) { df::ops::RestoreLostPartition(disk, lp, p); },
                [this](std::optional<std::wstring> err) {
                    if (err)
                        dfui::InfoDialog(Content().XamlRoot(), L"Restore failed", *err);
                    else
                        dfui::InfoDialog(Content().XamlRoot(), L"Restore complete",
                                         L"Partition restored. Its files should be accessible again "
                                         L"(assign a drive letter if it has none).");
                });
        });
}

}  // namespace winrt::DiskForge::Pages::implementation
