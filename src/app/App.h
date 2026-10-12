#pragma once

#include "App.xaml.g.h"

namespace winrt::DiskForge::implementation {

struct App : AppT<App> {
    App();

    void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

private:
    Microsoft::UI::Xaml::Window window{ nullptr };
};

}  // namespace winrt::DiskForge::implementation
