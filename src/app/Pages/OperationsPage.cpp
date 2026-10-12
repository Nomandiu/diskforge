#include "pch.h"

#include "OperationsPage.h"

#include "../Services/AppContext.h"
#include "../Services/UiHelpers.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using dfapp::Ctx;

namespace winrt::DiskForge::Pages::implementation {

std::vector<OperationsPage::OpRow>& OperationsPage::History() {
    static std::vector<OpRow> hist;
    return hist;
}

OperationsPage::OperationsPage() { InitializeComponent(); }

void OperationsPage::OnNavigatedTo(Navigation::NavigationEventArgs const&) { Reload(); }

void OperationsPage::RecordOperation(const std::wstring& title, const std::wstring& target) {
    History().push_back({title, target, 0});
    Ctx().Log(0, L"Queued operation: " + title);
}

void OperationsPage::MarkRunning(const std::wstring& title) {
    for (auto& h : History())
        if (h.title == title && h.state == 0) h.state = 1;
}

void OperationsPage::MarkResult(const std::wstring& title, bool failed) {
    for (auto& h : History())
        if (h.title == title) h.state = failed ? 3 : 2;
}

void OperationsPage::Reload() {
    OpsList().Items().Clear();
    auto& hist = History();
    for (auto it = hist.rbegin(); it != hist.rend(); ++it) {
        auto vm = winrt::make<VMs::OperationVM>();
        vm.Title(it->title);
        vm.Target(it->target);
        vm.State(it->state);
        vm.Status(it->state == 2 ? L"done" : it->state == 3 ? L"failed" : it->state == 1 ? L"running" : L"pending");
        OpsList().Items().Append(vm);
    }
    bool busy = Ctx().JobRunning();
    BusyRing().Visibility(busy ? Visibility::Visible : Visibility::Collapsed);
    BusyText().Text(busy ? L"An operation is running..." : L"");
}

void OperationsPage::CancelButton_Click(IInspectable const&, RoutedEventArgs const&) {
    Ctx().CancelCurrent();
    Reload();
}

void OperationsPage::ClearButton_Click(IInspectable const&, RoutedEventArgs const&) { History().clear(); Reload(); }

}  // namespace winrt::DiskForge::Pages::implementation
