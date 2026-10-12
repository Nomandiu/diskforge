#pragma once

#include "ResizeMoveDialog.g.h"

namespace winrt::DiskForge::Dialogs::implementation {

struct ResizeMoveDialog : ResizeMoveDialogT<ResizeMoveDialog> {
    ResizeMoveDialog();

    void SetPartition(const df::PartInfo& p, uint64_t freeBefore, uint64_t freeAfter,
                      uint64_t diskSectors, bool isGpt);

    struct Request {
        uint64_t oldStart = 0;
        uint64_t newStart = 0;
        uint64_t oldSectors = 0;
        uint64_t newSectors = 0;
    };
    Request Request() const;

    void SizeSlider_ValueChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                 winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& e);
    void SizeBox_ValueChanged(winrt::Microsoft::UI::Xaml::NumberBox const& sender,
                              winrt::Microsoft::UI::Xaml::NumberBoxValueChangedEventArgs const& e);
    void OffsetSlider_ValueChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                   winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& e);
    void OffsetBox_ValueChanged(winrt::Microsoft::UI::Xaml::NumberBox const& sender,
                                winrt::Microsoft::UI::Xaml::NumberBoxValueChangedEventArgs const& e);

private:
    void Sync();
    void RenderPreview();

    uint64_t m_partStart = 0, m_partSectors = 0;
    uint64_t m_freeBefore = 0, m_freeAfter = 0;
    uint64_t m_diskSectors = 0;
    bool m_isGpt = true;
    bool m_syncing = false;
    uint64_t m_minMiB = 16;
};

}  // namespace winrt::DiskForge::Dialogs::implementation

namespace winrt::DiskForge::Dialogs::factory_implementation {
struct ResizeMoveDialog : ResizeMoveDialogT<ResizeMoveDialog, implementation::ResizeMoveDialog> {};
}
