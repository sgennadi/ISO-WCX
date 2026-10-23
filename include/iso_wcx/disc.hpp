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
    std::filesystem::path direct_source; // Validated raw sidecar, e.g. CloneCD .sub
    std::vector<Extent> extents;
    std::vector<std::uint8_t> inline_data;
};
class Disc {
public:
    explicit Disc(std::filesystem::path image);
    [[nodiscard]] const std::vector<Item>& items() const noexcept { return entries_; }
    [[nodiscard]] const std::string& format() const noexcept { return format_; }
    // Returns false when cancelled. Throws on damaged source or I/O error.
    bool copy(std::size_t index, const std::filesystem::path& output,
              const std::function<bool(std::uint64_t)>& progress = {}, bool test_only = false) const;
private:
    std::filesystem::path image_;
    std::vector<Item> entries_;
    std::string format_;
    std::shared_ptr<VirtualArchive> virtual_archive_;
};
}
