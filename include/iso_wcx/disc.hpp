#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace iso_wcx {
class VirtualArchive;
class WimArchive;
class ChdStream;
struct Extent {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    bool sparse = false;
};
struct Item {
    std::string path; // UTF-8, always relative and separated by '/'
    std::uint64_t size = 0;
    bool directory = false;
    std::uint32_t dos_time = 0; // Packed DOS local date/time exposed by WCX.
    std::uint32_t attributes = 0; // Windows FILE_ATTRIBUTE_* mask.
    std::uint32_t source_index = 0xffffffffu; // Descriptor track for multisession views.
    std::uint32_t chd_audio_track = 0; // 1..99: synthesized Red Book WAV from a CHD CDDA track.
    std::filesystem::path direct_source; // Validated raw sidecar, e.g. CloneCD .sub
    std::vector<Extent> extents;
    std::vector<std::uint8_t> inline_data;
};
// Metadata-only ISO namespace view. Used to compare the ISO9660 primary
// namespace against UDF/Joliet in hybrid optical images. No file payloads
// are extracted and no image writes are performed.
struct IsoNamespaceView {
    std::string format;
    std::vector<Item> items;
};
[[nodiscard]] IsoNamespaceView inspect_iso_namespace(
    const std::filesystem::path& image, bool primary_only = true);

class Disc {
public:
    // Optional metadata progress is used for the WCX-owned opening dialog.
    // It is invoked while WIMGAPI enumerates WIM indexes, before Total
    // Commander can register SetProcessDataProc on the archive handle.
    using WimScanProgress = std::function<void(std::uint32_t,std::size_t)>;
    explicit Disc(std::filesystem::path image,
                  WimScanProgress wim_scan_progress = {});
    // WIM metadata is owned by WimArchive. Return the same immutable vector
    // instead of copying hundreds of thousands of Item strings into Disc.
    [[nodiscard]] const std::vector<Item>& items() const noexcept;
    [[nodiscard]] const std::string& format() const noexcept { return format_; }
    // Returns false when cancelled. Throws on damaged source or I/O error.
    bool copy(std::size_t index, const std::filesystem::path& output,
              const std::function<bool(std::uint64_t)>& progress = {}, bool test_only = false) const;
private:
    std::filesystem::path image_;
    std::vector<Item> entries_;
    std::string format_;
    std::shared_ptr<VirtualArchive> virtual_archive_;
    std::shared_ptr<WimArchive> wim_archive_;
    std::shared_ptr<ChdStream> chd_audio_;
};
}
