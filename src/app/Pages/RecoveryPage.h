#pragma once

#include "RecoveryPage.g.h"

namespace winrt::DiskForge::Pages::implementation {

struct RecoveryPage : RecoveryPageT<RecoveryPage> {
    RecoveryPage();

    void OnNavigatedTo(winrt::Microsoft::UI::Xaml::Navigation::NavigationEventArgs const& e);

    void ScanButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                          winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void RecoverButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                             winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void SearchBox_QuerySubmitted(winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox const& sender,
                                  winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBoxQuerySubmittedEventArgs const& e);
    void SearchBox_TextChanged(winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox const& sender,
                               winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBoxTextChangedEventArgs const& e);

private:
    struct SourceEntry {
        int disk = -1;
        bool wholeDisk = false;
        uint64_t partStart = 0;
        std::wstring title;
    };

    void LoadSources();
    void ApplyFilter();
    void RunScan();
    void RunRecover(const std::wstring& destDir);

    std::vector<SourceEntry> m_sources;
    std::vector<df::ops::RecoveryHit> m_hits;
    std::shared_ptr<df::ops::RecoverySession> m_session;
    std::wstring m_filter;
    uint64_t m_preselectStart = UINT64_MAX;
};

}  // namespace winrt::DiskForge::Pages::implementation

namespace winrt::DiskForge::Pages::factory_implementation {
struct RecoveryPage : RecoveryPageT<RecoveryPage, implementation::RecoveryPage> {};
}
