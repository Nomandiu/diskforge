#pragma once

#include "CreatePartitionDialog.g.h"

namespace winrt::DiskForge::Dialogs::implementation {

struct CreatePartitionDialog : CreatePartitionDialogT<CreatePartitionDialog> {
    CreatePartitionDialog();

    void SetRange(uint64_t freeStart, uint64_t freeSectors);

    struct Request {
        uint64_t startLba = 0;
        uint64_t sectors = 0;
        std::wstring fs;
        std::wstring label;
        char letter = 0;
    };
    Request Request() const;

    void SizeSlider_ValueChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                 winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& e);
    void SizeBox_ValueChanged(winrt::Microsoft::UI::Xaml::NumberBox const& sender,
                              winrt::Microsoft::UI::Xaml::NumberBoxValueChangedEventArgs const& e);

private:
    uint64_t m_freeStart = 0;
    uint64_t m_freeSectors = 0;
    bool m_syncing = false;
};

}  // namespace winrt::DiskForge::Dialogs::implementation

namespace winrt::DiskForge::Dialogs::factory_implementation {
struct CreatePartitionDialog : CreatePartitionDialogT<CreatePartitionDialog, implementation::CreatePartitionDialog> {};
}
