#pragma once
// Native, strict VHD v1 dynamic/sparse reader. No VirtDisk mount, libvhdi,
// Go/Rust/.NET runtime, helper EXE, or external parser DLL.
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

namespace iso_wcx {
class DynamicVhdReader {
public:
    DynamicVhdReader(const std::filesystem::path& image,
                     std::uint64_t capacity,
                     std::uint64_t physical_size,
                     std::uint64_t dynamic_header_offset,
                     const std::array<std::uint8_t,512>& footer);
    DynamicVhdReader(const DynamicVhdReader&) = delete;
    DynamicVhdReader& operator=(const DynamicVhdReader&) = delete;
    [[nodiscard]] std::vector<std::uint8_t> read(std::uint64_t offset,
                                                  std::size_t length);
private:
    void read_file(std::uint64_t physical,std::uint8_t* dest,std::size_t count);
    void load_bitmap(std::size_t block_index);

    std::ifstream input_;
    std::uint64_t capacity_ = 0;
    std::uint64_t physical_size_ = 0;
    std::uint32_t block_size_ = 0;
    std::uint32_t bitmap_bytes_ = 0;
    std::vector<std::uint32_t> bat_;
    std::vector<std::uint8_t> cached_bitmap_;
    std::size_t cached_block_ = std::numeric_limits<std::size_t>::max();
};
} // namespace iso_wcx
