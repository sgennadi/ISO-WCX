// Transactional, in-process FAT12 floppy editor: native C++20, no helpers.
// Write preview: unpartitioned images with validated FAT12 and DOS 8.3 paths.
#include "iso_wcx/archive_edit.hpp"
#include "iso_wcx/fat_volume.hpp"
#include "iso_wcx/partition_map.hpp"
#include "iso_wcx/virtual_disk.hpp"
#include "iso_wcx/vfat_name.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace iso_wcx {
namespace {
void check(bool ok, const char* why) {
    if (!ok) throw EditUnsupported(why);
}
std::uint16_t rd16(const std::vector<std::uint8_t>& v, std::size_t at) {
    check(at <= v.size() && v.size() - at >= 2, "truncated FAT12 word");
    return std::uint16_t(v[at]) | (std::uint16_t(v[at + 1]) << 8);
}
std::uint32_t rd32(const std::vector<std::uint8_t>& v, std::size_t at) {
    return std::uint32_t(rd16(v, at)) | (std::uint32_t(rd16(v, at + 2)) << 16);
}
void wr16(std::vector<std::uint8_t>& v, std::size_t at, std::uint16_t x) {
    check(at <= v.size() && v.size() - at >= 2, "FAT12 metadata write overflow");
    v[at] = static_cast<std::uint8_t>(x);
    v[at + 1] = static_cast<std::uint8_t>(x >> 8);
}
void wr32(std::vector<std::uint8_t>& v, std::size_t at, std::uint32_t x) {
    wr16(v, at, static_cast<std::uint16_t>(x));
    wr16(v, at + 2, static_cast<std::uint16_t>(x >> 16));
}
std::string upper(std::string name) {
    for (auto& c : name) if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    return name;
}
// Stable, locale-independent simple folding for common Latin, Cyrillic,
// and Greek letters. Other Unicode scalars remain exact.
std::string fold_name(std::string_view value) {
    const auto units = vfat::utf16(value);
    std::string out;
    for (const char16_t unit : units) {
        auto cp = static_cast<std::uint32_t>(unit);
        if (cp >= 0x61 && cp <= 0x7a) cp -= 0x20;
        else if (cp >= 0xe0 && cp <= 0xf6) cp -= 0x20;
        else if (cp >= 0xf8 && cp <= 0xfe) cp -= 0x20;
        else if (cp >= 0x430 && cp <= 0x44f) cp -= 0x20;
        else if (cp >= 0x450 && cp <= 0x45f) cp -= 0x50;
        else if (cp >= 0x3b1 && cp <= 0x3cb && cp != 0x3c2) cp -= 0x20;
        else if (cp == 0x3c2) cp = 0x3a3;
        vfat::put_scalar(out, cp);
    }
    return out;
}
std::string join(const std::string& dir, const std::string& name) {
    return dir.empty() ? name : dir + "/" + name;
}
void validate_vfat_component(std::string_view base) {
    check(!base.empty() && base != "." && base != ".." &&
          base.back() != '.' && base.back() != ' ',
          "invalid VFAT path component");
    (void)vfat::utf16(base);
}
std::string alias_name(const std::array<std::uint8_t,11>& alias) {
    std::string base(reinterpret_cast<const char*>(alias.data()), 8);
    std::string ext(reinterpret_cast<const char*>(alias.data()+8), 3);
    while (!base.empty() && base.back() == ' ') base.pop_back();
    while (!ext.empty() && ext.back() == ' ') ext.pop_back();
    return base + (ext.empty() ? "" : "." + ext);
}

std::string dos_name(const std::vector<std::uint8_t>& v, std::size_t at) {
    std::string base(reinterpret_cast<const char*>(v.data() + at), 8);
    std::string ext(reinterpret_cast<const char*>(v.data() + at + 8), 3);
    while (!base.empty() && base.back() == ' ') base.pop_back();
    while (!ext.empty() && ext.back() == ' ') ext.pop_back();
    check(!base.empty(), "empty FAT12 8.3 name");
    for (const unsigned char c : base) check(c >= 33 && c < 127, "non-ASCII FAT12 name is read-only");
    for (const unsigned char c : ext) check(c >= 33 && c < 127, "non-ASCII FAT12 name is read-only");
    return upper(base + (ext.empty() ? "" : "." + ext));
}
std::array<std::uint8_t, 11> dos_encode(std::string_view name) {
    const auto dot = name.find('.');
    const auto n = dot == std::string_view::npos ? name.size() : dot;
    const auto e = dot == std::string_view::npos ? 0u : name.size() - dot - 1;
    check(n >= 1 && n <= 8 && e <= 3 &&
          (dot == std::string_view::npos ||
           (e > 0 && name.find('.', dot + 1) == std::string_view::npos)),
          "FAT12 write preview requires DOS 8.3 names");
    std::array<std::uint8_t, 11> result{};
    result.fill(' ');
    const auto caps = upper(std::string(name));
    for (std::size_t i = 0; i < caps.size(); ++i) {
        if (i == dot) continue;
        const unsigned char c = static_cast<unsigned char>(caps[i]);
        const bool good = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
             std::string_view("!#$%&'()-@^_{}~").find(char(c)) != std::string_view::npos;
        check(good, "invalid FAT12 short-name character");
        const auto position = dot == std::string_view::npos || i < dot ? i : 8 + i - dot - 1;
        result[position] = c;
    }
    return result;
}
std::uint8_t name_checksum(const std::vector<std::uint8_t>& v, std::size_t at) {
    std::uint8_t s = 0;
    for (unsigned i = 0; i < 11; ++i)
        s = static_cast<std::uint8_t>(((s & 1u) << 7) + (s >> 1) + v[at + i]);
    return s;
}
struct Node {
    std::size_t at;
    std::uint16_t first;
    std::uint32_t size;
    std::uint8_t attrs;
    std::vector<std::size_t> long_name_slots;
    std::string dos_path;
    bool directory() const { return (attrs & 0x10u) != 0; }
};

class Editor {
public:
    explicit Editor(const std::filesystem::path& image) {
        check(std::filesystem::is_regular_file(image) && !std::filesystem::is_symlink(image),
              "FAT12 source image is not a regular file");
        const auto size = std::filesystem::file_size(image);
        check(size >= 512 && size <= 32u * 1024u * 1024u,
              "FAT12 write mode limited to 32 MiB");
        bytes_.resize(static_cast<std::size_t>(size));
        std::ifstream input(image, std::ios::binary);
        check(bool(input) && bool(input.read(reinterpret_cast<char*>(bytes_.data()),
                                             static_cast<std::streamsize>(bytes_.size()))),
              "cannot read FAT12 image");
        setup();
        walk("", 0, 0);
    }

    void apply(const ArchiveChange& c) {
        const auto logical = edit_path(c.path);
        const auto slash = logical.find_last_of('/');
        const auto raw_parent = slash == std::string::npos ? std::string{} :
                                logical.substr(0, slash);
        const auto parent = resolve_path(raw_parent);
        const auto base = slash == std::string::npos ? logical : logical.substr(slash + 1);
        validate_vfat_component(base);
        const auto path = resolve_path(join(parent, base));
        const auto existing = nodes_.find(path);
        if (c.remove) { erase(path); return; }
        check(dirs_.contains(parent), "FAT12 parent folder does not exist");
        if (c.directory && existing != nodes_.end()) {
            check(existing->second.directory(), "FAT12 file already uses folder name");
            return;
        }
        const auto planned = new_name(parent, base, existing != nodes_.end());
        if (c.directory) {
            const auto child = allocate(1).front();
            const auto at = offset(child);
            std::fill_n(bytes_.begin() + static_cast<std::ptrdiff_t>(at), cluster_bytes_, 0);
            std::array<std::uint8_t, 11> dot{};
            dot.fill(' '); dot[0] = '.';
            write_entry(at, dot, 0x10, child, 0);
            dot[1] = '.';
            write_entry(at + 32, dot, 0x10, dirs_.at(parent), 0);
            const auto reserved = reserve_slots(dirs_.at(parent), planned.lfn_count + 1);
            const auto short_at = reserved.back();
            const std::vector<std::size_t> lfn(reserved.begin(), reserved.end()-1);
            if (!lfn.empty()) vfat::encode(bytes_, lfn, base, planned.alias);
            write_entry(short_at, planned.alias, 0x10, child, 0);
            const auto dos_path = join(parent, alias_name(planned.alias));
            nodes_.emplace(path, Node{short_at, child, 0, 0x10, lfn, dos_path});
            dirs_.emplace(path, child);
            aliases_.emplace(dos_path, path);
            return;
        }
        check(std::filesystem::is_regular_file(c.source) &&
              !std::filesystem::is_symlink(c.source), "FAT12 input is not a regular file");
        const auto count = std::filesystem::file_size(c.source);
        check(count <= bytes_.size() && count <= UINT32_MAX,
              "new FAT12 file exceeds available volume size");
        std::vector<std::uint8_t> data(static_cast<std::size_t>(count));
        std::ifstream source(c.source, std::ios::binary);
        check(bool(source) && (data.empty() ||
              bool(source.read(reinterpret_cast<char*>(data.data()),
                               static_cast<std::streamsize>(data.size())))),
              "cannot read new FAT12 file");
        std::size_t slot{};
        std::vector<std::size_t> new_lfn;
        if (existing != nodes_.end()) {
            check(!existing->second.directory(), "cannot replace FAT12 directory");
            check(!(existing->second.attrs & 0x01), "FAT12 read-only file cannot be replaced");
            slot = existing->second.at;
            release(existing->second.first);
        } else {
            const auto reserved = reserve_slots(dirs_.at(parent), planned.lfn_count + 1);
            new_lfn.assign(reserved.begin(), reserved.end()-1);
            if (!new_lfn.empty())
                vfat::encode(bytes_, new_lfn, base, planned.alias);
            slot = reserved.back();
        }
        const auto needed = (data.size() + cluster_bytes_ - 1) / cluster_bytes_;
        const auto allocated = allocate(needed);
        for (std::size_t i = 0; i < allocated.size(); ++i) {
            const auto at = offset(allocated[i]);
            const auto first = i * cluster_bytes_;
            const auto count_here = std::min<std::size_t>(cluster_bytes_, data.size() - first);
            std::fill_n(bytes_.begin() + static_cast<std::ptrdiff_t>(at), cluster_bytes_, 0);
            std::copy_n(data.data() + first, count_here, bytes_.data() + at);
        }
        const auto first = allocated.empty() ? std::uint16_t{} : allocated.front();
        if (existing != nodes_.end()) {
            wr16(bytes_, slot + 26, first);
            wr32(bytes_, slot + 28, static_cast<std::uint32_t>(data.size()));
            existing->second.first = first;
            existing->second.size = static_cast<std::uint32_t>(data.size());
        } else {
            write_entry(slot, planned.alias, 0x20, first, static_cast<std::uint32_t>(data.size()));
            const auto dos_path = join(parent, alias_name(planned.alias));
            nodes_.emplace(path, Node{slot, first, static_cast<std::uint32_t>(data.size()),
                                      0x20, std::move(new_lfn), dos_path});
            aliases_.emplace(dos_path, path);
        }
    }

    void commit(const std::filesystem::path& image, const EditProgress& progress) const {
        if (progress && !progress()) throw EditCancelled();
        std::filesystem::path stage;
        std::random_device random;
        for (unsigned i = 0; i < 20; ++i) {
            auto attempt = image;
            attempt += ".iso-wcx-fat12-" + std::to_string(
                (std::uint64_t(random()) << 32) | random()) + ".tmp";
            if (!std::filesystem::exists(attempt)) { stage = attempt; break; }
        }
        check(!stage.empty(), "cannot select FAT12 staging filename");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() {
                if (!path.empty()) {
                    std::error_code ignored;
                    std::filesystem::remove(path, ignored);
                }
            }
        } cleanup{stage};
        {
            std::ofstream output(stage, std::ios::binary | std::ios::trunc);
            check(bool(output), "cannot create FAT12 staging image");
            output.write(reinterpret_cast<const char*>(bytes_.data()),
                         static_cast<std::streamsize>(bytes_.size()));
            output.close();
            check(bool(output), "cannot finish FAT12 staging image");
        }
        // Check the actual result, including mirrored FATs, all reachable
        // cluster chains, and browseability before replacing the original.
        {
            Editor audit(stage);
            (void)audit;
            VirtualDiskReader virtual_disk(stage);
            FatVolume filesystem(virtual_disk, 0, virtual_disk.size());
            check(filesystem.kind() == FatKind::fat12, "written FAT12 staging image invalid");
            (void)filesystem.list();
        } // Close read handles before the Windows atomic replacement.
        if (progress && !progress()) throw EditCancelled();
#ifdef _WIN32
        if (!MoveFileExW(stage.c_str(), image.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw EditUnsupported("atomic FAT12 image replace failed");
#else
        std::filesystem::rename(stage, image);
#endif
        cleanup.path.clear();
    }

private:
    std::vector<std::uint8_t> bytes_;
    std::size_t fat_at_{}, fat_length_{}, root_at_{}, data_at_{};
    std::size_t cluster_bytes_{}, root_entries_{};
    std::uint32_t clusters_{}, fats_{};
    std::map<std::string, Node> nodes_;
    std::map<std::string, std::uint16_t> dirs_{{"", 0}};
    std::map<std::string, std::string> aliases_;
    std::unordered_set<std::uint32_t> claimed_;

    void setup() {
        check(bytes_[0] == 0xe9 || (bytes_[0] == 0xeb && bytes_[2] == 0x90),
              "invalid FAT12 boot jump");
        check(bytes_[510] == 0x55 && bytes_[511] == 0xaa, "missing FAT12 boot signature");
        const auto bps = rd16(bytes_, 11);
        const auto spc = bytes_[13];
        const auto reserved = rd16(bytes_, 14);
        fats_ = bytes_[16];
        root_entries_ = rd16(bytes_, 17);
        const auto total = rd16(bytes_, 19) ? rd16(bytes_, 19) : rd32(bytes_, 32);
        const auto fat_sectors = rd16(bytes_, 22);
        check(bps >= 512 && bps <= 4096 && (bps & (bps - 1)) == 0 &&
              spc && (spc & (spc - 1)) == 0 && spc <= 128 && reserved &&
              (fats_ == 1 || fats_ == 2) && root_entries_ &&
              total && fat_sectors && (bytes_[21] == 0xf0 || bytes_[21] >= 0xf8),
              "unsupported FAT12 BPB");
        cluster_bytes_ = static_cast<std::size_t>(bps) * spc;
        check(cluster_bytes_ <= 1024u * 1024u, "FAT12 clusters too large");
        const auto root_sectors = (root_entries_ * 32u + bps - 1) / bps;
        const auto system_sectors = reserved + fats_ * fat_sectors + root_sectors;
        check(system_sectors < total, "FAT12 BPB has no data area");
        clusters_ = (total - system_sectors) / spc;
        check(clusters_ >= 1 && clusters_ < 4085, "image is not writable FAT12");
        check(static_cast<std::uint64_t>(total) * bps == bytes_.size(),
              "FAT12 editing requires exact raw image length");
        fat_at_ = static_cast<std::size_t>(reserved) * bps;
        fat_length_ = static_cast<std::size_t>(fat_sectors) * bps;
        root_at_ = (reserved + fats_ * fat_sectors) * static_cast<std::size_t>(bps);
        data_at_ = static_cast<std::size_t>(system_sectors) * bps;
        check((static_cast<std::uint64_t>(clusters_) + 2) * 3 <=
              static_cast<std::uint64_t>(fat_length_) * 2 &&
              fat_at_ + fats_ * fat_length_ <= root_at_ &&
              root_at_ + root_entries_ * 32 <= data_at_ &&
              data_at_ <= bytes_.size(), "FAT12 regions overlap");
        check((bytes_[fat_at_] & 0xf0u) == (bytes_[21] & 0xf0u) &&
              bytes_[fat_at_ + 1] == 0xff && bytes_[fat_at_ + 2] == 0xff,
              "FAT12 reserved entries damaged");
        for (std::uint32_t i = 1; i < fats_; ++i)
            check(std::equal(bytes_.begin() + static_cast<std::ptrdiff_t>(fat_at_),
                             bytes_.begin() + static_cast<std::ptrdiff_t>(fat_at_ + fat_length_),
                             bytes_.begin() + static_cast<std::ptrdiff_t>(fat_at_ + i * fat_length_)),
                  "FAT12 mirror copies differ");
    }
    std::uint16_t value(std::uint32_t c) const {
        check(c <= clusters_ + 1, "FAT12 cluster index out of range");
        const auto raw = rd16(bytes_, fat_at_ + c * 3 / 2);
        return static_cast<std::uint16_t>(c & 1u ? raw >> 4 : raw & 0xfffu);
    }
    void set(std::uint32_t c, std::uint16_t next) {
        check(c >= 2 && c <= clusters_ + 1 && next <= 0xfff,
              "invalid FAT12 allocation change");
        for (std::uint32_t f = 0; f < fats_; ++f) {
            const auto at = fat_at_ + f * fat_length_ + c * 3 / 2;
            if (c & 1u) {
                bytes_[at] = std::uint8_t((bytes_[at] & 0x0fu) | ((next & 15u) << 4));
                bytes_[at + 1] = std::uint8_t(next >> 4);
            } else {
                bytes_[at] = std::uint8_t(next);
                bytes_[at + 1] = std::uint8_t((bytes_[at + 1] & 0xf0u) | (next >> 8));
            }
        }
    }
    std::size_t offset(std::uint32_t c) const {
        check(c >= 2 && c <= clusters_ + 1, "invalid FAT12 data cluster");
        const auto at = data_at_ + static_cast<std::size_t>(c - 2) * cluster_bytes_;
        check(at <= bytes_.size() && cluster_bytes_ <= bytes_.size() - at,
              "FAT12 data cluster outside image");
        return at;
    }
    std::vector<std::uint16_t> chain(std::uint32_t first) const {
        check(first >= 2 && first <= clusters_ + 1, "invalid FAT12 chain head");
        std::unordered_set<std::uint32_t> seen;
        std::vector<std::uint16_t> result;
        auto c = first;
        for (;;) {
            check(seen.insert(c).second && seen.size() <= clusters_,
                  "cyclic FAT12 chain");
            (void)offset(c);
            result.push_back(static_cast<std::uint16_t>(c));
            const auto next = value(c);
            if (next >= 0xff8) return result;
            check(next >= 2 && next <= clusters_ + 1, "FAT12 chain has invalid link");
            c = next;
        }
    }
    std::vector<std::uint16_t> allocate(std::size_t count) {
        std::vector<std::uint16_t> result;
        for (std::uint32_t c = 2; c <= clusters_ + 1 && result.size() < count; ++c)
            if (value(c) == 0) result.push_back(static_cast<std::uint16_t>(c));
        check(result.size() == count, "FAT12 disk is full");
        for (std::size_t i = 0; i < result.size(); ++i)
            set(result[i], i + 1 < result.size() ? result[i + 1] : 0xfffu);
        return result;
    }
    void release(std::uint16_t first) {
        if (first) for (const auto c : chain(first)) set(c, 0);
    }
    std::vector<std::size_t> slots(std::uint16_t folder) const {
        std::vector<std::size_t> result;
        if (!folder) {
            for (std::size_t i = 0; i < root_entries_; ++i)
                result.push_back(root_at_ + i * 32);
        } else for (const auto c : chain(folder)) {
            const auto at = offset(c);
            for (std::size_t i = 0; i < cluster_bytes_; i += 32)
                result.push_back(at + i);
        }
        return result;
    }
    // Reserve adjacent logical directory records, including entries across
    // non-contiguous data clusters. Long-name entries must immediately precede
    // their short-name entry in the directory stream.
    std::vector<std::size_t> reserve_slots(std::uint16_t folder, std::size_t count) {
        check(count >= 1 && count <= 21, "invalid VFAT directory reservation");
        for (;;) {
            const auto positions = slots(folder);
            std::vector<std::size_t> available;
            for (const auto at : positions) {
                if (bytes_[at] == 0 || bytes_[at] == 0xe5) {
                    available.push_back(at);
                    if (available.size() == count) return available;
                } else available.clear();
            }
            check(folder != 0, "FAT12 root directory has no contiguous LFN slots");
            const auto old = chain(folder);
            const auto next = allocate(1).front();
            set(old.back(), next);
            const auto at = offset(next);
            std::fill_n(bytes_.begin() + static_cast<std::ptrdiff_t>(at),
                        cluster_bytes_, 0);
        }
    }
    // Resolve an explicit long name or its genuine 8.3 alias at every path
    // level. The alias map never determines a new file's identity.
    std::string resolve_path(std::string_view raw) const {
        if (raw.empty()) return {};
        std::string resolved;
        std::size_t at = 0;
        while (at < raw.size()) {
            const auto slash = raw.find('/', at);
            const auto end = slash == std::string_view::npos ? raw.size() : slash;
            check(end > at, "empty FAT12 path segment");
            const auto segment = fold_name(raw.substr(at, end-at));
            const auto proposed = join(resolved, segment);
            const auto match = aliases_.find(proposed);
            resolved = match == aliases_.end() ? proposed : match->second;
            at = end+1;
        }
        return resolved;
    }
    struct NamePlan {
        std::array<std::uint8_t,11> alias{};
        std::size_t lfn_count = 0;
    };
    NamePlan new_name(const std::string& parent, std::string_view name,
                      bool replacing) const {
        if (replacing) return {};
        validate_vfat_component(name);
        const auto count = vfat::required_slots(name);
        check(count >= 1 && count <= 20, "VFAT filename is too long");
        const auto unused = [&](const std::array<std::uint8_t,11>& entry) {
            const auto key = join(parent, alias_name(entry));
            return !aliases_.contains(key) && !nodes_.contains(key);
        };
        // Preserve a DOS short name when possible, but store LFN entries
        // when spelling/case does not exactly match the uppercase alias.
        try {
            const auto direct = dos_encode(name);
            if (unused(direct))
                return {direct, name == alias_name(direct) ? 0u : count};
        } catch (const EditUnsupported&) {
            // DOS alias generation below handles spaces, Unicode, and >8.3.
        }
        const auto dot = name.find_last_of('.');
        const auto source_base = dot == std::string_view::npos ? name : name.substr(0,dot);
        const auto source_ext = dot == std::string_view::npos ?
                                    std::string_view{} : name.substr(dot+1);
        const auto sanitized = [](std::string_view part) {
            std::string out;
            for (const unsigned char c : part) {
                const auto up = c >= 'a' && c <= 'z' ?
                                    static_cast<unsigned char>(c-32) : c;
                if ((up >= 'A' && up <= 'Z') || (up >= '0' && up <= '9') ||
                    up == '_' || up == '$')
                    out.push_back(static_cast<char>(up));
            }
            return out;
        };
        auto base = sanitized(source_base);
        if (base.empty()) base = "FILE";
        const auto extension = sanitized(source_ext).substr(0,3);
        for (std::uint32_t number=1; number<=999999; ++number) {
            const auto tail = "~" + std::to_string(number);
            const auto prefix = base.substr(0, 8-tail.size());
            auto stem = prefix + tail;
            std::array<std::uint8_t,11> result{};
            result.fill(' ');
            std::copy(stem.begin(), stem.end(), result.begin());
            std::copy(extension.begin(), extension.end(), result.begin()+8);
            if (unused(result)) return {result,count};
        }
        throw EditUnsupported("FAT12 short-name namespace exhausted");
    }
    void write_entry(std::size_t at, const std::array<std::uint8_t,11>& name,
                     std::uint8_t attrs, std::uint16_t first, std::uint32_t size) {
        check(at <= bytes_.size() && bytes_.size() - at >= 32, "FAT12 directory out of bounds");
        std::fill_n(bytes_.begin() + static_cast<std::ptrdiff_t>(at), 32, 0);
        std::copy(name.begin(), name.end(), bytes_.begin() + static_cast<std::ptrdiff_t>(at));
        bytes_[at + 11] = attrs;
        wr16(bytes_, at + 26, first);
        wr32(bytes_, at + 28, size);
    }
    void walk(const std::string& parent, std::uint16_t folder, unsigned depth) {
        check(depth <= 32, "FAT12 directory nesting too deep");
        if (folder) for (const auto c : chain(folder))
            check(claimed_.insert(c).second, "cross-linked FAT12 directories");
        std::vector<std::size_t> pending_lfn;
        for (const auto at : slots(folder)) {
            if (bytes_[at] == 0) {
                check(pending_lfn.empty(), "orphaned VFAT long-name records");
                break;
            }
            if (bytes_[at] == 0xe5) { pending_lfn.clear(); continue; }
            const auto attrs = bytes_[at + 11];
            if (attrs == 0x0f) {
                pending_lfn.push_back(at);
                check(pending_lfn.size() <= 20, "excessive VFAT long-name records");
                continue;
            }
            check((attrs & 0xc0u) == 0, "unsupported FAT12 directory attributes");
            if ((attrs & 0x08u) || bytes_[at] == '.') {
                check(pending_lfn.empty(), "orphaned VFAT records before label or dot entry");
                continue;
            }
            const auto dos = dos_name(bytes_, at);
            std::string actual = dos;
            if (!pending_lfn.empty()) {
                try {
                    actual = vfat::decode(bytes_, pending_lfn, at);
                } catch (const std::exception&) {
                    throw EditUnsupported("invalid VFAT long-name metadata; image remains read-only");
                }
                validate_vfat_component(actual);
            }
            const auto path = join(parent, fold_name(actual));
            const auto dos_path = join(parent, fold_name(dos));
            check(nodes_.size() < 100000 && !nodes_.contains(path) &&
                  !aliases_.contains(path) && !aliases_.contains(dos_path) &&
                  !nodes_.contains(dos_path),
                  "duplicate FAT12 name or ambiguous 8.3 alias");
            const auto first = rd16(bytes_, at + 26);
            const auto size = rd32(bytes_, at + 28);
            const bool directory = (attrs & 0x10u) != 0;
            check(rd16(bytes_, at + 20) == 0, "FAT12 high cluster word set");
            check(directory ? (size == 0 && first >= 2) :
                  (size == 0 ? first == 0 : first >= 2),
                  "invalid FAT12 directory entry");
            nodes_.emplace(path, Node{at, first, size, attrs, pending_lfn, dos_path});
            aliases_.emplace(dos_path, path);
            pending_lfn.clear();
            if (directory) {
                check(dirs_.emplace(path, first).second, "duplicate FAT12 folder");
                walk(path, first, depth + 1);
            } else if (first) {
                const auto blocks = chain(first);
                check(blocks.size() == (static_cast<std::size_t>(size) + cluster_bytes_ - 1) /
                      cluster_bytes_, "FAT12 file chain length mismatch");
                for (const auto c : blocks)
                    check(claimed_.insert(c).second, "cross-linked FAT12 files");
            }
        }
    }

    void erase(const std::string& path) {
        check(nodes_.contains(path), "FAT12 delete target missing");
        std::vector<std::string> targets;
        for (const auto& [key, node] : nodes_) {
            (void)node;
            if (key == path || (key.size() > path.size() &&
                key.compare(0, path.size(), path) == 0 && key[path.size()] == '/'))
                targets.push_back(key);
        }
        std::sort(targets.begin(), targets.end(), [](const auto& a, const auto& b) {
            return a.size() > b.size();
        });
        for (const auto& key : targets) {
            const auto node = nodes_.at(key);
            check(!(node.attrs & 0x01u), "cannot delete FAT12 read-only item");
            release(node.first);
            bytes_[node.at] = 0xe5;
            for (const auto at : node.long_name_slots) bytes_[at] = 0xe5;
            if (node.directory()) dirs_.erase(key);
            aliases_.erase(node.dos_path);
            nodes_.erase(key);
        }
    }
};
} // namespace

void edit_fat12(const std::filesystem::path& image,
                const std::vector<ArchiveChange>& changes,
                const EditProgress& progress) {
    const auto ext = upper(image.extension().string());
    check(ext == ".IMA" || ext == ".IMG" || ext == ".BIF" ||
          ext == ".VFD" || ext == ".FLP", "unsupported floppy image extension");
    {
        VirtualDiskReader disk(image);
        check(disk.format() == VirtualDiskFormat::raw_image,
              "FAT12 editor only accepts raw floppy images");
        try {
            const auto partitions = inspect_partition_table(disk, disk.logical_sector_size());
            check(partitions.empty(), "partitioned HDD is not editable as a floppy");
        } catch (const EditUnsupported&) { throw; }
          catch (const std::exception&) { /* A floppy is not an MBR. */ }
        FatVolume reader(disk, 0, disk.size());
        check(reader.kind() == FatKind::fat12, "FAT12 write mode refuses FAT16/32");
        (void)reader.list();
    } // The original must not be held open during an atomic replacement.
    Editor editor(image);
    std::vector<ArchiveChange> ordered = changes;
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const ArchiveChange& a, const ArchiveChange& b) {
        if (a.remove != b.remove) return !a.remove;
        auto depth = [](const std::string& s) {
            return std::count(s.begin(), s.end(), '/');
        };
        if (a.remove) return depth(a.path) > depth(b.path);
        if (a.directory != b.directory) return a.directory;
        return depth(a.path) < depth(b.path);
    });
    for (const auto& change : ordered) {
        if (progress && !progress()) throw EditCancelled();
        editor.apply(change);
    }
    if (!ordered.empty()) editor.commit(image, progress);
}
} // namespace iso_wcx
