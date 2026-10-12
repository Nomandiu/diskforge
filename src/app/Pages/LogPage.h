#pragma once

#include "LogPage.g.h"

namespace winrt::DiskForge::Pages::implementation {

struct LogPage : LogPageT<LogPage> {
    LogPage();

    void OnNavigatedTo(winrt::Microsoft::UI::Xaml::Navigation::NavigationEventArgs const& e);

    void RefreshButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                             winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void ExportButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void OpenFolderButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                                winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void LevelCombo_SelectionChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                     winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& e);

private:
    void Reload();
};

}  // namespace winrt::DiskForge::Pages::implementation

namespace winrt::DiskForge::Pages::factory_implementation {
struct LogPage : LogPageT<LogPage, implementation::LogPage> {};
}
