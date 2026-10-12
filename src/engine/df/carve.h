// Signature-based file carving (JPEG, PNG, PDF, ZIP/Office, MP4, MP3, ...)
#pragma once

#include <set>
#include <vector>

#include "common.h"
#include "diskio.h"
#include "units.h"

namespace df {

enum class CarveType {
    Jpeg, Png, Gif, Bmp, Pdf, Zip, Rar, SevenZ, Mp4, Mp3, Wav, Avi, Ole, Txt
};

struct CarvedFile {
    CarveType type = CarveType::Jpeg;
    std::string typeName;
    std::string ext;
    uint64_t startLba = 0;
    uint64_t sizeBytes = 0;
    bool sizeExact = false;     // derived from header/footer, not a cap heuristic
};

struct CarveOptions {
    std::set<CarveType> wanted;             // empty = all types
    uint64_t maxFileSize = 2ull * kGiB;     // safety cap
    // category helpers used by the UI
    static std::set<CarveType> All() {
        return {CarveType::Jpeg, CarveType::Png, CarveType::Gif, CarveType::Bmp, CarveType::Pdf,
                CarveType::Zip, CarveType::Rar, CarveType::SevenZ, CarveType::Mp4, CarveType::Mp3,
                CarveType::Wav, CarveType::Avi, CarveType::Ole, CarveType::Txt};
    }
    static std::set<CarveType> Documents() { return {CarveType::Pdf, CarveType::Zip, CarveType::Ole, CarveType::Txt, CarveType::Rar, CarveType::SevenZ}; }
    static std::set<CarveType> Photos() { return {CarveType::Jpeg, CarveType::Png, CarveType::Gif, CarveType::Bmp}; }
    static std::set<CarveType> Videos() { return {CarveType::Mp4, CarveType::Avi}; }
    static std::set<CarveType> Audio() { return {CarveType::Mp3, CarveType::Wav}; }
    static std::set<CarveType> Archives() { return {CarveType::Zip, CarveType::Rar, CarveType::SevenZ}; }
};

const char* CarveTypeName(CarveType t);
const char* CarveTypeExt(CarveType t);

// Scan [start, start+sectors) for file signatures; recovered ranges never overlap.
std::vector<CarvedFile> CarveRegion(IDiskDevice& dev, Lba start, uint64_t sectors,
                                    const CarveOptions& opt, const Progress& prog);

}  // namespace df
