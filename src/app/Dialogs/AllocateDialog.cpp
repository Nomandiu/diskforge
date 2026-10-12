#include "pch.h"

#include "AllocateDialog.h"

#include "../Services/AppContext.h"
#include "../Services/UiHelpers.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using dfapp::AppContext;

namespace winrt::DiskForge::Dialogs::implementation {

AllocateDialog::AllocateDialog() { InitializeComponent(); }

void AllocateDialog::SetDisk(const df::ops::DiskInfo& di, uint64_t targetStart) {
    m_disk = di;
    m_targetStart = targetStart;

    const df::PartInfo* target = nullptr;
    for (auto& p : di.layout.parts)
        if (p.start == targetStart) target = &p;
    if (!target) return;
    std::wstring name = target->letter ? std::wstring(1, wchar_t(target->letter)) + L":" : AppContext::U8ToW(target->label);

    // donors: formatted data partitions other than the target with meaningful free space
    m_donors.clear();
    for (auto& p : di.layout.parts) {
        if (p.start == targetStart || p.fs.empty() || p.sectors == 0) continue;
        if (p.letter == target->letter) continue;
        m_donors.push_back(p);
    }
    // fallback: any other partition with space
    if (m_donors.empty()) {
        for (auto& p : di.layout.parts)
            if (p.start != targetStart && p.sectors > 64 * 2048) m_donors.push_back(p);
    }

    InfoText().Text(L"Move free space to " + name + L". Choose a donor partition and how much of its free "
                                                     L"space to reallocate. The target partition is extended by "
                                                     L"that amount.");
    DonorCombo().Items().Clear();
    for (auto& d : m_donors) {
        std::wstring dname = d.letter ? std::wstring(1, wchar_t(d.letter)) + L": (" + AppContext::U8ToW(d.label) + L")"
                                      : AppContext::U8ToW(d.label);
        ComboBoxItem item;
        uint64_t freeB = d.freeBytes;
        item.Content(box_value(dname + L" — " + dfui::SizeStr(d.sectors * 512) +
                               (freeB ? L" (" + dfui::SizeStr(freeB) + L" free)" : L"")));
        DonorCombo().Items().Append(item);
    }
    if (!m_donors.empty()) DonorCombo().SelectedIndex(0);
    UpdateDonorMax();
}

void AllocateDialog::UpdateDonorMax() {
    int idx = DonorCombo().SelectedIndex();
    uint64_t maxMiB = 1024;
    bool warn = false;
    if (idx >= 0 && size_t(idx) < m_donors.size()) {
        auto& d = m_donors[size_t(idx)];
        uint64_t freeB = d.freeBytes ? d.freeBytes : d.sectors * 512 / 2;
        maxMiB = std::max<uint64_t>(freeB / kMiB - 16, 16);
        // moving across other partitions requires relocation
        for (auto& p : m_disk.layout.parts) {
            if (p.start == d.start || p.start == m_targetStart || p.sectors == 0) continue;
            bool between = (p.start > d.start && p.start < m_targetStart) ||
                           (p.start > m_targetStart && p.start < d.start);
            if (between) warn = true;
        }
    }
    MoveWarn().IsOpen(warn);
    m_syncing = true;
    AmountSlider().Minimum(16);
    AmountSlider().Maximum(double(maxMiB));
    AmountSlider().Value(double(std::min<uint64_t>(maxMiB, 10240)));
    AmountBox().Minimum(16);
    AmountBox().Maximum(double(maxMiB));
    AmountBox().Value(double(std::min<uint64_t>(maxMiB, 10240)));
    m_syncing = false;
}

AllocateDialog::Request AllocateDialog::Request() const {
    Request r;
    int idx = DonorCombo().SelectedIndex();
    if (idx >= 0 && size_t(idx) < m_donors.size()) {
        r.donorStart = m_donors[size_t(idx)].start;
        r.bytes = uint64_t(AmountBox().Value()) * kMiB;
    }
    return r;
}

void AllocateDialog::DonorCombo_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&) {
    UpdateDonorMax();
}

void AllocateDialog::AmountSlider_ValueChanged(IInspectable const&,
                                               Primitives::RangeBaseValueChangedEventArgs const& e) {
    if (m_syncing) return;
    m_syncing = true;
    AmountBox().Value(e.NewValue());
    m_syncing = false;
}

void AllocateDialog::AmountBox_ValueChanged(NumberBox const&, NumberBoxValueChangedEventArgs const& e) {
    if (m_syncing) return;
    m_syncing = true;
    AmountSlider().Value(e.NewValue());
    m_syncing = false;
}

}  // namespace winrt::DiskForge::Dialogs::implementation
