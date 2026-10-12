#include "pch.h"

#include "MainWindow.h"

#include <commctrl.h>

#include "Pages/DashboardPage.h"
#include "Pages/DiskDetailPage.h"
#include "Pages/LogPage.h"
#include "Pages/OperationsPage.h"
#include "Pages/PartitionRecoveryPage.h"
#include "Pages/RecoveryPage.h"
#include "Pages/SettingsPage.h"
#include "Services/AppContext.h"
#include "Services/UiHelpers.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using dfapp::AppContext;

namespace winrt::DiskForge::implementation {

MainWindow::MainWindow() {
    // Mica backdrop + dark-friendly title bar
    InitializeComponent();

    Title(L"DiskForge — Partition & Recovery Suite");
    ExtendsContentIntoTitleBar(true);
    SetTitleBar(AppTitleBar());

    // icon + min-size
    m_hwnd = winrt::Microsoft::UI::Xaml::WindowNative::GetWindowHandle(*this);
    {
        std::wstring exePath(1024, L'\0');
        GetModuleFileNameW(nullptr, exePath.data(), 1024);
        exePath.resize(wcslen(exePath.c_str()));
        try {
            auto appWindow = AppWindow();
            appWindow.SetIcon(exePath);
        } catch (...) {
        }
    }
    SetWindowSubclass(m_hwnd, SubclassProc, 1, 0);

    // UI dispatcher for engine jobs
    Ctx().SetUiDispatcher([](AppContext::UiThreadFn fn) {
        if (auto dq = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread(); dq) {
            dq.TryEnqueue([fn]() { fn(); });
        }
    });

    Navigate(L"dashboard");
}

LRESULT CALLBACK MainWindow::SubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                          UINT_PTR uIdSubclass, DWORD_PTR) {
    if (msg == WM_GETMINMAXINFO) {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize.x = 1000;
        mmi->ptMinTrackSize.y = 640;
        return 0;
    }
    return DefSubclassProc(hWnd, msg, wParam, lParam, uIdSubclass, 0);
}

winrt::Microsoft::UI::Xaml::XamlRoot MainWindow::ContentRoot() { return ContentFrame().XamlRoot(); }

Brush MainWindow::AccentColorBrush() {
    return dfui::Solid(0x00, 0x78, 0xD4);
}

void MainWindow::Navigate(std::wstring_view tag) {
    if (tag == L"dashboard") ContentFrame().Navigate(xaml_typename<Pages::DashboardPage>());
    else if (tag == L"disk") ContentFrame().Navigate(xaml_typename<Pages::DiskDetailPage>(), box_value(m_pendingDisk));
    else if (tag == L"recovery") ContentFrame().Navigate(xaml_typename<Pages::RecoveryPage>());
    else if (tag == L"partrecovery") ContentFrame().Navigate(xaml_typename<Pages::PartitionRecoveryPage>());
    else if (tag == L"operations") ContentFrame().Navigate(xaml_typename<Pages::OperationsPage>());
    else if (tag == L"log") ContentFrame().Navigate(xaml_typename<Pages::LogPage>());
    else if (tag == L"settings") ContentFrame().Navigate(xaml_typename<Pages::SettingsPage>());
    else if (tag == L"about") ContentFrame().Navigate(xaml_typename<Pages::SettingsPage>());
    else ContentFrame().Navigate(xaml_typename<Pages::DashboardPage>());
    m_pendingDisk = -1;
}

void MainWindow::NavigateToDisk(int32_t diskNumber) {
    m_pendingDisk = diskNumber;
    // select the disk nav item then navigate
    for (auto&& item : NavView().MenuItems()) {
        if (auto navItem = item.try_as<NavigationViewItem>()) {
            if (winrt::unbox_value_or<winrt::hstring>(navItem.Tag(), L"") == L"disk") {
                NavView().SelectedItem(navItem);
                break;
            }
        }
    }
    Navigate(L"disk");
}

void MainWindow::NavView_SelectionChanged(NavigationView const&,
                                          NavigationViewSelectionChangedEventArgs const& args) {
    if (auto item = args.SelectedItem().try_as<NavigationViewItem>()) {
        auto tag = winrt::unbox_value_or<winrt::hstring>(item.Tag(), L"");
        Navigate(std::wstring_view(tag));
    }
}

void MainWindow::CancelJobButton_Click(IInspectable const&, RoutedEventArgs const&) { Ctx().CancelCurrent(); }

void MainWindow::ShowStatus(const std::wstring& text) {
    StatusText().Text(text);
    StatusBar().Visibility(Visibility::Visible);
}

void MainWindow::HideStatus() { StatusBar().Visibility(Visibility::Collapsed); }

}  // namespace winrt::DiskForge::implementation
