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

// Read-only NTFS preview. Interprets on-disk MFT and $I30 directory indexes;
// never mounts volumes or calls Windows filesystem drivers.
class NtfsVolume final : public VolumeReader {
public:
    NtfsVolume(VirtualDiskReader& disk, std::uint64_t partition_start,
               std::uint64_t partition_length);
    [[nodiscard]] std::vector<VolumeFile> list() override;
    void extract(std::string_view path, std::ostream& output) override;

private:
    struct Run {
        std::uint64_t first_vcn{}, clusters{}, lcn{};
        bool sparse{};
    };
    struct Stream {
        std::vector<std::uint8_t> resident;
        std::vector<Run> runs;
        std::uint64_t length{}, initialized{};
        bool nonresident{};
    };
    struct Attribute {
        std::uint32_t type{};
        std::uint16_t flags{};
        std::u16string name;
        Stream data;
    };
    struct Record {
        std::uint16_t sequence{}, flags{};
        std::vector<Attribute> attributes;
    };
    struct FileNode {
        VolumeFile info;
        std::uint64_t file_number{};
        std::uint16_t sequence{};
    };
    std::vector<std::uint8_t> read(std::uint64_t at, std::size_t size);
    void fixup(std::vector<std::uint8_t>& record, const char* signature);
    Record parse_record(std::vector<std::uint8_t> bytes);
    Record file_record(std::uint64_t file_number);
    Stream parse_nonresident(const std::uint8_t* bytes, std::size_t length,
                             std::uint16_t attr_flags);
    std::vector<std::uint8_t> stream_read(const Stream& stream,
                                          std::uint64_t offset,
                                          std::size_t size);
    static const Attribute* attribute(const Record& record,
                                     std::uint32_t type,
                                     std::u16string_view name={});
    void directory_entries(const Record& record,
                           const std::function<void(std::uint64_t,
                                                   std::uint16_t,
                                                   const std::string&,
                                                   bool)>& receive);
    void walk(std::uint64_t file_number, std::uint16_t sequence,
              const std::string& prefix, unsigned depth);
    void scan_once();
    static std::string utf16_name(const std::uint8_t* bytes,
                                  std::size_t code_units);
    static void safe_name(std::string_view value);
    VirtualDiskReader& disk_;
    std::uint64_t start_{}, length_{}, volume_bytes_{};
    std::uint32_t sector_bytes_{}, cluster_bytes_{}, record_bytes_{};
    std::uint64_t mft_lcn_{};
    Stream mft_;
    bool scanned_{};
    std::map<std::string, FileNode> files_;
    std::set<std::uint64_t> visited_directories_;
};
} // namespace iso_wcx
