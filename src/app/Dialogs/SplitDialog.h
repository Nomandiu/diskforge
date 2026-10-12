#pragma once

#include "SplitDialog.g.h"

namespace winrt::DiskForge::Dialogs::implementation {

struct SplitDialog : SplitDialogT<SplitDialog> {
    SplitDialog();

    void SetPartition(const df::PartInfo& p);

    struct Request {
        uint64_t keepSectors = 0;
        std::wstring fs;
        std::wstring label;
    };
    Request Request() const;

    void KeepSlider_ValueChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                 winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& e);
    void KeepBox_ValueChanged(winrt::Microsoft::UI::Xaml::NumberBox const& sender,
                              winrt::Microsoft::UI::Xaml::NumberBoxValueChangedEventArgs const& e);

private:
    void UpdatePreview();
    uint64_t m_partSectors = 0;
    uint64_t m_minMiB = 16;
    bool m_syncing = false;
};

}  // namespace winrt::DiskForge::Dialogs::implementation

namespace winrt::DiskForge::Dialogs::factory_implementation {
struct SplitDialog : SplitDialogT<SplitDialog, implementation::SplitDialog> {};
}
