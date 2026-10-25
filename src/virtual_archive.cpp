#include "iso_wcx/virtual_archive.hpp"
#include "iso_wcx/partition_map.hpp"
#include "iso_wcx/fat_volume.hpp"
#include "iso_wcx/exfat_volume.hpp"
#include "iso_wcx/ext_volume.hpp"
#include "iso_wcx/ntfs_volume.hpp"

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
    require(!path.empty() && path.size() <= 1000, "invalid virtual volume path length");
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
    case VirtualDiskFormat::compressed_floppy: return "IMZ compressed floppy";
    case VirtualDiskFormat::fixed_vhd: return "Fixed VHD";
    case VirtualDiskFormat::dynamic_vhd: return "Dynamic VHD";
    case VirtualDiskFormat::vhdx: return "VHDX";
    }
    return "Virtual HDD";
}
} // namespace

VirtualArchive::VirtualArchive(const std::filesystem::path& image)
    : disk_(image) {
    const auto sector = disk_.logical_sector_size();
    // A raw floppy (superfloppy) begins with the FAT boot sector at byte zero:
    // it has no MBR or GPT. The normal partition parser must still take
    // priority for partitioned IMG/RAW and VHD/VHDX containers.
    std::vector<PartitionEntry> partitions;
    try {
        partitions = inspect_partition_table(disk_, sector);
    } catch (const std::exception&) {
        // Only independently validated FAT at byte zero may be used below.
    }
    constexpr std::size_t max_items = 100000;
    if (partitions.empty()) {
        require(disk_.format() == VirtualDiskFormat::raw_image ||
                disk_.format() == VirtualDiskFormat::compressed_floppy,
                "virtual disk has no valid partition table");
        auto volume = std::make_unique<FatVolume>(disk_, 0, disk_.size());
        auto files = volume->list();
        require(files.size() <= max_items,
                "too many FAT superfloppy directory entries");
        for (const auto& file : files) {
            check_path(file.path);
            Item item;
            item.path = file.path;
            item.directory = file.directory;
            item.size = file.directory ? 0 : file.size;
            item.dos_time = file.dos_time;
            item.attributes = file.attributes != 0 ? file.attributes :
                              (file.directory ? 0x10u : 0x20u);
            items_.push_back(std::move(item));
            files_.push_back({0, file.path});
        }
        const auto prefix = disk_.format() == VirtualDiskFormat::compressed_floppy ?
                                    "IMZ / " : "Raw ";
        format_ = std::string(prefix) +
                  (volume->kind() == FatKind::fat12 ? "FAT12 floppy" :
                   volume->kind() == FatKind::fat16 ? "FAT16 superfloppy" :
                                                     "FAT32 superfloppy");
        volumes_.push_back(std::move(volume));
        return;
    }
    format_ = std::string(disk_name(disk_.format())) + " / FAT12/16/32/exFAT/ext2/3/4/NTFS";
    for (std::size_t i = 0; i < partitions.size(); ++i) {
        const auto& partition = partitions[i];
        require(partition.first_lba <= disk_.size() / sector &&
                partition.sector_count <= disk_.size() / sector - partition.first_lba,
                "partition outside virtual disk");
        std::unique_ptr<VolumeReader> volume;
        std::vector<VolumeFile> files;
        const auto offset = partition.first_lba * sector;
        const auto length = partition.sector_count * sector;
        try {
            volume = std::make_unique<FatVolume>(disk_,offset,length);
            files = volume->list();
        } catch (const std::exception&) {
            // exFAT is independent of FAT12/16/32 and has a different BPB.
            try {
                volume = std::make_unique<ExfatVolume>(disk_,offset,length);
                files = volume->list();
            } catch (const std::exception&) {
                try {
                    volume=std::make_unique<ExtVolume>(disk_,offset,length);
                    files=volume->list();
                } catch(const std::exception&) {
                    try {
                        volume=std::make_unique<NtfsVolume>(disk_,offset,length);
                        files=volume->list();
                    } catch (const std::exception&) {
                        // Unknown, corrupt and unsupported volumes are not guessed.
                        continue;
                    }
                }
            }
        }
        for (const auto& file : files) check_path(file.path);
        require(files.size() < max_items && items_.size() < max_items - files.size(),
                "too many virtual volume directory entries");
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
            item.dos_time = entry.dos_time;
            item.attributes = entry.attributes != 0 ? entry.attributes :
                              (entry.directory ? 0x10u : 0x20u);
            items_.push_back(std::move(item));
            files_.push_back({volume_index, entry.path});
        }
        volumes_.push_back(std::move(volume));
    }
    require(!volumes_.empty(), "no supported FAT/exFAT/ext/NTFS partition in virtual disk");
}

bool VirtualArchive::read(
    std::size_t index,
    const std::function<bool(const std::uint8_t*, std::size_t)>& accept) {
    require(index < items_.size() && !items_[index].directory,
            "invalid virtual archive item index");
    const auto& loc = files_[index];
    require(loc.volume_index < volumes_.size(), "invalid virtual volume partition index");
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
