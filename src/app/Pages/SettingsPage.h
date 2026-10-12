#pragma once

#include "SettingsPage.g.h"

namespace winrt::DiskForge::Pages::implementation {

struct SettingsPage : SettingsPageT<SettingsPage> {
    SettingsPage();

    void OnNavigatedTo(winrt::Microsoft::UI::Xaml::Navigation::NavigationEventArgs const& e);

    void Theme_Checked(winrt::Windows::Foundation::IInspectable const& sender,
                       winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void ReadOnlyToggle_Toggled(winrt::Windows::Foundation::IInspectable const& sender,
                                winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void ConfirmToggle_Toggled(winrt::Windows::Foundation::IInspectable const& sender,
                               winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void ChunkCombo_SelectionChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                     winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& e);
    void RepoButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                          winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void ViewLogButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                             winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

private:
    void Load();
    bool m_loading = false;
};

}  // namespace winrt::DiskForge::Pages::implementation

namespace winrt::DiskForge::Pages::factory_implementation {
struct SettingsPage : SettingsPageT<SettingsPage, implementation::SettingsPage> {};
}
