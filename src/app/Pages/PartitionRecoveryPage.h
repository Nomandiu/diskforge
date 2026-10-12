#pragma once

#include "PartitionRecoveryPage.g.h"

namespace winrt::DiskForge::Pages::implementation {

struct PartitionRecoveryPage : PartitionRecoveryPageT<PartitionRecoveryPage> {
    PartitionRecoveryPage();

    void OnNavigatedTo(winrt::Microsoft::UI::Xaml::Navigation::NavigationEventArgs const& e);

    void ScanButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                          winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void RepairGptButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                               winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void RestoreButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                             winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

private:
    void LoadDisks();
    void ShowResults(const df::ScanResult& r);
    int SelectedDisk() const;

    std::vector<df::LostPartition> m_results;
};

}  // namespace winrt::DiskForge::Pages::implementation

namespace winrt::DiskForge::Pages::factory_implementation {
struct PartitionRecoveryPage : PartitionRecoveryPageT<PartitionRecoveryPage, implementation::PartitionRecoveryPage> {};
}
