#pragma once
// Read-only WIM fast-metadata-edit preflight. NEVER modifies a WIM.
// A candidate result does not authorize writing: a future editor must
// decode/validate and rebuild metadata, lookup, XML and integrity resources.
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

namespace iso_wcx::wim_detail {

struct FastEditProbe {
    bool header_and_lookup_valid = false;
    bool metadata_layout_candidate = false;
    std::string reason;
    std::uint32_t images = 0;
    std::uint32_t version = 0;
    std::uint32_t flags = 0;
    std::uint64_t archive_size = 0;
    std::uint64_t lookup_entries = 0;
    std::uint64_t metadata_resources = 0;
    std::uint64_t compressed_metadata_resources = 0;
    std::uint64_t metadata_uncompressed_bytes = 0;
};

namespace fast_probe_detail {
inline std::uint16_t le16(const std::uint8_t* p) noexcept {
    return std::uint16_t(p[0]) | (std::uint16_t(p[1]) << 8);
}
inline std::uint32_t le32(const std::uint8_t* p) noexcept {
    return std::uint32_t(le16(p)) | (std::uint32_t(le16(p + 2)) << 16);
}
inline std::uint64_t le64(const std::uint8_t* p) noexcept {
    return std::uint64_t(le32(p)) | (std::uint64_t(le32(p + 4)) << 32);
}
inline std::uint64_t resource_size(const std::uint8_t* p) noexcept {
    std::uint64_t size = 0;
    for (unsigned i = 0; i < 7; ++i) size |= std::uint64_t(p[i]) << (i * 8);
    return size;
}
inline bool inside(std::uint64_t offset, std::uint64_t bytes, std::uint64_t size) noexcept {
    return offset <= size && bytes <= size - offset;
}
inline bool intersects(std::uint64_t a, std::uint64_t len_a,
                       std::uint64_t b, std::uint64_t len_b) noexcept {
    return a < b + len_b && b < a + len_a;
}
inline bool read_at(std::ifstream& input, std::uint64_t offset, void* output, std::size_t len) {
    if (offset > std::uint64_t(std::numeric_limits<std::streamoff>::max())) return false;
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    input.read(static_cast<char*>(output), static_cast<std::streamsize>(len));
    return static_cast<bool>(input);
}
} // namespace fast_probe_detail

// Candidate means only that the header, uncompressed lookup table and
// metadata resource descriptors passed minimal bounds/layout checks.
// Metadata security, directory trees and streams have NOT been parsed.
// Any future fast writer must independently validate the full format.
inline FastEditProbe probe_fast_edit_metadata(const std::filesystem::path& file) {
    using namespace fast_probe_detail;
    FastEditProbe p;
    const auto invalid = [&](const char* reason) {
        p.reason = reason;
        return p;
    };
    std::error_code ec;
    const auto actual = std::filesystem::file_size(file, ec);
    if (ec) return invalid("cannot stat WIM");
    p.archive_size = actual;
    if (actual < 208) return invalid("WIM is smaller than its header");
    std::ifstream in(file, std::ios::binary);
    if (!in) return invalid("cannot open WIM");
    std::array<std::uint8_t,208> hdr{};
    if (!read_at(in, 0, hdr.data(), hdr.size())) return invalid("cannot read WIM header");
    constexpr std::array<std::uint8_t,8> magic{{'M','S','W','I','M',0,0,0}};
    for (unsigned i = 0; i < magic.size(); ++i)
        if (hdr[i] != magic[i]) return invalid("invalid WIM signature");
    const auto header_size = le32(hdr.data() + 8);
    if (header_size < 208 || header_size > actual || header_size > 4096)
        return invalid("invalid WIM header size");
    p.version = le32(hdr.data() + 12);
    p.flags = le32(hdr.data() + 16);
    p.images = le32(hdr.data() + 44);
    const auto part = le16(hdr.data() + 40);
    const auto parts = le16(hdr.data() + 42);
    if (p.version != 0x00010d00)
        return invalid("unrecognized WIM version");
    if (!p.images || p.images > 256 || !part || !parts || part > parts)
        return invalid("invalid WIM image or segment count");
    const std::uint8_t* lookup = hdr.data() + 48;
    const auto lookup_length = resource_size(lookup);
    const auto lookup_flags = lookup[7];
    const auto lookup_offset = le64(lookup + 8);
    const auto lookup_original = le64(lookup + 16);
    if (!lookup_length || lookup_length > (128u << 20) ||
        lookup_length % 50u != 0 || lookup_flags != 0 ||
        lookup_length != lookup_original ||
        !inside(lookup_offset, lookup_length, actual) || lookup_offset < header_size)
        return invalid("WIM lookup table is unsupported or outside file");
    p.lookup_entries = lookup_length / 50u;
    if (p.lookup_entries < p.images)
        return invalid("WIM lookup table has fewer entries than images");
    std::array<std::uint8_t,50> entry{};
    for (std::uint64_t i = 0; i < p.lookup_entries; ++i) {
        const auto entry_pos = lookup_offset + i * entry.size();
        if (!read_at(in, entry_pos, entry.data(), entry.size()))
            return invalid("cannot read WIM lookup entry");
        const auto entry_flags = entry[7];
        if (!(entry_flags & 0x02u)) continue;
        // A metadata descriptor may not be marked free, spanned or carry
        // any unknown bit in this deliberately narrow fast-edit preflight.
        if ((entry_flags & ~0x06u) != 0)
            return invalid("unsupported WIM metadata resource flags");
        ++p.metadata_resources;
        const auto len = resource_size(entry.data());
        const auto offset = le64(entry.data() + 8);
        const auto uncompressed = le64(entry.data() + 16);
        const auto entry_part = le16(entry.data() + 24);
        if (!len || !uncompressed || entry_part != part ||
            !inside(offset, len, actual) || offset < header_size ||
            intersects(offset, len, lookup_offset, lookup_length))
            return invalid("WIM metadata resource is invalid or overlaps lookup table");
        if (uncompressed > (512u << 20) ||
            p.metadata_uncompressed_bytes > (512u << 20) - uncompressed)
            return invalid("WIM metadata exceeds preflight size limit");
        p.metadata_uncompressed_bytes += uncompressed;
        if ((entry_flags & 0x04u) != 0) ++p.compressed_metadata_resources;
        else if (len != uncompressed)
            return invalid("uncompressed WIM metadata has inconsistent size");
    }
    p.header_and_lookup_valid = true;
    if (p.metadata_resources != p.images) {
        p.reason = "metadata lookup count does not match image count";
    }else if (part != 1 || parts != 1 ||
             (p.flags & (0x00000004u | 0x00000008u | 0x00000010u |
                         0x00000020u | 0x00000040u | 0x00080000u)) != 0) {
        p.reason = "segmented, solid, read-only or special WIM is not a fast-edit candidate";
    }else if (p.compressed_metadata_resources) {
        p.reason = "WIM image metadata must be decompressed before fast editing";
    }else {
        p.metadata_layout_candidate = true;
        p.reason = "uncompressed metadata layout observed (read-only; writer not implemented)";
    }
    return p;
}
} // namespace iso_wcx::wim_detail
