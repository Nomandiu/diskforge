#include "pch.h"

#include "SplitDialog.h"

#include "../Services/AppContext.h"
#include "../Services/UiHelpers.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using dfapp::AppContext;

namespace winrt::DiskForge::Dialogs::implementation {

SplitDialog::SplitDialog() { InitializeComponent(); }

void SplitDialog::SetPartition(const df::PartInfo& p) {
    m_partSectors = p.sectors;
    uint64_t usedSectors = p.usedBytes ? (p.usedBytes + p.freeBytes) / 512 : p.sectors / 4;
    m_minMiB = std::max<uint64_t>((usedSectors + 4 * 2048) / 2048, 16);
    if (m_minMiB >= p.sectors / 2048) m_minMiB = p.sectors / 2048 / 2;

    std::wstring name = p.letter ? std::wstring(1, wchar_t(p.letter)) + L":" : AppContext::U8ToW(p.label);
    InfoText().Text(name + L" — " + dfui::SizeStr(p.sectors * 512) +
                    L". Shrinks the original, then creates a new partition in the freed space.");

    m_syncing = true;
    KeepSlider().Minimum(double(m_minMiB));
    KeepSlider().Maximum(double(p.sectors / 2048));
    KeepSlider().Value(double(p.sectors / 2048 / 2));
    KeepBox().Minimum(double(m_minMiB));
    KeepBox().Maximum(double(p.sectors / 2048));
    KeepBox().Value(double(p.sectors / 2048 / 2));
    m_syncing = false;
    UpdatePreview();
}

SplitDialog::Request SplitDialog::Request() const {
    Request r;
    uint64_t keepMiB = uint64_t(KeepBox().Value());
    keepMiB = std::min(std::max(keepMiB, m_minMiB), m_partSectors / 2048);
    r.keepSectors = keepMiB * 2048;
    auto idx = FsCombo().SelectedIndex();
    r.fs = idx == 3 ? L"" : std::wstring(FsCombo().SelectedItem().as<ComboBoxItem>().Content().as<winrt::hstring>());
    r.label = std::wstring(LabelBox().Text());
    return r;
}

void SplitDialog::UpdatePreview() {
    uint64_t keepMiB = uint64_t(KeepBox().Value());
    uint64_t newMiB = m_partSectors / 2048 > keepMiB ? m_partSectors / 2048 - keepMiB : 0;
    NewSizeText().Text(L"New partition will be " + dfui::SizeStr(newMiB * 2048 * 512));
}

void SplitDialog::KeepSlider_ValueChanged(IInspectable const&,
                                          Primitives::RangeBaseValueChangedEventArgs const& e) {
    if (m_syncing) return;
    m_syncing = true;
    KeepBox().Value(e.NewValue());
    m_syncing = false;
    UpdatePreview();
}

void SplitDialog::KeepBox_ValueChanged(NumberBox const&, NumberBoxValueChangedEventArgs const& e) {
    if (m_syncing) return;
    m_syncing = true;
    KeepSlider().Value(e.NewValue());
    m_syncing = false;
    UpdatePreview();
}

}  // namespace winrt::DiskForge::Dialogs::implementation
