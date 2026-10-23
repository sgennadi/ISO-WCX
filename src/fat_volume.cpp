#include "iso_wcx/fat_volume.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace iso_wcx {
namespace {

constexpr std::size_t kMaxDirectoryBytes = 8u * 1024u * 1024u;
constexpr std::size_t kMaxListedEntries = 100000;
constexpr unsigned kMaxDirectoryDepth = 32;

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
std::uint16_t le16(const std::uint8_t* p) noexcept {
    return std::uint16_t(p[0]) | (std::uint16_t(p[1]) << 8);
}
std::uint32_t le32(const std::uint8_t* p) noexcept {
    return std::uint32_t(le16(p)) | (std::uint32_t(le16(p + 2)) << 16);
}
bool pow2(std::uint32_t n) noexcept { return n && !(n & (n - 1)); }
void utf8_append(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) out.push_back(static_cast<char>(cp));
    else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 63)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
        out.push_back(static_cast<char>(0x80 | (cp & 63)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 63)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
        out.push_back(static_cast<char>(0x80 | (cp & 63)));
    }
}
std::optional<std::string> decode_lfn(const std::vector<std::u16string>& parts) {
    if (parts.empty()) return std::nullopt;
    std::u16string units;
    for (const auto& part : parts) units += part;
    std::string utf8;
    bool ended = false;
    for (std::size_t i = 0; i < units.size(); ++i) {
        const auto u = static_cast<std::uint32_t>(units[i]);
        if (ended) {
            if (u != 0xffff && u != 0) return std::nullopt;
            continue;
        }
        if (u == 0 || u == 0xffff) {
            ended = true;
            continue;
        }
        if (u >= 0xd800 && u <= 0xdbff) {
            if (++i == units.size()) return std::nullopt;
            const auto lo = static_cast<std::uint32_t>(units[i]);
            if (lo < 0xdc00 || lo > 0xdfff) return std::nullopt;
            utf8_append(utf8, 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00));
        } else {
            if (u >= 0xdc00 && u <= 0xdfff) return std::nullopt;
            utf8_append(utf8, u);
        }
        if (utf8.size() > 255u * 4u) return std::nullopt;
    }
    if (utf8.empty()) return std::nullopt;
    return utf8;
}
std::u16string read_lfn_fragment(const std::uint8_t* e) {
    std::u16string result;
    result.reserve(13);
    for (const auto start : {1u, 14u, 28u}) {
        const auto count = start == 1 ? 5u : start == 14 ? 6u : 2u;
        for (unsigned i = 0; i < count; ++i)
            result.push_back(static_cast<char16_t>(le16(e + start + 2 * i)));
    }
    return result;
}
std::uint8_t short_checksum(const std::uint8_t* e) {
    std::uint8_t sum = 0;
    for (unsigned i = 0; i < 11; ++i)
        sum = static_cast<std::uint8_t>(((sum & 1) << 7) + (sum >> 1) + e[i]);
    return sum;
}
std::string short_name(const std::uint8_t* e) {
    const char* hex = "0123456789ABCDEF";
    std::string result;
    const auto append = [&](std::string& target, std::uint8_t c, bool lower) {
        if (c >= 0x80 || c == 0x05) {
            // OEM codepage is not reliably inferable from an image. Preserve
            // raw OEM identity instead of decoding it with the host locale.
            target.push_back('~');
            target.push_back(hex[c >> 4]);
            target.push_back(hex[c & 15]);
        } else {
            if (lower && c >= 'A' && c <= 'Z') c = static_cast<std::uint8_t>(c + 32);
            target.push_back(static_cast<char>(c));
        }
    };
    unsigned base = 8, extension = 3;
    while (base && e[base - 1] == ' ') --base;
    while (extension && e[8 + extension - 1] == ' ') --extension;
    for (unsigned i = 0; i < base; ++i) append(result, e[i], (e[12] & 0x08) != 0);
    if (extension) {
        result.push_back('.');
        for (unsigned i = 0; i < extension; ++i)
            append(result, e[8 + i], (e[12] & 0x10) != 0);
    }
    return result;
}
struct LongNameParts {
    std::vector<std::u16string> fragments;
    std::uint8_t next = 0;
    std::uint8_t checksum = 0;
    void reset() {
        fragments.clear();
        next = checksum = 0;
    }
    void accept(const std::uint8_t* e) {
        const auto ordinal = e[0];
        const auto count = static_cast<unsigned>(ordinal & 0x1f);
        if ((ordinal & 0x40) != 0) {
            reset();
            if (count >= 1 && count <= 20 && (ordinal & 0xa0) == 0) {
                fragments.resize(count);
                next = static_cast<std::uint8_t>(count);
                checksum = e[13];
            }
        }
        if (count == 0 || count > fragments.size() ||
            count != next || e[13] != checksum ||
            e[12] != 0 || e[26] != 0 || e[27] != 0 ||
            (ordinal & 0x80) != 0) {
            reset();
            return;
        }
        fragments[count - 1] = read_lfn_fragment(e);
        --next;
    }
    std::optional<std::string> name(const std::uint8_t* short_entry) const {
        if (fragments.empty() || next || checksum != short_checksum(short_entry))
            return std::nullopt;
        return decode_lfn(fragments);
    }
};
} // namespace

FatVolume::FatVolume(VirtualDiskReader& disk, std::uint64_t partition_offset,
                     std::uint64_t partition_length)
    : disk_(disk), partition_offset_(partition_offset),
      partition_length_(partition_length) {
    require(partition_offset_ <= disk_.size() &&
            partition_length_ <= disk_.size() - partition_offset_ &&
            partition_length_ >= 512, "FAT partition is out of disk bounds");
    const auto boot = volume_read(0, 512);
    require(boot[510] == 0x55 && boot[511] == 0xaa,
            "missing FAT boot sector signature");
    require(boot[0] == 0xe9 ||
            (boot[0] == 0xeb && boot[2] == 0x90),
            "invalid FAT boot sector jump");
    bytes_per_sector_ = le16(boot.data() + 11);
    const auto spc = boot[13];
    const auto reserved = le16(boot.data() + 14);
    const auto fats = boot[16];
    const auto root_entries = le16(boot.data() + 17);
    const auto total_sectors = le16(boot.data() + 19) ?
        le16(boot.data() + 19) : le32(boot.data() + 32);
    const auto fat_sectors = le16(boot.data() + 22) ?
        le16(boot.data() + 22) : le32(boot.data() + 36);
    require(bytes_per_sector_ >= 512 && bytes_per_sector_ <= 4096 &&
            pow2(bytes_per_sector_) && pow2(spc) && spc <= 128 &&
            reserved && fats >= 1 && fats <= 2 && fat_sectors &&
            total_sectors, "unsupported FAT BPB geometry");
    cluster_bytes_ = bytes_per_sector_ * spc;
    require(cluster_bytes_ <= 1024 * 1024,
            "FAT cluster exceeds bounded single-read limit");
    volume_size_ = std::uint64_t(total_sectors) * bytes_per_sector_;
    require(volume_size_ <= partition_length_,
            "FAT volume exceeds partition extent");

    const auto root_sectors =
        (std::uint64_t(root_entries) * 32 + bytes_per_sector_ - 1) / bytes_per_sector_;
    const auto system_sectors =
        std::uint64_t(reserved) + std::uint64_t(fats) * fat_sectors + root_sectors;
    require(system_sectors < total_sectors,
            "FAT metadata leaves no data area");
    total_clusters_ = static_cast<std::uint32_t>(
        (total_sectors - system_sectors) / spc);
    require(total_clusters_ >= 1 && total_clusters_ <= 0x0ffffff5,
            "unsupported FAT data-cluster count");

    kind_ = total_clusters_ < 4085 ? FatKind::fat12 :
            total_clusters_ < 65525 ? FatKind::fat16 : FatKind::fat32;
    const auto fat32_bpb = root_entries == 0;
    require((kind_ == FatKind::fat32) == fat32_bpb,
            "FAT type contradicts root directory layout");
    require(kind_ == FatKind::fat32 ?
                (le16(boot.data() + 22) == 0 && le32(boot.data() + 36) != 0) :
                (le16(boot.data() + 22) != 0 && root_entries > 0),
            "FAT16/FAT32 BPB fields do not match FAT type");
    require(boot[21] == 0xf0 || boot[21] >= 0xf8,
            "invalid FAT media byte");
    const auto count_entries = std::uint64_t(total_clusters_) + 2;
    const auto required_bytes = kind_ == FatKind::fat12 ?
        (count_entries * 3 + 1) / 2 :
        count_entries * (kind_ == FatKind::fat16 ? 2u : 4u);
    fat_length_ = std::uint64_t(fat_sectors) * bytes_per_sector_;
    require(required_bytes <= fat_length_,
            "FAT allocation table cannot address its data clusters");

    std::uint32_t selected_fat = 0;
    if (kind_ == FatKind::fat32) {
        const auto flags = le16(boot.data() + 40);
        require(le16(boot.data() + 42) == 0,
                "unsupported FAT32 version");
        if (flags & 0x80) selected_fat = flags & 0xf;
        require(selected_fat < fats,
                "invalid FAT32 active FAT number");
        root_cluster_ = le32(boot.data() + 44) & 0x0fffffff;
        require(root_cluster_ >= 2 && root_cluster_ < total_clusters_ + 2,
                "invalid FAT32 root cluster");
    }
    fat_offset_ = (std::uint64_t(reserved) +
                   std::uint64_t(selected_fat) * fat_sectors) * bytes_per_sector_;
    root_offset_ = (std::uint64_t(reserved) +
                    std::uint64_t(fats) * fat_sectors) * bytes_per_sector_;
    root_length_ = static_cast<std::uint32_t>(root_sectors * bytes_per_sector_);
    data_offset_ = system_sectors * bytes_per_sector_;
    require(fat_offset_ <= volume_size_ &&
            fat_length_ <= volume_size_ - fat_offset_ &&
            root_offset_ <= data_offset_ &&
            data_offset_ <= volume_size_,
            "FAT layout exceeds volume extent");
}

std::vector<std::uint8_t> FatVolume::volume_read(std::uint64_t offset,
                                                   std::size_t length) {
    require(offset <= volume_size_ || volume_size_ == 0,
            "FAT read offset outside volume");
    const auto cap = volume_size_ ? volume_size_ : partition_length_;
    require(offset <= cap && length <= cap - offset,
            "FAT read exceeds partition");
    return disk_.read(partition_offset_ + offset, length);
}

std::uint64_t FatVolume::cluster_offset(std::uint32_t cluster) const {
    require(cluster >= 2 && std::uint64_t(cluster) < std::uint64_t(total_clusters_) + 2,
            "invalid FAT cluster index");
    const auto offset = data_offset_ + std::uint64_t(cluster - 2) * cluster_bytes_;
    require(offset <= volume_size_ && cluster_bytes_ <= volume_size_ - offset,
            "FAT cluster offset exceeds volume");
    return offset;
}

std::uint32_t FatVolume::next_cluster(std::uint32_t cluster) {
    (void)cluster_offset(cluster);
    const auto fat_entry_offset = kind_ == FatKind::fat12 ?
        (std::uint64_t(cluster) * 3) / 2 :
        std::uint64_t(cluster) * (kind_ == FatKind::fat16 ? 2 : 4);
    const auto width = kind_ == FatKind::fat32 ? 4u : 2u;
    require(fat_entry_offset <= fat_length_ &&
            width <= fat_length_ - fat_entry_offset,
            "FAT cluster entry outside FAT allocation table");
    const auto bytes = volume_read(fat_offset_ + fat_entry_offset, width);
    const auto raw = kind_ == FatKind::fat12 ?
        (cluster & 1u ? (le16(bytes.data()) >> 4) : (le16(bytes.data()) & 0xfffu)) :
        kind_ == FatKind::fat16 ? le16(bytes.data()) :
        (le32(bytes.data()) & 0x0fffffffu);
    const auto eoc = kind_ == FatKind::fat12 ? 0xff8u :
                     kind_ == FatKind::fat16 ? 0xfff8u : 0x0ffffff8u;
    if (raw >= eoc) return UINT32_MAX;
    const auto reserved_from = kind_ == FatKind::fat12 ? 0xff0u :
                               kind_ == FatKind::fat16 ? 0xfff0u : 0x0ffffff0u;
    require(raw < reserved_from && raw >= 2 &&
            std::uint64_t(raw) < std::uint64_t(total_clusters_) + 2,
            "FAT chain references bad/free/reserved/out-of-range cluster");
    return raw;
}

std::vector<std::uint8_t> FatVolume::directory_chain(std::uint32_t first) {
    std::vector<std::uint8_t> bytes;
    std::unordered_set<std::uint32_t> visited;
    auto cluster = first;
    for (;;) {
        require(visited.insert(cluster).second,
                "cyclic FAT directory cluster chain");
        require(bytes.size() <= kMaxDirectoryBytes &&
                cluster_bytes_ <= kMaxDirectoryBytes - bytes.size(),
                "FAT directory exceeds 8 MiB safety limit");
        const auto chunk = volume_read(cluster_offset(cluster), cluster_bytes_);
        bytes.insert(bytes.end(), chunk.begin(), chunk.end());
        cluster = next_cluster(cluster);
        if (cluster == UINT32_MAX) return bytes;
    }
}

void FatVolume::validate_entry_name(std::string_view name) const {
    require(!name.empty() && name != "." && name != ".." &&
            name.size() <= 1024 && name.back() != '.' && name.back() != ' ',
            "invalid FAT filename component");
    for (const auto c : name) {
        const auto u = static_cast<unsigned char>(c);
        require(u >= 0x20 && c != '/' && c != '\\' && c != ':' &&
                c != '*' && c != '?' && c != '"' && c != '<' &&
                c != '>' && c != '|',
                "unsafe character in FAT filename component");
    }
}

void FatVolume::scan_directory_bytes(std::string_view prefix,
                                     const std::vector<std::uint8_t>& data,
                                     unsigned depth) {
    require(depth <= kMaxDirectoryDepth,
            "FAT directory nesting exceeds safety limit");
    LongNameParts lfn;
    for (std::size_t i = 0; i + 32 <= data.size(); i += 32) {
        const auto* entry = data.data() + i;
        if (entry[0] == 0x00) break;
        if (entry[0] == 0xe5) { lfn.reset(); continue; }
        const auto attrs = entry[11];
        if (attrs == 0x0f) { lfn.accept(entry); continue; }
        if (attrs & 0xc0) { lfn.reset(); continue; } // reserved DOS attributes
        if (attrs & 0x08) { lfn.reset(); continue; } // volume label
        std::string name = short_name(entry);
        if (const auto long_name = lfn.name(entry))
            name = *long_name;
        lfn.reset();
        if (name == "." || name == "..") continue;
        validate_entry_name(name);
        const auto path = prefix.empty() ? name : std::string(prefix) + "/" + name;
        require(path.size() <= 2048 && entries_.size() < kMaxListedEntries,
                "FAT path or listing exceeds safety limit");

        const bool is_directory = (attrs & 0x10) != 0;
        const auto size = le32(entry + 28);
        const auto first = (std::uint32_t(le16(entry + 20)) << 16) |
                           le16(entry + 26);
        const auto first_cluster = kind_ == FatKind::fat32 ?
            first & 0x0fffffffu : first & 0xffffu;
        require(is_directory ? (size == 0 && first_cluster >= 2) :
                (size == 0 ? first_cluster == 0 : first_cluster >= 2),
                "invalid FAT file/directory entry cluster or size");
        if (first_cluster != 0)
            (void)cluster_offset(first_cluster);
        require(is_directory || size <= volume_size_,
                "FAT file claims more bytes than the volume can contain");
        // No duplicate output paths even if a damaged FAT directory points
        // to the same entity multiple times.
        require(seen_paths_.insert(path).second,
                "duplicate FAT directory path");
        entries_.push_back({path, size, first_cluster, is_directory});
        if (is_directory)
            walk_directory(path, first_cluster, false, depth + 1);
    }
}

void FatVolume::walk_directory(std::string_view prefix, std::uint32_t first,
                               bool fixed_root, unsigned depth) {
    require(depth <= kMaxDirectoryDepth,
            "FAT directory recursion exceeds safety limit");
    std::vector<std::uint8_t> data;
    if (fixed_root) {
        require(root_length_ <= kMaxDirectoryBytes,
                "FAT12/16 root directory exceeds safety limit");
        data = volume_read(root_offset_, root_length_);
    } else {
        require(visited_directories_.insert(first).second,
                "FAT directory cluster referenced multiple times");
        require(visited_directories_.size() <= kMaxListedEntries,
                "too many FAT directories");
        data = directory_chain(first);
    }
    scan_directory_bytes(prefix, data, depth);
}

void FatVolume::scan_once() {
    if (scanned_) return;
    entries_.clear();
    seen_paths_.clear();
    visited_directories_.clear();
    try {
        walk_directory("", root_cluster_, kind_ != FatKind::fat32, 0);
        scanned_ = true;
    } catch (...) {
        entries_.clear();
        visited_directories_.clear();
        throw;
    }
}

std::vector<FatFile> FatVolume::list() {
    scan_once();
    return entries_;
}

void FatVolume::extract(std::string_view path, std::ostream& output) {
    scan_once();
    const auto iter = std::find_if(entries_.begin(), entries_.end(),
        [&](const FatFile& f) { return f.path == path; });
    require(iter != entries_.end(), "FAT file not found in verified directory tree");
    require(!iter->directory, "cannot extract a FAT directory as a regular file");
    auto remaining = iter->size;
    if (!remaining) return;

    std::unordered_set<std::uint32_t> visited;
    const auto needed_clusters = (remaining + cluster_bytes_ - 1) / cluster_bytes_;
    require(needed_clusters <= total_clusters_ && needed_clusters <= 1000000,
            "FAT file chain exceeds available clusters or safety limit");
    auto cluster = iter->first_cluster;
    while (remaining) {
        require(visited.insert(cluster).second, "cyclic FAT file chain");
        const auto n = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, cluster_bytes_));
        const auto data = volume_read(cluster_offset(cluster), n);
        output.write(reinterpret_cast<const char*>(data.data()),
                     static_cast<std::streamsize>(data.size()));
        require(static_cast<bool>(output), "cannot write extracted FAT file");
        remaining -= n;
        if (remaining) {
            cluster = next_cluster(cluster);
            require(cluster != UINT32_MAX, "truncated FAT file chain");
        } else {
            require(next_cluster(cluster) == UINT32_MAX,
                    "FAT file chain has unexpected extra allocated clusters");
        }
    }
}

} // namespace iso_wcx
