#pragma once

#include "DashboardPage.g.h"

namespace winrt::DiskForge::Pages::implementation {

struct DashboardPage : DashboardPageT<DashboardPage> {
    DashboardPage();

    void RescanButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void DisksList_ItemClick(winrt::Windows::Foundation::IInspectable const& sender,
                             winrt::Microsoft::UI::Xaml::Controls::ItemClickEventArgs const& e);

    void OnNavigatedTo(winrt::Microsoft::UI::Xaml::Navigation::NavigationEventArgs const& e);

private:
    void LoadDisks();
};

}  // namespace winrt::DiskForge::Pages::implementation

namespace winrt::DiskForge::Pages::factory_implementation {
struct DashboardPage : DashboardPageT<DashboardPage, implementation::DashboardPage> {};
}
