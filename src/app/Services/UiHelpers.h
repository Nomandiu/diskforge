// UI helpers: VM construction, colors, async dialogs, Win32 folder picker
#pragma once

#include "pch.h"

#include "ViewModels/ViewModels.h"

namespace dfui {

// ---- brushes ----
winrt::Microsoft::UI::Xaml::Media::Brush Solid(uint8_t r, uint8_t g, uint8_t b);
winrt::Microsoft::UI::Xaml::Media::Brush PartitionBrush(const df::PartInfo& p);

// ---- VM builders ----
winrt::DiskForge::VMs::DiskVM MakeDiskVM(const df::ops::DiskInfo& d);
winrt::DiskForge::VMs::PartitionVM MakePartitionVM(const df::PartInfo& p);
winrt::DiskForge::VMs::FileVM MakeFileVM(const df::ops::RecoveryHit& h, int index);
winrt::DiskForge::VMs::LostPartitionVM MakeLostVM(const df::LostPartition& lp);
winrt::DiskForge::VMs::LogEntryVM MakeLogVM(const dfapp::LogRecord& r);

// ---- formatting ----
std::wstring SizeStr(uint64_t bytes);

// ---- async dialogs (callback style; completion runs on the UI thread) -------
void TextInputDialog(winrt::Microsoft::UI::Xaml::XamlRoot root, const std::wstring& title,
                     const std::wstring& prompt, const std::wstring& initial,
                     std::function<void(std::optional<std::wstring>)> done);

// yes=0 no=1 cancel=2
void ConfirmDialog(winrt::Microsoft::UI::Xaml::XamlRoot root, const std::wstring& title,
                   const std::wstring& body, bool danger, std::function<void(int)> done);

void InfoDialog(winrt::Microsoft::UI::Xaml::XamlRoot root, const std::wstring& title,
                const std::wstring& body);

// ---- Win32 folder picker (works elevated) ------------------------------------
std::wstring PickFolderDialog(const std::wstring& title);

void MessageBoxWin(const std::wstring& title, const std::wstring& text, bool error);

}  // namespace dfui
