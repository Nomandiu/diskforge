#pragma once

#include "DiskDetailPage.g.h"

namespace winrt::DiskForge::Pages::implementation {

struct DiskDetailPage : DiskDetailPageT<DiskDetailPage> {
    DiskDetailPage();

    void OnNavigatedTo(winrt::Microsoft::UI::Xaml::Navigation::NavigationEventArgs const& e);
    void Reload();

    // map + list
    void MapHost_SizeChanged(winrt::Windows::Foundation::IInspectable const& sender,
                             winrt::Microsoft::UI::Xaml::SizeChangedEventArgs const& e);
    void PartsList_SelectionChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                    winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& e);

    // operations
    void CreateButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void DeleteButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void FormatButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void ResizeMoveButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                                winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void SplitButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                           winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void MergeButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                           winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void AllocateButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                              winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void OneClickButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                              winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void LabelButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                           winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void LetterButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void ActiveButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void HideButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                          winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void TypeButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                          winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void ExploreButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                             winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void InitButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                          winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
    void RecoverFilesButton_Click(winrt::Windows::Foundation::IInspectable const& sender,
                                  winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

private:
    // dialog helpers
    void ShowCreateDialog(uint64_t freeStart, uint64_t freeSectors);
    void ShowResizeMoveDialog();
    void ShowSplitDialog();
    void ShowMergeDialog();
    void ShowAllocateDialog();
    void ShowFormatDialog();
    void ShowTypeDialog();
    void ShowLetterDialog();

    void RunOp(const std::wstring& title, std::function<void(df::ops::Progress&)> op,
               const std::wstring& doneMessage);
    void Note(const std::wstring& msg, bool error = false);

    int m_disk = -1;
    df::ops::DiskInfo m_info{};
    int64_t m_selectedStart = -1;   // selected partition or free extent start (-1 none)
    bool m_selectedIsFree = false;
    uint64_t m_selectedFreeSectors = 0;
    bool m_ignoreSelection = false;

    void LoadAndRender();
    void RenderMap();
    void RenderList();
    std::optional<df::PartInfo> SelectedPartition() const;
    std::wstring SelectedName() const;
};

}  // namespace winrt::DiskForge::Pages::implementation

namespace winrt::DiskForge::Pages::factory_implementation {
struct DiskDetailPage : DiskDetailPageT<DiskDetailPage, implementation::DiskDetailPage> {};
}
