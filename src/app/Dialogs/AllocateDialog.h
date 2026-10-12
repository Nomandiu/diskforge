#pragma once

#include "AllocateDialog.g.h"

namespace winrt::DiskForge::Dialogs::implementation {

struct AllocateDialog : AllocateDialogT<AllocateDialog> {
    AllocateDialog();

    // target = partition that will receive the space
    void SetDisk(const df::ops::DiskInfo& di, uint64_t targetStart);

    struct Request {
        uint64_t donorStart = 0;
        uint64_t bytes = 0;
    };
    Request Request() const;

    void DonorCombo_SelectionChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                     winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& e);
    void AmountSlider_ValueChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                   winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& e);
    void AmountBox_ValueChanged(winrt::Microsoft::UI::Xaml::NumberBox const& sender,
                                winrt::Microsoft::UI::Xaml::NumberBoxValueChangedEventArgs const& e);

private:
    void UpdateDonorMax();

    df::ops::DiskInfo m_disk{};
    uint64_t m_targetStart = 0;
    std::vector<df::PartInfo> m_donors;
    bool m_syncing = false;
};

}  // namespace winrt::DiskForge::Dialogs::implementation

namespace winrt::DiskForge::Dialogs::factory_implementation {
struct AllocateDialog : AllocateDialogT<AllocateDialog, implementation::AllocateDialog> {};
}
