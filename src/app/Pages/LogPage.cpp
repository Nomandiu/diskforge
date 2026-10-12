#include "pch.h"

#include "LogPage.h"

#include "../Services/AppContext.h"
#include "../Services/UiHelpers.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using dfapp::Ctx;

namespace winrt::DiskForge::Pages::implementation {

LogPage::LogPage() { InitializeComponent(); }

void LogPage::OnNavigatedTo(Navigation::NavigationEventArgs const&) { Reload(); }

void LogPage::Reload() {
    LogPathText().Text(Ctx().LogFilePath());
    int minLevel = LevelCombo().SelectedIndex();
    LogList().Items().Clear();
    for (auto& r : Ctx().LogSnapshot(minLevel)) LogList().Items().Append(dfui::MakeLogVM(r));
    if (LogList().Items().Size() > 0) LogList().ScrollIntoView(LogList().Items().GetAt(LogList().Items().Size() - 1));
}

void LogPage::RefreshButton_Click(IInspectable const&, RoutedEventArgs const&) { Reload(); }

void LogPage::LevelCombo_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&) { Reload(); }

void LogPage::ExportButton_Click(IInspectable const&, RoutedEventArgs const&) {
    // simple save via PickFolder + fixed name
    std::wstring dir = dfui::PickFolderDialog(L"Choose where to save the exported log");
    if (dir.empty()) return;
    std::wstring path = dir + L"\\diskforge-log-" + std::to_wstring(GetTickCount()) + L".txt";
    bool ok = Ctx().ExportLog(path);
    dfui::InfoDialog(Content().XamlRoot(), ok ? L"Export complete" : L"Export failed",
                     ok ? L"Log exported to:\n" + path : L"Could not write the export file.");
}

void LogPage::OpenFolderButton_Click(IInspectable const&, RoutedEventArgs const&) {
    std::wstring dir = dfapp::AppContext::AppDataDir();
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace winrt::DiskForge::Pages::implementation
