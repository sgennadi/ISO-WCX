#pragma once
#include "iso_wcx/volume_reader.hpp"
#include "iso_wcx/virtual_disk.hpp"
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace iso_wcx {
// Native, read-only exFAT 1.0 filesystem interpreter for partitioned
// IMG/VHD/VHDX. Rejects TexFAT, invalid checksums and unsafe paths.
class ExfatVolume final : public VolumeReader {
public:
    ExfatVolume(VirtualDiskReader& disk, std::uint64_t partition_offset,
                std::uint64_t partition_length);
    std::vector<VolumeFile> list() override;
    void extract(std::string_view path, std::ostream& output) override;
private:
    struct Node {
        VolumeFile info;
        std::uint64_t length = 0;
        std::uint64_t valid_length = 0;
        std::uint32_t cluster = 0;
        bool contiguous = false;
    };
    std::vector<std::uint8_t> read(std::uint64_t offset, std::size_t bytes);
    std::uint64_t offset_of(std::uint32_t cluster) const;
    std::uint32_t fat_next(std::uint32_t cluster);
    std::vector<std::uint32_t> cluster_chain(std::uint32_t first,
        std::uint64_t length, bool contiguous, bool until_end=false);
    std::vector<std::uint8_t> directory_bytes(std::uint32_t first,
        std::uint64_t bytes, bool contiguous, bool root);
    void parse_directory(const std::string& prefix, std::uint32_t cluster,
        std::uint64_t length, bool contiguous, unsigned depth, bool root=false);
    void scan_once();
    void verify_allocated(std::uint32_t cluster);
    static std::string filename(const std::vector<std::uint16_t>& units);
    static void safe_component(std::string_view name);
    static std::uint32_t dos_datetime(std::uint16_t date, std::uint16_t time);

    VirtualDiskReader& disk_;
    std::uint64_t start_, length_, volume_bytes_ = 0;
    std::uint64_t fat_start_ = 0, fat_bytes_ = 0, heap_start_ = 0;
    std::uint32_t sector_bytes_ = 0, cluster_bytes_ = 0;
    std::uint32_t clusters_ = 0, root_cluster_ = 0;
    bool scanned_ = false;
    std::uint32_t bitmap_cluster_ = 0;
    std::uint64_t bitmap_length_ = 0;
    std::vector<std::uint32_t> bitmap_chain_;
    std::vector<VolumeFile> files_;
    std::map<std::string, Node> nodes_;
    std::set<std::uint32_t> visited_dirs_;
};
} // namespace iso_wcx
