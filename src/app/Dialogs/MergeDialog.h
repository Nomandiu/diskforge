#pragma once

#include "MergeDialog.g.h"

namespace winrt::DiskForge::Dialogs::implementation {

struct MergeDialog : MergeDialogT<MergeDialog> {
    MergeDialog();

    void SetCandidates(const df::PartInfo& target, const std::vector<df::PartInfo>& sources);

    struct Request {
        uint64_t sourceStart = 0;
    };
    Request Request() const;

private:
    std::vector<df::PartInfo> m_sources;
};

}  // namespace winrt::DiskForge::Dialogs::implementation

namespace winrt::DiskForge::Dialogs::factory_implementation {
struct MergeDialog : MergeDialogT<MergeDialog, implementation::MergeDialog> {};
}
