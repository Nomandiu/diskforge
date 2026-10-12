#pragma once

#include "ViewModels.g.h"

namespace winrt::DiskForge::VMs::implementation {

using namespace winrt::Microsoft::UI::Xaml::Media;

struct DiskVM : DiskVMT<DiskVM> {
    DiskVM() = default;

    int32_t Number() const { return m_number; }
    void Number(int32_t v) { m_number = v; }
    hstring Title() const { return m_title; }
    void Title(hstring const& v) { m_title = v; }
    hstring Detail() const { return m_detail; }
    void Detail(hstring const& v) { m_detail = v; }
    hstring SizeText() const { return m_sizeText; }
    void SizeText(hstring const& v) { m_sizeText = v; }
    hstring BusText() const { return m_busText; }
    void BusText(hstring const& v) { m_busText = v; }
    hstring StyleText() const { return m_styleText; }
    void StyleText(hstring const& v) { m_styleText = v; }
    hstring ModelText() const { return m_modelText; }
    void ModelText(hstring const& v) { m_modelText = v; }
    hstring SerialText() const { return m_serialText; }
    void SerialText(hstring const& v) { m_serialText = v; }
    int32_t PartitionCount() const { return m_partitionCount; }
    void PartitionCount(int32_t v) { m_partitionCount = v; }
    bool IsSsd() const { return m_isSsd; }
    void IsSsd(bool v) { m_isSsd = v; }
    bool IsRemovable() const { return m_isRemovable; }
    void IsRemovable(bool v) { m_isRemovable = v; }
    bool IsReadOnly() const { return m_isReadOnly; }
    void IsReadOnly(bool v) { m_isReadOnly = v; }
    bool IsSystemDisk() const { return m_isSystemDisk; }
    void IsSystemDisk(bool v) { m_isSystemDisk = v; }
    winrt::Microsoft::UI::Xaml::Visibility SystemVis() const {
        return m_isSystemDisk ? winrt::Microsoft::UI::Xaml::Visibility::Visible
                              : winrt::Microsoft::UI::Xaml::Visibility::Collapsed;
    }

private:
    int32_t m_number{ -1 };
    hstring m_title, m_detail, m_sizeText, m_busText, m_styleText, m_modelText, m_serialText;
    int32_t m_partitionCount{ 0 };
    bool m_isSsd{ false }, m_isRemovable{ false }, m_isReadOnly{ false }, m_isSystemDisk{ false };
};

struct PartitionVM : PartitionVMT<PartitionVM> {
    PartitionVM() = default;

    hstring Title() const { return m_title; }
    void Title(hstring const& v) { m_title = v; }
    hstring Subtitle() const { return m_subtitle; }
    void Subtitle(hstring const& v) { m_subtitle = v; }
    hstring SizeText() const { return m_sizeText; }
    void SizeText(hstring const& v) { m_sizeText = v; }
    hstring UsedText() const { return m_usedText; }
    void UsedText(hstring const& v) { m_usedText = v; }
    hstring FsText() const { return m_fsText; }
    void FsText(hstring const& v) { m_fsText = v; }
    hstring KindText() const { return m_kindText; }
    void KindText(hstring const& v) { m_kindText = v; }
    hstring LetterText() const { return m_letterText; }
    void LetterText(hstring const& v) { m_letterText = v; }
    hstring StartText() const { return m_startText; }
    void StartText(hstring const& v) { m_startText = v; }
    double UsedFraction() const { return m_usedFraction; }
    void UsedFraction(double v) { m_usedFraction = v; }
    Brush FillBrush() const { return m_fillBrush; }
    void FillBrush(Brush const& v) { m_fillBrush = v; }
    bool IsUnallocated() const { return m_isUnallocated; }
    void IsUnallocated(bool v) { m_isUnallocated = v; }
    bool HasLetter() const { return m_hasLetter; }
    void HasLetter(bool v) { m_hasLetter = v; }
    bool IsSystem() const { return m_isSystem; }
    void IsSystem(bool v) { m_isSystem = v; }
    bool IsEsp() const { return m_isEsp; }
    void IsEsp(bool v) { m_isEsp = v; }
    int64_t StartLba() const { return m_startLba; }
    void StartLba(int64_t v) { m_startLba = v; }
    int64_t Sectors() const { return m_sectors; }
    void Sectors(int64_t v) { m_sectors = v; }
    winrt::Microsoft::UI::Xaml::Visibility UsedBarVis() const {
        return m_usedFraction > 0.0 ? winrt::Microsoft::UI::Xaml::Visibility::Visible
                                    : winrt::Microsoft::UI::Xaml::Visibility::Collapsed;
    }
    winrt::Microsoft::UI::Xaml::Visibility LetterVis() const {
        return m_hasLetter ? winrt::Microsoft::UI::Xaml::Visibility::Visible
                           : winrt::Microsoft::UI::Xaml::Visibility::Collapsed;
    }

private:
    hstring m_title, m_subtitle, m_sizeText, m_usedText, m_fsText, m_kindText, m_letterText, m_startText;
    double m_usedFraction{ 0 };
    Brush m_fillBrush{ nullptr };
    bool m_isUnallocated{ false }, m_hasLetter{ false }, m_isSystem{ false }, m_isEsp{ false };
    int64_t m_startLba{ 0 }, m_sectors{ 0 };
};

struct FileVM : FileVMT<FileVM> {
    FileVM() = default;
    hstring Name() const { return m_name; }
    void Name(hstring const& v) { m_name = v; }
    hstring Path() const { return m_path; }
    void Path(hstring const& v) { m_path = v; }
    hstring SizeText() const { return m_sizeText; }
    void SizeText(hstring const& v) { m_sizeText = v; }
    hstring Category() const { return m_category; }
    void Category(hstring const& v) { m_category = v; }
    hstring TypeName() const { return m_typeName; }
    void TypeName(hstring const& v) { m_typeName = v; }
    bool Deleted() const { return m_deleted; }
    void Deleted(bool v) { m_deleted = v; }
    bool Carved() const { return m_carved; }
    void Carved(bool v) { m_carved = v; }
    bool Orphaned() const { return m_orphaned; }
    void Orphaned(bool v) { m_orphaned = v; }
    bool SizeEstimated() const { return m_sizeEstimated; }
    void SizeEstimated(bool v) { m_sizeEstimated = v; }
    int32_t Index() const { return m_index; }
    void Index(int32_t v) { m_index = v; }
    winrt::Microsoft::UI::Xaml::Visibility DeletedVis() const {
        return m_deleted ? winrt::Microsoft::UI::Xaml::Visibility::Visible
                         : winrt::Microsoft::UI::Xaml::Visibility::Collapsed;
    }
    winrt::Microsoft::UI::Xaml::Visibility BadgeVis() const {
        return (m_deleted || m_carved) ? winrt::Microsoft::UI::Xaml::Visibility::Visible
                                       : winrt::Microsoft::UI::Xaml::Visibility::Collapsed;
    }
    hstring Glyph() const {
        if (m_glyph.empty()) return L"\xE7C3";
        return m_glyph;
    }
    void Glyph(hstring const& v) { m_glyph = v; }

private:
    hstring m_name, m_path, m_sizeText, m_category, m_typeName;
    bool m_deleted{ false }, m_carved{ false }, m_orphaned{ false }, m_sizeEstimated{ false };
    int32_t m_index{ -1 };
    hstring m_glyph;
};

struct OperationVM : OperationVMT<OperationVM> {
    OperationVM() = default;
    hstring Title() const { return m_title; }
    void Title(hstring const& v) { m_title = v; }
    hstring Target() const { return m_target; }
    void Target(hstring const& v) { m_target = v; }
    hstring Status() const { return m_status; }
    void Status(hstring const& v) { m_status = v; }
    hstring Detail() const { return m_detail; }
    void Detail(hstring const& v) { m_detail = v; }
    int32_t State() const { return m_state; }
    void State(int32_t v) { m_state = v; }
    bool IsPending() const { return m_state == 0; }
    bool IsRunning() const { return m_state == 1; }
    bool IsDone() const { return m_state == 2; }
    bool IsFailed() const { return m_state == 3; }
    winrt::Microsoft::UI::Xaml::Visibility RunningVis() const {
        return m_state == 1 ? winrt::Microsoft::UI::Xaml::Visibility::Visible
                            : winrt::Microsoft::UI::Xaml::Visibility::Collapsed;
    }
    winrt::Microsoft::UI::Xaml::Visibility DoneVis() const {
        return m_state == 2 ? winrt::Microsoft::UI::Xaml::Visibility::Visible
                            : winrt::Microsoft::UI::Xaml::Visibility::Collapsed;
    }
    winrt::Microsoft::UI::Xaml::Visibility FailedVis() const {
        return m_state == 3 ? winrt::Microsoft::UI::Xaml::Visibility::Visible
                            : winrt::Microsoft::UI::Xaml::Visibility::Collapsed;
    }

private:
    hstring m_title, m_target, m_status, m_detail;
    int32_t m_state{ 0 };
};

struct LostPartitionVM : LostPartitionVMT<LostPartitionVM> {
    LostPartitionVM() = default;
    hstring PositionText() const { return m_positionText; }
    void PositionText(hstring const& v) { m_positionText = v; }
    hstring SizeText() const { return m_sizeText; }
    void SizeText(hstring const& v) { m_sizeText = v; }
    hstring FsText() const { return m_fsText; }
    void FsText(hstring const& v) { m_fsText = v; }
    hstring LabelText() const { return m_labelText; }
    void LabelText(hstring const& v) { m_labelText = v; }
    hstring EvidenceText() const { return m_evidenceText; }
    void EvidenceText(hstring const& v) { m_evidenceText = v; }
    double Confidence() const { return m_confidence; }
    void Confidence(double v) { m_confidence = v; }
    hstring ConfidenceText() const { return m_confidenceText; }
    void ConfidenceText(hstring const& v) { m_confidenceText = v; }
    bool Overlaps() const { return m_overlaps; }
    void Overlaps(bool v) { m_overlaps = v; }
    int64_t StartLba() const { return m_startLba; }
    void StartLba(int64_t v) { m_startLba = v; }
    int64_t Sectors() const { return m_sectors; }
    void Sectors(int64_t v) { m_sectors = v; }
    winrt::Microsoft::UI::Xaml::Visibility OverlapVis() const {
        return m_overlaps ? winrt::Microsoft::UI::Xaml::Visibility::Visible
                          : winrt::Microsoft::UI::Xaml::Visibility::Collapsed;
    }

private:
    hstring m_positionText, m_sizeText, m_fsText, m_labelText, m_evidenceText, m_confidenceText;
    double m_confidence{ 0 };
    bool m_overlaps{ false };
    int64_t m_startLba{ 0 }, m_sectors{ 0 };
};

struct LogEntryVM : LogEntryVMT<LogEntryVM> {
    LogEntryVM() = default;
    hstring TimeText() const { return m_timeText; }
    void TimeText(hstring const& v) { m_timeText = v; }
    hstring LevelText() const { return m_levelText; }
    void LevelText(hstring const& v) { m_levelText = v; }
    hstring Message() const { return m_message; }
    void Message(hstring const& v) { m_message = v; }
    int32_t Level() const { return m_level; }
    void Level(int32_t v) { m_level = v; }

private:
    hstring m_timeText, m_levelText, m_message;
    int32_t m_level{ 0 };
};

}  // namespace winrt::DiskForge::VMs::implementation

namespace winrt::DiskForge::VMs::factory_implementation {
struct DiskVM : DiskVMT<DiskVM, implementation::DiskVM> {};
struct PartitionVM : PartitionVMT<PartitionVM, implementation::PartitionVM> {};
struct FileVM : FileVMT<FileVM, implementation::FileVM> {};
struct OperationVM : OperationVMT<OperationVM, implementation::OperationVM> {};
struct LostPartitionVM : LostPartitionVMT<LostPartitionVM, implementation::LostPartitionVM> {};
struct LogEntryVM : LogEntryVMT<LogEntryVM, implementation::LogEntryVM> {};
}  // namespace winrt::DiskForge::VMs::factory_implementation
