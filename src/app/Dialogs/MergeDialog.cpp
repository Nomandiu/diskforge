#include "pch.h"

#include "MergeDialog.h"

#include "../Services/AppContext.h"
#include "../Services/UiHelpers.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using dfapp::AppContext;

namespace winrt::DiskForge::Dialogs::implementation {

MergeDialog::MergeDialog() { InitializeComponent(); }

void MergeDialog::SetCandidates(const df::PartInfo& target, const std::vector<df::PartInfo>& sources) {
    m_sources = sources;
    std::wstring name = target.letter ? std::wstring(1, wchar_t(target.letter)) + L":" : AppContext::U8ToW(target.label);
    InfoText().Text(L"Merge an adjacent partition into " + name + L" (" +
                    AppContext::U8ToW(target.fs) + L", " + dfui::SizeStr(target.sectors * 512) + L").");
    SourceCombo().Items().Clear();
    for (auto& s : sources) {
        std::wstring sname = s.letter ? std::wstring(1, wchar_t(s.letter)) + L": (" + AppContext::U8ToW(s.label) + L")"
                                      : AppContext::U8ToW(s.label);
        ComboBoxItem item;
        item.Content(box_value(sname + L" — " + AppContext::U8ToW(s.fs) + L", " + dfui::SizeStr(s.sectors * 512)));
        SourceCombo().Items().Append(item);
    }
    if (!sources.empty()) SourceCombo().SelectedIndex(0);
}

MergeDialog::Request MergeDialog::Request() const {
    Request r;
    int idx = SourceCombo().SelectedIndex();
    if (idx >= 0 && size_t(idx) < m_sources.size()) r.sourceStart = m_sources[size_t(idx)].start;
    return r;
}

}  // namespace winrt::DiskForge::Dialogs::implementation
