#include "pch.h"

#include "SettingsPage.h"

#include "../Services/AppContext.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using dfapp::Ctx;

namespace winrt::DiskForge::Pages::implementation {

SettingsPage::SettingsPage() { InitializeComponent(); }

void SettingsPage::OnNavigatedTo(Navigation::NavigationEventArgs const&) { Load(); }

void SettingsPage::Load() {
    m_loading = true;
    auto& s = Ctx().SettingsRef();
    if (s.theme == 0) ThemeSystem().IsChecked(true);
    else if (s.theme == 1) ThemeLight().IsChecked(true);
    else ThemeDark().IsChecked(true);
    ReadOnlyToggle().IsOn(s.readOnlyMode);
    ConfirmToggle().IsOn(s.confirmDestructive);
    int idx = s.copyChunkMiB <= 4 ? 0 : s.copyChunkMiB <= 8 ? 1 : s.copyChunkMiB <= 16 ? 2 : s.copyChunkMiB <= 32 ? 3 : 4;
    ChunkCombo().SelectedIndex(idx);
    m_loading = false;
}

static void ApplyTheme(int theme) {
    // the XamlRoot content is the window's root element; setting the theme there
    // propagates to all pages hosted in the frame
    auto requested = theme == 1 ? winrt::Microsoft::UI::Xaml::ElementTheme::Light
                                : theme == 2 ? winrt::Microsoft::UI::Xaml::ElementTheme::Dark
                                             : winrt::Microsoft::UI::Xaml::ElementTheme::Default;
    if (auto root = Content().XamlRoot().Content().try_as<winrt::Microsoft::UI::Xaml::FrameworkElement>()) {
        root.RequestedTheme(requested);
    }
}

void SettingsPage::Theme_Checked(IInspectable const& sender, RoutedEventArgs const&) {
    if (m_loading) return;
    int theme = 0;
    if (sender.as<RadioButton>() == ThemeLight())
        theme = 1;
    else if (sender.as<RadioButton>() == ThemeDark())
        theme = 2;
    Ctx().SettingsRef().theme = theme;
    Ctx().SaveSettings();
    ApplyTheme(theme);
}

void SettingsPage::ReadOnlyToggle_Toggled(IInspectable const&, RoutedEventArgs const&) {
    if (m_loading) return;
    Ctx().SettingsRef().readOnlyMode = ReadOnlyToggle().IsOn();
    Ctx().SaveSettings();
    Ctx().Log(1, std::wstring(L"Read-only mode ") + (ReadOnlyToggle().IsOn() ? L"ENABLED" : L"disabled"));
}

void SettingsPage::ConfirmToggle_Toggled(IInspectable const&, RoutedEventArgs const&) {
    if (m_loading) return;
    Ctx().SettingsRef().confirmDestructive = ConfirmToggle().IsOn();
    Ctx().SaveSettings();
}

void SettingsPage::ChunkCombo_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&) {
    if (m_loading) return;
    static const uint64_t vals[] = {4, 8, 16, 32, 64};
    int idx = ChunkCombo().SelectedIndex();
    if (idx >= 0 && size_t(idx) < 5) {
        Ctx().SettingsRef().copyChunkMiB = vals[size_t(idx)];
        Ctx().SaveSettings();
    }
}

void SettingsPage::RepoButton_Click(IInspectable const&, RoutedEventArgs const&) {
    ShellExecuteW(nullptr, L"open", L"https://github.com/Nomandiu/diskforge", nullptr, nullptr, SW_SHOWNORMAL);
}

void SettingsPage::ViewLogButton_Click(IInspectable const&, RoutedEventArgs const&) {
    if (auto frame = Frame()) frame.Navigate(xaml_typename<Pages::LogPage>());
}

}  // namespace winrt::DiskForge::Pages::implementation
