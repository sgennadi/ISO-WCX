#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>

namespace iso_wcx {

// Standalone native C++20, strictly read-only VHDX v1 logical block reader.
// Neither VirtDisk.dll nor an external parser is loaded or linked.
// Only clean fixed/dynamic base images are accepted.  Differencing, dirty
// transaction logs and sector-bitmap-dependent partial blocks fail closed.
// Read-only validated physical mapping used by the separate atomic FAT writer.
// Every logical payload block is fully present and has a unique non-overlapping
// physical owner. No caller can mutate the reader's BAT or metadata.
struct VhdxFixedEditLayout {
    std::uint32_t block_bytes = 0;
    std::uint64_t capacity = 0;
    std::uint64_t active_sequence = 0;
    std::uint64_t active_header_offset = 0;
    std::vector<std::uint64_t> physical_blocks;
};

class VhdxReader {
public:
    explicit VhdxReader(const std::filesystem::path& file);
    VhdxReader(const VhdxReader&) = delete;
    VhdxReader& operator=(const VhdxReader&) = delete;

    [[nodiscard]] std::uint64_t size() const noexcept { return logical_size_; }
    [[nodiscard]] std::uint32_t logical_sector_size() const noexcept {
        return sector_size_;
    }
    [[nodiscard]] std::vector<std::uint8_t> read(std::uint64_t offset,
                                                  std::size_t length);
    // Strict *fixed* VHDX writer preflight; reject dirty, 4K logical,
    // unallocated/partial/aliased BAT blocks and non-fixed metadata.
    // Enforces the caller's logical and physical safety budgets.
    [[nodiscard]] VhdxFixedEditLayout fully_mapped_fat_edit_layout(
        std::uint64_t maximum_logical_bytes,
        std::uint64_t maximum_physical_bytes);

private:
    [[nodiscard]] std::vector<std::uint8_t> file_read(
        std::uint64_t offset, std::size_t length);
    void parse();

    std::ifstream file_;
    std::uint64_t file_size_ = 0;
    std::uint64_t logical_size_ = 0;
    std::uint64_t bat_offset_ = 0;
    std::uint64_t bat_length_ = 0;
    std::uint64_t metadata_offset_ = 0;
    std::uint64_t metadata_length_ = 0;
    struct OptionalRegion {
        std::uint64_t offset{};
        std::uint64_t size{};
    };
    std::vector<OptionalRegion> optional_regions_;
    // Lazy physical owner map: reject overlapping live BAT payload extents
    // without scanning enormous sparse VHDX BAT tables during opening.
    std::map<std::uint64_t, std::uint64_t> visited_payload_blocks_;
    std::uint64_t log_offset_ = 0;
    std::uint64_t log_length_ = 0;
    std::uint64_t chunk_ratio_ = 0;
    std::uint32_t block_size_ = 0;
    std::uint32_t sector_size_ = 0;
    bool paired_headers_valid_ = false;
    bool paired_region_tables_valid_ = false;
    bool fixed_file_parameters_ = false;
    std::uint64_t active_header_sequence_ = 0;
    std::uint64_t active_header_offset_ = 0;
};

} // namespace iso_wcx
