#include "iso_wcx/virtual_archive.hpp"
#include "iso_wcx/partition_map.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <streambuf>
#include <string_view>
#include <utility>

namespace iso_wcx {
namespace {
void require(bool valid, const char* message) {
    if (!valid) throw std::runtime_error(message);
}

class ProgressBuffer final : public std::streambuf {
public:
    explicit ProgressBuffer(
        const std::function<bool(const std::uint8_t*, std::size_t)>& accept)
        : accept_(accept) {}
    bool cancelled() const noexcept { return cancelled_; }
protected:
    std::streamsize xsputn(const char* bytes, std::streamsize count) override {
        if (count <= 0 || cancelled_) return 0;
        if (!accept_(reinterpret_cast<const std::uint8_t*>(bytes),
                     static_cast<std::size_t>(count))) {
            cancelled_ = true;
            return 0;
        }
        return count;
    }
    int_type overflow(int_type ch) override {
        if (traits_type::eq_int_type(ch, traits_type::eof()))
            return traits_type::not_eof(ch);
        const char byte = traits_type::to_char_type(ch);
        return xsputn(&byte, 1) == 1 ? ch : traits_type::eof();
    }
private:
    const std::function<bool(const std::uint8_t*, std::size_t)>& accept_;
    bool cancelled_ = false;
};

void check_path(std::string_view path) {
    require(!path.empty() && path.size() <= 2048, "invalid FAT path length");
    std::size_t start = 0;
    while (start < path.size()) {
        const auto slash = path.find('/', start);
        const auto end = slash == std::string_view::npos ? path.size() : slash;
        const auto name = path.substr(start, end - start);
        require(!name.empty() && name != "." && name != ".." &&
                name.back() != '.' && name.back() != ' ',
                "unsafe FAT filename");
        auto base = std::string(name.substr(0, name.find('.')));
        std::transform(base.begin(), base.end(), base.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        require(base != "CON" && base != "PRN" && base != "AUX" &&
                base != "NUL" && base != "CLOCK$",
                "reserved Windows FAT filename");
        if (base.size() == 4 &&
            (base.substr(0, 3) == "COM" || base.substr(0, 3) == "LPT"))
            require(base[3] < '1' || base[3] > '9',
                    "reserved Windows device filename");
        if (slash == std::string_view::npos) break;
        start = slash + 1;
        require(start < path.size(), "trailing FAT path separator");
    }
}
const char* disk_name(VirtualDiskFormat type) {
    switch (type) {
    case VirtualDiskFormat::raw_image: return "Raw HDD";
    case VirtualDiskFormat::fixed_vhd: return "Fixed VHD";
    case VirtualDiskFormat::vhdx: return "VHDX";
    }
    return "Virtual HDD";
}
} // namespace

VirtualArchive::VirtualArchive(const std::filesystem::path& image)
    : disk_(image) {
    const auto sector = disk_.logical_sector_size();
    const auto partitions = inspect_partition_table(disk_, sector);
    require(!partitions.empty(), "virtual disk has no partition table");
    format_ = std::string(disk_name(disk_.format())) + " / FAT12/16/32";
    constexpr std::size_t max_items = 100000;
    for (std::size_t i = 0; i < partitions.size(); ++i) {
        const auto& partition = partitions[i];
        require(partition.first_lba <= disk_.size() / sector &&
                partition.sector_count <= disk_.size() / sector - partition.first_lba,
                "partition outside virtual disk");
        std::unique_ptr<FatVolume> volume;
        std::vector<FatFile> files;
        try {
            volume = std::make_unique<FatVolume>(
                disk_, partition.first_lba * sector,
                partition.sector_count * sector);
            files = volume->list();
        } catch (const std::exception&) {
            // Non-FAT partitions are not listed as though they were FAT.
            continue;
        }
        for (const auto& file : files) check_path(file.path);
        require(files.size() < max_items && items_.size() < max_items - files.size(),
                "too many FAT directory entries");
        const auto volume_index = volumes_.size();
        const auto prefix = "partition" + std::to_string(i + 1);
        Item folder;
        folder.path = prefix;
        folder.directory = true;
        folder.attributes = 0x10;
        items_.push_back(std::move(folder));
        files_.push_back({});
        for (const auto& entry : files) {
            Item item;
            item.path = prefix + "/" + entry.path;
            item.directory = entry.directory;
            item.size = entry.directory ? 0 : entry.size;
            item.attributes = entry.directory ? 0x10u : 0x20u;
            items_.push_back(std::move(item));
            files_.push_back({volume_index, entry.path});
        }
        volumes_.push_back(std::move(volume));
    }
    require(!volumes_.empty(), "no FAT12/16/32 partition in virtual disk");
}

bool VirtualArchive::read(
    std::size_t index,
    const std::function<bool(const std::uint8_t*, std::size_t)>& accept) {
    require(index < items_.size() && !items_[index].directory,
            "invalid virtual archive item index");
    const auto& loc = files_[index];
    require(loc.volume_index < volumes_.size(), "invalid FAT partition index");
    std::lock_guard<std::mutex> guard(read_mutex_);
    ProgressBuffer buffer(accept);
    std::ostream stream(&buffer);
    try {
        volumes_[loc.volume_index]->extract(loc.relative_path, stream);
    } catch (const std::exception&) {
        if (buffer.cancelled()) return false;
        throw;
    }
    return !buffer.cancelled();
}

} // namespace iso_wcx
