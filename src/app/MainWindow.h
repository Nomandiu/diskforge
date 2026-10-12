#pragma once

#include "MainWindow.g.h"

namespace winrt::DiskForge::implementation {

struct MainWindow : MainWindowT<MainWindow> {
    MainWindow();

    void NavView_SelectionChanged(winrt::Microsoft::UI::Xaml::Controls::NavigationView const& sender,
                                  winrt::Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const& args);
    void CancelJobButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                               winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

    void NavigateToDisk(int32_t diskNumber);
    winrt::Microsoft::UI::Xaml::XamlRoot ContentRoot();

    winrt::Microsoft::UI::Xaml::Media::Brush AccentColorBrush();

    void ShowStatus(const std::wstring& text);
    void HideStatus();

private:
    void Navigate(std::wstring_view tag);
    void ApplyMinSize();
    static LRESULT CALLBACK SubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                         UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
    HWND m_hwnd = nullptr;
    int32_t m_pendingDisk = -1;
};

}  // namespace winrt::DiskForge::implementation

namespace winrt::DiskForge::factory_implementation {
struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}
