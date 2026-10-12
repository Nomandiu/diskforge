#include "pch.h"

#include "DashboardPage.h"

#include "../MainWindow.h"
#include "../Services/AppContext.h"
#include "../Services/UiHelpers.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using dfapp::AppContext;
using dfapp::Ctx;

namespace winrt::DiskForge::Pages::implementation {

DashboardPage::DashboardPage() { InitializeComponent(); }

void DashboardPage::OnNavigatedTo(Navigation::NavigationEventArgs const&) { LoadDisks(); }

void DashboardPage::LoadDisks() {
    DisksList().Items().Clear();
    SummaryText().Text(L"Scanning disks...");
    Ctx().RunJob(L"Enumerate disks",
                 [](df::ops::Progress&) { Ctx().RefreshDisks(); },
                 [this](std::optional<std::wstring>) {
                     auto disks = Ctx().Disks();
                     DisksList().Items().Clear();
                     for (auto& d : disks) DisksList().Items().Append(dfui::MakeDiskVM(d));
                     uint64_t totalBytes = 0;
                     for (auto& d : disks) totalBytes += d.sizeBytes;
                     SummaryText().Text(std::to_wstring(disks.size()) + L" disk(s), " +
                                        dfui::SizeStr(totalBytes) + L" total storage");
                 });
}

void DashboardPage::RescanButton_Click(IInspectable const&, RoutedEventArgs const&) { LoadDisks(); }

void DashboardPage::DisksList_ItemClick(IInspectable const&, ItemClickEventArgs const& e) {
    if (auto vm = e.ClickedItem().try_as<VMs::DiskVM>()) {
        if (auto frame = Frame()) frame.Navigate(xaml_typename<Pages::DiskDetailPage>(), box_value(vm.Number()));
    }
}

}  // namespace winrt::DiskForge::Pages::implementation
