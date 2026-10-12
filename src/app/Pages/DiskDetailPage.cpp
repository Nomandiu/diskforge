#include "pch.h"

#include "DiskDetailPage.h"

#include "../Dialogs/AllocateDialog.h"
#include "../Dialogs/CreatePartitionDialog.h"
#include "../Dialogs/MergeDialog.h"
#include "../Dialogs/ResizeMoveDialog.h"
#include "../Dialogs/SplitDialog.h"
#include "../Services/AppContext.h"
#include "../Services/UiHelpers.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Shapes;
using namespace winrt::Microsoft::UI::Input;
using namespace winrt::Microsoft::UI::Xaml::Media;
using dfapp::AppContext;
using dfapp::Ctx;

namespace winrt::DiskForge::Pages::implementation {

DiskDetailPage::DiskDetailPage() { InitializeComponent(); }

void DiskDetailPage::OnNavigatedTo(Navigation::NavigationEventArgs const& e) {
    m_disk = winrt::unbox_value_or<int32_t>(e.Parameter(), -1);
    if (m_disk < 0) {
        auto disks = Ctx().Disks();
        m_disk = disks.empty() ? -1 : disks.front().number;
    }
    LoadAndRender();
}

void DiskDetailPage::Reload() { LoadAndRender(); }

void DiskDetailPage::LoadAndRender() {
    if (m_disk < 0) {
        DiskTitle().Text(L"No disk selected");
        return;
    }
    Ctx().RunJob(L"Read disk layout",
                 [this](df::ops::Progress&) { Ctx().RefreshDisk(m_disk); },
                 [this](std::optional<std::wstring> err) {
                     if (err) {
                         Note(L"Reading the disk failed: " + *err, true);
                         return;
                     }
                     auto di = Ctx().Disk(m_disk);
                     if (!di) {
                         DiskTitle().Text(L"Disk not found");
                         return;
                     }
                     m_info = *di;
                     // header
                     std::wstring model = AppContext::U8ToW(m_info.model.empty() ? "Disk" : m_info.model);
                     DiskTitle().Text(L"Disk " + std::to_wstring(m_info.number) + L" — " + model);
                     DiskSubtitle().Text(dfui::SizeStr(m_info.sizeBytes) + L"  •  " +
                                         (m_info.isSsd ? L"SSD" : L"HDD") +
                                         (m_info.readOnly ? L"  •  READ-ONLY" : L""));
                     CapacityText().Text(dfui::SizeStr(m_info.sizeBytes));
                     StyleText().Text(m_info.layout.style == df::DiskStyle::Gpt
                                          ? L"GPT"
                                          : m_info.layout.style == df::DiskStyle::Mbr ? L"MBR" : L"—");
                     GuidText().Text(m_info.layout.style == df::DiskStyle::Gpt
                                         ? AppContext::U8ToW(m_info.layout.diskGuid.Str())
                                         : L"");
                     SectorText().Text(std::to_wstring(m_info.layout.totalSectors));
                     SectorSizeText().Text(L"logical " + std::to_wstring(m_info.sectorSize) + L"B / physical " +
                                           std::to_wstring(m_info.physicalSectorSize) + L"B");
                     BusText().Text(AppContext::U8ToW(m_info.busText()));
                     SerialText().Text(L"Serial: " + AppContext::U8ToW(m_info.serial));
                     RenderMap();
                     RenderList();
                 });
}

// ---------------------------------------------------------------------------
// rendering
// ---------------------------------------------------------------------------
void DiskDetailPage::RenderMap() {
    MapHost().Children().Clear();
    double total = double(m_info.layout.totalSectors);
    if (total <= 0) return;

    auto extents = m_info.layout.FreeExtents();

    auto addRect = [&](uint64_t start, uint64_t count, Brush fill, const std::wstring& tip, bool free,
                       double usedFrac) {
        double w = double(count) / total * 100.0;
        if (w < 0.35) w = 0.35;
        Grid g;
        g.Width(w);
        ColumnDefinition cd;
        cd.Width(GridLength(1, GridUnitType::Star));
        g.ColumnDefinitions().Append(cd);

        // progress overlay for used space
        Grid bar;
        bar.VerticalAlignment(VerticalAlignment::Stretch);
        if (usedFrac > 0) {
            Grid overlay;
            overlay.VerticalAlignment(VerticalAlignment::Stretch);
            overlay.Height(usedFrac * 100.0);
            overlay.MinHeight(0);
            // proportional used indicator
            overlay.VerticalAlignment(VerticalAlignment::Bottom);
            overlay.Background(SolidColorBrush(winrt::Microsoft::UI::Color{ 120, 255, 255, 255 }));
            // use a row-def grid so height binding isn't needed: skip overlay complexity
            (void)overlay;
        }
        (void)bar;

        Border b;
        b.Background(fill);
        b.CornerRadius({ 4 });
        b.BorderThickness({ 1 });
        b.BorderBrush(SolidColorBrush(winrt::Microsoft::UI::Color{ 90, 0, 0, 0 }));
        b.VerticalAlignment(VerticalAlignment::Stretch);
        ToolTipService::SetToolTip(b, winrt::box_value(tip));
        b.Tag(winrt::box_value(int64_t(start)));
        b.PointerPressed([=](IInspectable const&, Input::PointerRoutedEventArgs const&) {
            SelectByStart(int64_t(start));
        });
        g.Children().Append(b);
        MapHost().Children().Append(g);
    };

    // allocated partitions
    for (auto& p : m_info.layout.parts) {
        if (p.sectors == 0) continue;
        double usedFrac = 0;
        if (p.usedBytes || p.freeBytes) usedFrac = double(p.usedBytes) / double(p.usedBytes + p.freeBytes);
        std::wstring tip = (p.letter ? std::wstring(1, wchar_t(p.letter)) + L": " : L"") +
                           AppContext::U8ToW(p.label) + L" (" + AppContext::U8ToW(p.fs.empty() ? "RAW" : p.fs) +
                           L") — " + dfui::SizeStr(p.sectors * 512);
        // logical partitions render inside the extended container; still show them
        addRect(p.start, p.isLogical ? p.sectors : p.sectors, dfui::PartitionBrush(p), tip, false, usedFrac);
    }
    // unallocated
    for (auto& e : extents) {
        addRect(e.start, e.sectors,
                Application::Current().Resources().Lookup(box_value(L"UnallocatedBrush")).as<Brush>(),
                L"Unallocated — " + dfui::SizeStr(e.sectors * 512), true, 0);
    }
}

void DiskDetailPage::RenderList() {
    m_ignoreSelection = true;
    PartsList().Items().Clear();
    for (auto& p : m_info.layout.parts) {
        if (p.sectors == 0) continue;
        PartsList().Items().Append(dfui::MakePartitionVM(p));
    }
    for (auto& e : m_info.layout.FreeExtents()) {
        df::PartInfo fake;
        fake.start = e.start;
        fake.sectors = e.sectors;
        auto vm = dfui::MakePartitionVM(fake);
        vm.Title(L"Unallocated space");
        vm.FsText(L"");
        vm.Subtitle(L"Unallocated — " + dfui::SizeStr(e.sectors * 512));
        vm.FillBrush(Application::Current().Resources().Lookup(box_value(L"UnallocatedBrush")).as<Brush>());
        PartsList().Items().Append(vm);
    }
    m_ignoreSelection = false;
}

void DiskDetailPage::SelectByStart(int64_t start) {
    m_selectedStart = start;
    // sync list selection
    m_ignoreSelection = true;
    uint32_t i = 0;
    for (auto&& item : PartsList().Items()) {
        if (auto vm = item.try_as<VMs::PartitionVM>()) {
            if (vm.StartLba() == start) {
                PartsList().SelectedIndex(i);
                break;
            }
        }
        ++i;
    }
    m_ignoreSelection = false;
}

void DiskDetailPage::MapHost_SizeChanged(IInspectable const&, SizeChangedEventArgs const&) { RenderMap(); }

void DiskDetailPage::PartsList_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&) {
    if (m_ignoreSelection) return;
    if (auto vm = PartsList().SelectedItem().try_as<VMs::PartitionVM>()) {
        m_selectedStart = vm.StartLba();
        m_selectedIsFree = false;
        m_selectedFreeSectors = 0;
        // unallocated items have RAW-empty fs and title "Unallocated"
        if (vm.Title() == L"Unallocated space") {
            m_selectedIsFree = true;
            m_selectedFreeSectors = uint64_t(vm.Sectors());
        }
    }
}

std::optional<df::PartInfo> DiskDetailPage::SelectedPartition() const {
    if (m_selectedStart < 0 || m_selectedIsFree) return std::nullopt;
    for (auto& p : m_info.layout.parts)
        if (int64_t(p.start) == m_selectedStart) return p;
    return std::nullopt;
}

std::wstring DiskDetailPage::SelectedName() const {
    if (m_selectedIsFree) return L"unallocated space";
    auto p = SelectedPartition();
    if (!p) return L"(nothing)";
    if (p->letter) return std::wstring(1, wchar_t(p->letter)) + L": (" + AppContext::U8ToW(p->label) + L")";
    return AppContext::U8ToW(p->label.empty() ? p->KindName() : p->label);
}

void DiskDetailPage::Note(const std::wstring& msg, bool error) {
    InfoBar().IsOpen(true);
    InfoBar().Message(msg);
    InfoBar().Severity(error ? InfoBarSeverity::Error : InfoBarSeverity::Informational);
}

void DiskDetailPage::RunOp(const std::wstring& title, std::function<void(df::ops::Progress&)> op,
                           const std::wstring& doneMessage) {
    if (Ctx().SettingsRef().readOnlyMode) {
        Note(L"Read-only mode is enabled — disable it in Settings first.", true);
        return;
    }
    Ctx().RunJob(title, std::move(op), [this, title, doneMessage](std::optional<std::wstring> err) {
        if (err)
            Note(title + L" failed: " + *err, true);
        else if (!doneMessage.empty())
            Note(doneMessage);
        LoadAndRender();
    });
}

// ---------------------------------------------------------------------------
// operations
// ---------------------------------------------------------------------------
void DiskDetailPage::CreateButton_Click(IInspectable const&, RoutedEventArgs const&) {
    if (m_selectedStart < 0 || !m_selectedIsFree) {
        Note(L"Select an unallocated area on the map or in the list first.");
        return;
    }
    ShowCreateDialog(uint64_t(m_selectedStart), m_selectedFreeSectors);
}

void DiskDetailPage::ShowCreateDialog(uint64_t freeStart, uint64_t freeSectors) {
    auto dlg = winrt::make<Dialogs::CreatePartitionDialog>();
    auto impl = winrt::get_self<Dialogs::implementation::CreatePartitionDialog>(dlg);
    impl->SetRange(freeStart, freeSectors);
    ContentDialog host;
    host.Title(box_value(L"Create partition"));
    host.Content(dlg);
    host.PrimaryButtonText(L"Create");
    host.CloseButtonText(L"Cancel");
    host.DefaultButton(ContentDialogButton::Primary);
    host.XamlRoot(Content().XamlRoot());
    auto op = host.ShowAsync();
    op.Completed([this, dlg, impl](auto&& o, auto&&) {
        if (o.GetResults() == ContentDialogResult::Primary) {
            auto req = impl->Request();
            RunOp(L"Create partition",
                  [this, req](df::ops::Progress& p) {
                      df::ops::CreatePartition(m_disk, req.startLba, req.sectors, req.fs,
                                               AppContext::WToU8(req.label), req.letter, p);
                  },
                  L"Partition created and formatted.");
        }
    });
}

void DiskDetailPage::DeleteButton_Click(IInspectable const&, RoutedEventArgs const&) {
    auto p = SelectedPartition();
    if (!p) {
        Note(L"Select a partition first.");
        return;
    }
    auto name = SelectedName();
    dfui::ConfirmDialog(
        Content().XamlRoot(), L"Delete partition",
        L"Delete " + name + L"? ALL DATA ON IT WILL BE LOST. This cannot be undone unless you "
                            L"later recover it with partition recovery.",
        true, [this, p, name](int r) {
            if (r != 0) return;
            RunOp(L"Delete " + name,
                  [this, p](df::ops::Progress& prog) { df::ops::DeletePartition(m_disk, p->start, prog); },
                  L"Partition deleted.");
        });
}

void DiskDetailPage::ShowFormatDialog() {
    auto p = SelectedPartition();
    if (!p) return;
    auto implSelf = this;
    dfui::TextInputDialog(
        Content().XamlRoot(), L"Format " + SelectedName(),
        L"File system (NTFS, FAT32 or exFAT) and volume label, separated by a comma.\ne.g.  NTFS, My Files",
        AppContext::U8ToW(p->fs.empty() ? std::string("NTFS") : p->fs) + L", " + AppContext::U8ToW(p->label),
        [this, p](std::optional<std::wstring> text) {
            if (!text) return;
            std::wstring fs = *text, label;
            auto comma = text->find(L',');
            if (comma != std::wstring::npos) {
                fs = text->substr(0, comma);
                label = text->substr(comma + 1);
                while (!fs.empty() && fs.back() == L' ') fs.pop_back();
                while (!label.empty() && label.front() == L' ') label.erase(label.begin());
            }
            std::string fsA = AppContext::WToU8(fs);
            if (fsA != "NTFS" && fsA != "FAT32" && fsA != "exFAT") {
                Note(L"File system must be NTFS, FAT32 or exFAT.", true);
                return;
            }
            dfui::ConfirmDialog(
                Content().XamlRoot(), L"Format " + SelectedName(),
                L"Format " + SelectedName() + L" as " + fs + L"? ALL DATA WILL BE ERASED.", true,
                [this, p, fsA, label](int r) {
                    if (r != 0) return;
                    RunOp(L"Format",
                          [this, p, fsA, label](df::ops::Progress& prog) {
                              df::ops::FormatPartition(m_disk, p->start, fsA, AppContext::WToU8(label), true, prog);
                          },
                          L"Format complete.");
                });
        });
}

void DiskDetailPage::FormatButton_Click(IInspectable const&, RoutedEventArgs const&) {
    if (!SelectedPartition()) {
        Note(L"Select a partition first.");
        return;
    }
    ShowFormatDialog();
}

void DiskDetailPage::ShowResizeMoveDialog() {
    auto p = SelectedPartition();
    if (!p) return;
    // compute bounds: free space before/after considering adjacent free extents
    uint64_t freeBefore = 0, freeAfter = 0;
    for (auto& e : m_info.layout.FreeExtents()) {
        if (e.End() == p->start) freeBefore = e.sectors;
        if (e.start == p->start + p->sectors) freeAfter = e.sectors;
    }
    auto dlg = winrt::make<Dialogs::ResizeMoveDialog>();
    auto impl = winrt::get_self<Dialogs::implementation::ResizeMoveDialog>(dlg);
    impl->SetPartition(*p, freeBefore, freeAfter, m_info.layout.totalSectors,
                       m_info.layout.style == df::DiskStyle::Gpt);
    ContentDialog host;
    host.Title(box_value(L"Resize / move — " + SelectedName()));
    host.Content(dlg);
    host.PrimaryButtonText(L"Apply");
    host.CloseButtonText(L"Cancel");
    host.DefaultButton(ContentDialogButton::Primary);
    host.XamlRoot(Content().XamlRoot());
    auto op = host.ShowAsync();
    op.Completed([this, dlg, impl](auto&& o, auto&&) {
        if (o.GetResults() == ContentDialogResult::Primary) {
            auto req = impl->Request();
            auto name = SelectedName();
            if (req.newStart != req.oldStart && req.newSectors == req.oldSectors) {
                dfui::ConfirmDialog(Content().XamlRoot(), L"Move partition",
                                    L"Move " + name + L" to offset " +
                                        dfui::SizeStr(req.newStart * 512) + L"?",
                                    false, [this, req](int r) {
                                        if (r != 0) return;
                                        RunOp(L"Move partition",
                                              [this, req](df::ops::Progress& p) {
                                                  df::ops::MovePartition(m_disk, req.oldStart, req.newStart, p);
                                              },
                                              L"Move complete.");
                                    });
            } else if (req.newSectors > req.oldSectors) {
                RunOp(L"Extend partition",
                      [this, req](df::ops::Progress& p) {
                          df::ops::ExtendPartition(m_disk, req.oldStart, req.newSectors, p);
                      },
                      L"Extend complete.");
            } else {
                RunOp(L"Shrink partition",
                      [this, req](df::ops::Progress& p) {
                          df::ops::ShrinkPartition(m_disk, req.oldStart, req.newSectors, p);
                      },
                      L"Shrink complete.");
            }
        }
    });
}

void DiskDetailPage::ResizeMoveButton_Click(IInspectable const&, RoutedEventArgs const&) {
    if (!SelectedPartition()) {
        Note(L"Select a partition first.");
        return;
    }
    ShowResizeMoveDialog();
}

void DiskDetailPage::ShowSplitDialog() {
    auto p = SelectedPartition();
    if (!p) return;
    auto dlg = winrt::make<Dialogs::SplitDialog>();
    auto impl = winrt::get_self<Dialogs::implementation::SplitDialog>(dlg);
    impl->SetPartition(*p);
    ContentDialog host;
    host.Title(box_value(L"Split — " + SelectedName()));
    host.Content(dlg);
    host.PrimaryButtonText(L"Split");
    host.CloseButtonText(L"Cancel");
    host.DefaultButton(ContentDialogButton::Primary);
    host.XamlRoot(Content().XamlRoot());
    auto op = host.ShowAsync();
    op.Completed([this, dlg, impl](auto&& o, auto&&) {
        if (o.GetResults() == ContentDialogResult::Primary) {
            auto req = impl->Request();
            RunOp(L"Split partition",
                  [this, req, p](df::ops::Progress& prog) {
                      df::ops::SplitPartition(m_disk, p->start, req.keepSectors,
                                              AppContext::WToU8(req.fs), AppContext::WToU8(req.label), prog);
                  },
                  L"Split complete.");
        }
    });
}

void DiskDetailPage::SplitButton_Click(IInspectable const&, RoutedEventArgs const&) {
    if (!SelectedPartition()) {
        Note(L"Select a partition first.");
        return;
    }
    ShowSplitDialog();
}

void DiskDetailPage::ShowMergeDialog() {
    auto p = SelectedPartition();
    if (!p || p->fs.empty()) {
        Note(L"Select a formatted partition as the merge target first.");
        return;
    }
    // collect merge sources: adjacent formatted partitions
    std::vector<df::PartInfo> candidates;
    for (auto& q : m_info.layout.parts) {
        if (q.start == p->start || q.fs.empty() || q.sectors == 0) continue;
        bool adjacent = (q.start == p->start + p->sectors) || (p->start == q.start + q.sectors);
        if (adjacent) candidates.push_back(q);
    }
    if (candidates.empty()) {
        Note(L"No adjacent formatted partition to merge into " + SelectedName() + L".");
        return;
    }
    auto dlg = winrt::make<Dialogs::MergeDialog>();
    auto impl = winrt::get_self<Dialogs::implementation::MergeDialog>(dlg);
    impl->SetCandidates(*p, candidates);
    ContentDialog host;
    host.Title(box_value(L"Merge into " + SelectedName()));
    host.Content(dlg);
    host.PrimaryButtonText(L"Merge");
    host.CloseButtonText(L"Cancel");
    host.DefaultButton(ContentDialogButton::Primary);
    host.XamlRoot(Content().XamlRoot());
    auto op = host.ShowAsync();
    op.Completed([this, dlg, impl, p](auto&& o, auto&&) {
        if (o.GetResults() == ContentDialogResult::Primary) {
            auto req = impl->Request();
            dfui::ConfirmDialog(
                Content().XamlRoot(), L"Merge partitions",
                L"Merge the source partition into " + SelectedName() +
                    L"? Its files will be copied to a folder first, then the partition is removed.",
                true, [this, p, req](int r) {
                    if (r != 0) return;
                    RunOp(L"Merge partitions",
                          [this, p, req](df::ops::Progress& prog) {
                              df::ops::MergePartitions(m_disk, p->start, req.sourceStart, prog);
                          },
                          L"Merge complete.");
                });
        }
    });
}

void DiskDetailPage::MergeButton_Click(IInspectable const&, RoutedEventArgs const&) {
    ShowMergeDialog();
}

void DiskDetailPage::ShowAllocateDialog() {
    auto target = SelectedPartition();
    if (!target) {
        Note(L"Select the TARGET partition that should receive space first.");
        return;
    }
    auto dlg = winrt::make<Dialogs::AllocateDialog>();
    auto impl = winrt::get_self<Dialogs::implementation::AllocateDialog>(dlg);
    impl->SetDisk(m_info, target->start);
    ContentDialog host;
    host.Title(box_value(L"Allocate free space"));
    host.Content(dlg);
    host.PrimaryButtonText(L"Allocate");
    host.CloseButtonText(L"Cancel");
    host.DefaultButton(ContentDialogButton::Primary);
    host.XamlRoot(Content().XamlRoot());
    auto op = host.ShowAsync();
    op.Completed([this, dlg, impl, target](auto&& o, auto&&) {
        if (o.GetResults() == ContentDialogResult::Primary) {
            auto req = impl->Request();
            RunOp(L"Allocate free space",
                  [this, req, target](df::ops::Progress& p) {
                      df::ops::AllocateFreeSpace(m_disk, req.donorStart, target->start, req.bytes, p);
                  },
                  L"Space allocated and target extended.");
        }
    });
}

void DiskDetailPage::AllocateButton_Click(IInspectable const&, RoutedEventArgs const&) {
    ShowAllocateDialog();
}

void DiskDetailPage::OneClickButton_Click(IInspectable const&, RoutedEventArgs const&) {
    auto p = SelectedPartition();
    if (!p) {
        Note(L"Select the partition to extend first.");
        return;
    }
    auto name = SelectedName();
    dfui::ConfirmDialog(
        Content().XamlRoot(), L"1-click extend",
        L"Automatically find unallocated space on this disk and extend " + name +
            L". If needed, intervening partitions will be moved (this can take a while).",
        false, [this, p, name](int r) {
            if (r != 0) return;
            RunOp(L"1-click extend " + name,
                  [this, p](df::ops::Progress& prog) {
                      uint64_t added = df::ops::OneClickExtend(m_disk, p->start, 0, prog);
                      Ctx().Log(0, L"1-click extend added " + dfui::SizeStr(added));
                  },
                  L"1-click extend complete.");
        });
}

void DiskDetailPage::LabelButton_Click(IInspectable const&, RoutedEventArgs const&) {
    auto p = SelectedPartition();
    if (!p) {
        Note(L"Select a partition first.");
        return;
    }
    dfui::TextInputDialog(
        Content().XamlRoot(), L"Set volume label", L"New label for " + SelectedName() + L":",
        AppContext::U8ToW(p->label),
        [this, p](std::optional<std::wstring> text) {
            if (!text || text->empty()) return;
            RunOp(L"Set label",
                  [this, p, text](df::ops::Progress&) {
                      df::ops::SetPartitionLabel(m_disk, p->start, AppContext::WToU8(*text));
                  },
                  L"Label updated.");
        });
}

void DiskDetailPage::ShowLetterDialog() {
    auto p = SelectedPartition();
    if (!p) return;
    std::wstring current = p->letter ? std::wstring(1, wchar_t(p->letter)) : L"(none)";
    dfui::TextInputDialog(
        Content().XamlRoot(), L"Change drive letter",
        L"New drive letter for " + SelectedName() + L" (single letter A-Z, or - to remove):", current,
        [this, p](std::optional<std::wstring> text) {
            if (!text || text->empty()) return;
            wchar_t c = (*text)[0];
            if (c == L'-' || c == L'0') {
                RunOp(L"Remove drive letter",
                      [this, p](df::ops::Progress&) { df::ops::SetDriveLetter(m_disk, p->start, 0); },
                      L"Drive letter removed.");
            } else if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z')) {
                if (c >= L'a') c = wchar_t(c - L'a' + L'A');
                RunOp(L"Change drive letter",
                      [this, p, c](df::ops::Progress&) { df::ops::SetDriveLetter(m_disk, p->start, char(c)); },
                      L"Drive letter changed.");
            } else {
                Note(L"Enter a single letter A-Z or '-' to remove.", true);
            }
        });
}

void DiskDetailPage::LetterButton_Click(IInspectable const&, RoutedEventArgs const&) {
    if (!SelectedPartition()) {
        Note(L"Select a partition first.");
        return;
    }
    ShowLetterDialog();
}

void DiskDetailPage::ActiveButton_Click(IInspectable const&, RoutedEventArgs const&) {
    auto p = SelectedPartition();
    if (!p) {
        Note(L"Select a partition first.");
        return;
    }
    if (m_info.layout.style != df::DiskStyle::Mbr) {
        Note(L"Active partitions are an MBR-disk concept; this disk is GPT.");
        return;
    }
    dfui::ConfirmDialog(Content().XamlRoot(), L"Set active partition",
                        L"Mark " + SelectedName() + L" as active (bootable)? Other partitions' active flags "
                                                     L"will be cleared.",
                        false, [this, p](int r) {
                            if (r != 0) return;
                            RunOp(L"Set active",
                                  [this, p](df::ops::Progress&) { df::ops::SetActivePartition(m_disk, p->start); },
                                  L"Active partition set.");
                        });
}

void DiskDetailPage::HideButton_Click(IInspectable const&, RoutedEventArgs const&) {
    auto p = SelectedPartition();
    if (!p) {
        Note(L"Select a partition first.");
        return;
    }
    bool hidden = (m_info.layout.style == df::DiskStyle::Gpt && (p->gptAttrs & (1ull << 63))) ||
                  (m_info.layout.style == df::DiskStyle::Mbr && df::mbr::IsHiddenType(p->mbrType));
    std::wstring verb = hidden ? L"Unhide" : L"Hide";
    dfui::ConfirmDialog(
        Content().XamlRoot(), verb + L" partition",
        verb + L" " + SelectedName() + L"? Hidden partitions lose their drive letter until unhidden.", false,
        [this, p, hidden](int r) {
            if (r != 0) return;
            RunOp(L"Hide/unhide",
                  [this, p, hidden](df::ops::Progress&) { df::ops::SetHidden(m_disk, p->start, !hidden); },
                  hidden ? L"Partition unhidden." : L"Partition hidden.");
        });
}

void DiskDetailPage::ShowTypeDialog() {
    auto p = SelectedPartition();
    if (!p) return;
    bool gpt = m_info.layout.style == df::DiskStyle::Gpt;
    std::wstring prompt = gpt
        ? L"GPT type GUID (basic, efi, msr, winre, linux, lvm, swap, bios):"
        : L"MBR type byte in hex (07 NTFS, 0C FAT32, 83 Linux, 27 WinRE, 07...):";
    std::wstring initial = gpt ? L"basic" : std::wstring(p->mbrType ? L"07" : L"07");
    if (gpt) {
        if (p->gptType == df::Guid::GptBasicData()) initial = L"basic";
        else if (p->gptType == df::Guid::GptSystemEsp()) initial = L"efi";
        else if (p->gptType == df::Guid::GptMsr()) initial = L"msr";
        else if (p->gptType == df::Guid::GptWindowsRe()) initial = L"winre";
        else if (p->gptType == df::Guid::GptLinuxFs()) initial = L"linux";
        else if (p->gptType == df::Guid::GptLinuxLvm()) initial = L"lvm";
        else if (p->gptType == df::Guid::GptLinuxSwap()) initial = L"swap";
        else if (p->gptType == df::Guid::GptBiosBoot()) initial = L"bios";
    } else {
        wchar_t buf[8];
        swprintf_s(buf, L"%02X", p->mbrType);
        initial = buf;
    }
    dfui::TextInputDialog(
        Content().XamlRoot(), L"Change partition type", prompt, initial,
        [this, p, gpt](std::optional<std::wstring> text) {
            if (!text || text->empty()) return;
            if (gpt) {
                std::string key = AppContext::WToU8(*text);
                df::Guid type;
                if (key == "basic") type = df::Guid::GptBasicData();
                else if (key == "efi") type = df::Guid::GptSystemEsp();
                else if (key == "msr") type = df::Guid::GptMsr();
                else if (key == "winre") type = df::Guid::GptWindowsRe();
                else if (key == "linux") type = df::Guid::GptLinuxFs();
                else if (key == "lvm") type = df::Guid::GptLinuxLvm();
                else if (key == "swap") type = df::Guid::GptLinuxSwap();
                else if (key == "bios") type = df::Guid::GptBiosBoot();
                else if (key.size() == 36 || key.size() == 38) type = df::Guid::Parse(key);
                else {
                    Note(L"Unknown GPT type keyword.", true);
                    return;
                }
                RunOp(L"Change GPT type",
                      [this, p, type](df::ops::Progress&) { df::ops::ChangeGptType(m_disk, p->start, type); },
                      L"Partition type changed.");
            } else {
                int v = 0;
                if (swscanf_s(text->c_str(), L"%x", &v) != 1 || v < 0 || v > 255) {
                    Note(L"Enter the type byte as 2 hex digits.", true);
                    return;
                }
                RunOp(L"Change MBR type ID",
                      [this, p, v](df::ops::Progress&) { df::ops::ChangeMbrTypeId(m_disk, p->start, uint8_t(v)); },
                      L"Partition type changed.");
            }
        });
}

void DiskDetailPage::TypeButton_Click(IInspectable const&, RoutedEventArgs const&) {
    if (!SelectedPartition()) {
        Note(L"Select a partition first.");
        return;
    }
    ShowTypeDialog();
}

void DiskDetailPage::ExploreButton_Click(IInspectable const&, RoutedEventArgs const&) {
    auto p = SelectedPartition();
    if (!p || !p->letter) {
        Note(L"Select a mounted partition with a drive letter.");
        return;
    }
    std::wstring path = std::wstring(1, wchar_t(p->letter)) + L":\\";
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void DiskDetailPage::InitButton_Click(IInspectable const&, RoutedEventArgs const&) {
    dfui::ConfirmDialog(
        Content().XamlRoot(), L"Initialize disk",
        L"Initialize this disk with a fresh partition table? This ERASES the current table (data "
        L"partitions become unallocated). GPT is recommended for modern systems.",
        true, [this](int r) {
            if (r != 0) return;
            dfui::ConfirmDialog(Content().XamlRoot(), L"Partition table style", L"Choose: Yes = GPT, No = MBR.",
                                false, [this](int r2) {
                                    if (r2 == 2) return;
                                    auto style = r2 == 0 ? df::DiskStyle::Gpt : df::DiskStyle::Mbr;
                                    RunOp(L"Initialize disk",
                                          [this, style](df::ops::Progress& p) {
                                              df::ops::InitializeDisk(m_disk, style, p);
                                          },
                                          L"Disk initialized.");
                                });
        });
}

void DiskDetailPage::RecoverFilesButton_Click(IInspectable const&, RoutedEventArgs const&) {
    auto p = SelectedPartition();
    if (!p) {
        Note(L"Select a partition first.");
        return;
    }
    if (auto frame = Frame()) frame.Navigate(xaml_typename<Pages::RecoveryPage>(), box_value(p->start));
}

}  // namespace winrt::DiskForge::Pages::implementation
