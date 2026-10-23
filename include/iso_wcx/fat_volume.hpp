#pragma once

#include "iso_wcx/virtual_disk.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace iso_wcx {

// Read-only FAT12/16/32 implementation for virtual HDD partitions.
// No filesystem driver, host mounting, physical-drive access, writes or
// implicit extraction destinations are involved.
enum class FatKind : std::uint8_t { fat12, fat16, fat32 };

struct FatFile {
    std::string path;          // validated UTF-8, relative to partition root
    std::uint64_t size = 0;
    std::uint32_t first_cluster = 0;
    bool directory = false;
};

class FatVolume {
public:
    // Offset and extent are logical container bytes, not physical file bytes.
    FatVolume(VirtualDiskReader& disk, std::uint64_t partition_offset,
              std::uint64_t partition_length);
    FatVolume(const FatVolume&) = delete;
    FatVolume& operator=(const FatVolume&) = delete;

    [[nodiscard]] FatKind kind() const noexcept { return kind_; }
    [[nodiscard]] std::uint64_t size() const noexcept { return volume_size_; }

    // Validates directory trees, FAT chains reached during discovery,
    // untrusted VFAT names, recursion depth and total number of items.
    [[nodiscard]] std::vector<FatFile> list();

    // Only an already-listed relative path can be extracted.
    // Streams binary bytes to the caller; this function never opens paths.
    // A cyclic, short, bad or excessive cluster chain fails with an error.
    void extract(std::string_view path, std::ostream& output);

private:
    [[nodiscard]] std::vector<std::uint8_t> volume_read(std::uint64_t offset,
                                                         std::size_t length);
    [[nodiscard]] std::uint32_t next_cluster(std::uint32_t cluster);
    [[nodiscard]] std::uint64_t cluster_offset(std::uint32_t cluster) const;
    [[nodiscard]] std::vector<std::uint8_t> directory_chain(std::uint32_t first);
    void walk_directory(std::string_view prefix, std::uint32_t first,
                        bool fixed_root, unsigned depth);
    void scan_directory_bytes(std::string_view prefix,
                              const std::vector<std::uint8_t>& bytes,
                              unsigned depth);
    void validate_entry_name(std::string_view name) const;
    void scan_once();

    VirtualDiskReader& disk_;
    std::uint64_t partition_offset_ = 0;
    std::uint64_t partition_length_ = 0;
    std::uint64_t volume_size_ = 0;
    std::uint64_t fat_offset_ = 0;
    std::uint64_t fat_length_ = 0;
    std::uint64_t root_offset_ = 0;
    std::uint64_t data_offset_ = 0;
    std::uint32_t total_clusters_ = 0;
    std::uint32_t root_cluster_ = 0;
    std::uint32_t root_length_ = 0;
    std::uint32_t bytes_per_sector_ = 0;
    std::uint32_t cluster_bytes_ = 0;
    FatKind kind_ = FatKind::fat12;
    bool scanned_ = false;
    std::vector<FatFile> entries_;
    std::unordered_set<std::string> seen_paths_;
    std::unordered_set<std::uint32_t> visited_directories_;
};

} // namespace iso_wcx
