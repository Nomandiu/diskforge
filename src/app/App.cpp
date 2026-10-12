#include "pch.h"

#include "App.h"
#include "MainWindow.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

namespace winrt::DiskForge::implementation {

App::App() {
    UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e) {
        try {
            wchar_t buf[512];
            swprintf_s(buf, L"[DiskForge] Unhandled XAML exception: %s\n", e.Message().c_str());
            OutputDebugStringW(buf);
        } catch (...) {
        }
    });
}

void App::OnLaunched(LaunchActivatedEventArgs const&) {
    window = make<MainWindow>();
    window.Activate();
}

}  // namespace winrt::DiskForge::implementation
