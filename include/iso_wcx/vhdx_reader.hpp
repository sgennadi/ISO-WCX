#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace iso_wcx {

// Standalone native C++20, strictly read-only VHDX v1 logical block reader.
// Neither VirtDisk.dll nor an external parser is loaded or linked.
// Only clean fixed/dynamic base images are accepted.  Differencing, dirty
// transaction logs and sector-bitmap-dependent partial blocks fail closed.
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
    std::uint64_t chunk_ratio_ = 0;
    std::uint32_t block_size_ = 0;
    std::uint32_t sector_size_ = 0;
};

} // namespace iso_wcx
