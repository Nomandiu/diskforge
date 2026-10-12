#pragma once

#include "OperationsPage.g.h"

namespace winrt::DiskForge::Pages::implementation {

struct OperationsPage : OperationsPageT<OperationsPage> {
    OperationsPage();

    void OnNavigatedTo(winrt::Microsoft::UI::Xaml::Navigation::NavigationEventArgs const& e);

    void CancelButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void ClearButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                           winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

    static void RecordOperation(const std::wstring& title, const std::wstring& target);
    static void MarkRunning(const std::wstring& title);
    static void MarkResult(const std::wstring& title, bool failed);

private:
    void Reload();

    struct OpRow {
        std::wstring title, target;
        int state;  // 0 pending 1 running 2 done 3 failed
    };
    static std::vector<OpRow>& History();
};

}  // namespace winrt::DiskForge::Pages::implementation

namespace winrt::DiskForge::Pages::factory_implementation {
struct OperationsPage : OperationsPageT<OperationsPage, implementation::OperationsPage> {};
}
