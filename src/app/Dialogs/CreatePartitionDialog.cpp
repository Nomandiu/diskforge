#include "pch.h"

#include "CreatePartitionDialog.h"

#include "../Services/UiHelpers.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml::Controls;

namespace winrt::DiskForge::Dialogs::implementation {

CreatePartitionDialog::CreatePartitionDialog() { InitializeComponent(); }

void CreatePartitionDialog::SetRange(uint64_t freeStart, uint64_t freeSectors) {
    m_freeStart = freeStart;
    m_freeSectors = freeSectors;
    uint64_t maxMiB = freeSectors / 2048;
    RangeText().Text(L"Unallocated area: " + dfui::SizeStr(freeSectors * 512) + L" at offset " +
                     dfui::SizeStr(freeStart * 512));
    m_syncing = true;
    SizeSlider().Maximum(double(std::max<uint64_t>(maxMiB, 16)));
    uint64_t defMiB = std::max<uint64_t>(std::min<uint64_t>(maxMiB, 10240), 16);
    SizeSlider().Value(double(defMiB));
    SizeSlider().Minimum(16);
    SizeBox().Minimum(16);
    SizeBox().Maximum(double(std::max<uint64_t>(maxMiB, 16)));
    SizeBox().Value(double(defMiB));
    m_syncing = false;
}

CreatePartitionDialog::Request CreatePartitionDialog::Request() const {
    Request r;
    uint64_t wantMiB = uint64_t(SizeBox().Value());
    uint64_t maxMiB = m_freeSectors / 2048;
    if (wantMiB < 16) wantMiB = 16;
    if (wantMiB > maxMiB) wantMiB = maxMiB;
    r.startLba = m_freeStart;
    r.sectors = wantMiB * 2048;
    auto idx = FsCombo().SelectedIndex();
    r.fs = idx == 3 ? L"" : std::wstring(FsCombo().SelectedItem().as<ComboBoxItem>().Content().as<winrt::hstring>());
    r.label = std::wstring(LabelBox().Text());
    wchar_t c = LetterBox().Text().empty() ? 0 : LetterBox().Text()[0];
    if (c >= L'a' && c <= L'z') c = wchar_t(c - L'a' + L'A');
    r.letter = (c >= L'A' && c <= L'Z') ? char(c) : 0;
    return r;
}

void CreatePartitionDialog::SizeSlider_ValueChanged(IInspectable const&,
                                                    Primitives::RangeBaseValueChangedEventArgs const& e) {
    if (m_syncing) return;
    m_syncing = true;
    SizeBox().Value(e.NewValue());
    m_syncing = false;
}

void CreatePartitionDialog::SizeBox_ValueChanged(NumberBox const&, NumberBoxValueChangedEventArgs const& e) {
    if (m_syncing) return;
    m_syncing = true;
    SizeSlider().Value(e.NewValue());
    m_syncing = false;
}

}  // namespace winrt::DiskForge::Dialogs::implementation
