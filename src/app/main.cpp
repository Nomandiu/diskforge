// DiskForge entry point
#include "pch.h"

#include "App.xaml.g.h"

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    ::winrt::Microsoft::UI::Xaml::Application::Start([](auto&&) { ::winrt::make<::winrt::DiskForge::implementation::App>(); });
    return 0;
}
