#include "pch.h"

#include "ResizeMoveDialog.h"

#include "../Services/AppContext.h"
#include "../Services/UiHelpers.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Shapes;
using namespace winrt::Microsoft::UI::Xaml::Media;
using dfapp::AppContext;

namespace winrt::DiskForge::Dialogs::implementation {

ResizeMoveDialog::ResizeMoveDialog() { InitializeComponent(); }

void ResizeMoveDialog::SetPartition(const df::PartInfo& p, uint64_t freeBefore, uint64_t freeAfter,
                                    uint64_t diskSectors, bool isGpt) {
    m_partStart = p.start;
    m_partSectors = p.sectors;
    m_freeBefore = freeBefore;
    m_freeAfter = freeAfter;
    m_diskSectors = diskSectors;
    m_isGpt = isGpt;

    // minimum: raw guess (used + 8 MiB) — engine enforces the true minimum
    uint64_t usedSectors = p.usedBytes ? (p.usedBytes + p.freeBytes) / 512 : p.sectors / 4;
    m_minMiB = std::max<uint64_t>((usedSectors + 4 * 2048) / 2048, 16);
    if (m_minMiB > p.sectors / 2048) m_minMiB = p.sectors / 2048;

    std::wstring name = p.letter ? std::wstring(1, wchar_t(p.letter)) + L":" : AppContext::U8ToW(p.label);
    InfoText().Text(name + L" — current size " + dfui::SizeStr(p.sectors * 512) + L". Free space before: " +
                    dfui::SizeStr(freeBefore * 512) + L", after: " + dfui::SizeStr(freeAfter * 512) +
                    L". Adjust size and position; the file system is updated automatically.");

    m_syncing = true;
    SizeSlider().Minimum(double(m_minMiB));
    SizeSlider().Maximum(double(m_partSectors / 2048 + m_freeBefore / 2048 + m_freeAfter / 2048));
    SizeSlider().Value(double(m_partSectors / 2048));
    SizeBox().Minimum(double(m_minMiB));
    SizeBox().Maximum(SizeSlider().Maximum());
    SizeBox().Value(double(m_partSectors / 2048));

    uint64_t maxOff = m_freeBefore / 2048 + m_freeAfter / 2048;
    OffsetSlider().Maximum(double(maxOff));
    OffsetSlider().Value(0);
    OffsetBox().Maximum(double(maxOff));
    OffsetBox().Value(0);
    m_syncing = false;
    RenderPreview();
}

ResizeMoveDialog::Request ResizeMoveDialog::Request() const {
    Request r;
    uint64_t sizeMiB = uint64_t(SizeBox().Value());
    uint64_t offMiB = uint64_t(OffsetBox().Value());
    uint64_t maxSectors = m_partSectors + m_freeBefore + m_freeAfter;
    r.oldStart = m_partStart;
    r.oldSectors = m_partSectors;
    r.newSectors = std::min(sizeMiB * 2048, maxSectors);
    // moving left uses freeBefore; moving right shrinks first then uses freeAfter
    uint64_t moveLeft = std::min(offMiB, m_freeBefore / 2048) * 2048;
    uint64_t extraRight = (offMiB > m_freeBefore / 2048) ? (offMiB - m_freeBefore / 2048) * 2048 : 0;
    extraRight = std::min(extraRight, m_freeAfter);
    r.newStart = m_partStart - moveLeft + extraRight;
    if (r.newSectors > maxSectors - (r.newStart - (m_partStart - m_freeBefore))) {
        // clamp to the total available window
        uint64_t windowStart = m_partStart - m_freeBefore;
        r.newSectors = maxSectors - (r.newStart - windowStart);
    }
    if (r.newSectors < m_minMiB * 2048) r.newSectors = m_minMiB * 2048;
    return r;
}

void ResizeMoveDialog::Sync() { RenderPreview(); }

void ResizeMoveDialog::SizeSlider_ValueChanged(IInspectable const&,
                                               Primitives::RangeBaseValueChangedEventArgs const& e) {
    if (m_syncing) return;
    m_syncing = true;
    SizeBox().Value(e.NewValue());
    m_syncing = false;
    Sync();
}

void ResizeMoveDialog::SizeBox_ValueChanged(NumberBox const&, NumberBoxValueChangedEventArgs const& e) {
    if (m_syncing) return;
    m_syncing = true;
    SizeSlider().Value(e.NewValue());
    m_syncing = false;
    Sync();
}

void ResizeMoveDialog::OffsetSlider_ValueChanged(IInspectable const&,
                                                 Primitives::RangeBaseValueChangedEventArgs const& e) {
    if (m_syncing) return;
    m_syncing = true;
    OffsetBox().Value(e.NewValue());
    m_syncing = false;
    Sync();
}

void ResizeMoveDialog::OffsetBox_ValueChanged(NumberBox const&, NumberBoxValueChangedEventArgs const& e) {
    if (m_syncing) return;
    m_syncing = true;
    OffsetSlider().Value(e.NewValue());
    m_syncing = false;
    Sync();
}

void ResizeMoveDialog::RenderPreview() {
    PreviewMap().Children().Clear();
    if (!m_diskSectors) return;
    auto req = Request();
    uint64_t windowStart = m_partStart - m_freeBefore;
    uint64_t windowLen = m_freeBefore + m_partSectors + m_freeAfter;

    auto append = [&](uint64_t relStart, uint64_t len, Brush b, const std::wstring& tip, bool outline) {
        Border bd;
        bd.Width(std::max(double(relStart) / double(windowLen) * 100.0, 0.4));
        bd.Background(b);
        bd.CornerRadius({ 3 });
        if (outline) {
            bd.BorderThickness({ 2 });
            bd.BorderBrush(SolidColorBrush(winrt::Microsoft::UI::Color{ 255, 255, 200, 0 }));
        }
        ToolTipService::SetToolTip(bd, box_value(tip));
        PreviewMap().Children().Append(bd);
    };

    auto gray = Application::Current().Resources().Lookup(box_value(L"UnallocatedBrush")).as<Brush>();
    auto blue = dfui::Solid(0x00, 0x78, 0xD4);

    append(0, m_freeBefore, gray, L"free before", false);
    append(m_freeBefore, m_partSectors, gray, L"current position", false);
    append(m_freeBefore + m_partSectors, m_freeAfter, gray, L"free after", false);

    // new position overlay
    uint64_t newRel = req.newStart - windowStart;
    append(newRel, req.newSectors, blue, L"new position + size", true);
}

}  // namespace winrt::DiskForge::Dialogs::implementation
