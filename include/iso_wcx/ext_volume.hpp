#pragma once
#include "iso_wcx/volume_reader.hpp"
#include "iso_wcx/virtual_disk.hpp"
#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace iso_wcx {
// Preview read-only ext2/3/4: inodes, directories, indirect block pointers
// and ext4 extent trees. No mount, driver, helper EXE or journal replay.
class ExtVolume final : public VolumeReader {
public:
    ExtVolume(VirtualDiskReader& disk,std::uint64_t start,std::uint64_t length);
    std::vector<VolumeFile> list() override;
    void extract(std::string_view path,std::ostream& output) override;
private:
    struct Inode {
        std::uint16_t mode{};
        std::uint64_t size{};
        std::uint32_t flags{};
        std::array<std::uint8_t,60> mapping{};
    };
    struct Extent {
        std::uint64_t logical{},blocks{},physical{};
        bool uninitialized{};
    };
    struct FileNode {VolumeFile info;std::uint32_t inode{};};
    std::vector<std::uint8_t> read(std::uint64_t at,std::size_t n);
    std::vector<std::uint8_t> read_block(std::uint64_t n);
    Inode read_inode(std::uint32_t number);
    void extent_tree(const std::uint8_t* node,std::size_t length,
                     unsigned expected_depth,std::vector<Extent>& result,
                     std::set<std::uint64_t>& visited);
    std::vector<Extent> extents(const Inode& inode);
    std::uint64_t mapped_block(const Inode& inode,std::uint64_t index,
                               const std::vector<Extent>& tree);
    std::uint64_t indirect(std::uint64_t block,std::uint64_t index,unsigned depth);
    std::vector<std::uint8_t> directory(const Inode& inode);
    void walk(std::uint32_t number,const std::string& prefix,unsigned depth);
    void scan_once();
    static void safe_name(std::string_view s);
    VirtualDiskReader& disk_;
    std::uint64_t start_{},length_{},blocks_{};
    std::uint32_t block_size_{},first_data_{},bpg_{},ipg_{},inode_count_{};
    std::uint32_t desc_size_{},inode_size_{},groups_{};
    std::uint64_t descriptor_offset_{};
    bool feature64_{};
    bool scanned_{};
    std::vector<VolumeFile> files_;
    std::map<std::string,FileNode> nodes_;
    std::set<std::uint32_t> visited_inodes_;
};
} // namespace iso_wcx
