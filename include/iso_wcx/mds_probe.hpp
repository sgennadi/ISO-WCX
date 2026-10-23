#pragma once
#include <cstdint>
#include <filesystem>

namespace iso_wcx {

// Read-only, *metadata-only* recognition. No decryption, decompression,
// password handling, data-track extraction or companion file access.
enum class MdsDescriptorKind : std::uint8_t {
    alcohol_v1, daemon_mds_v2, daemon_mdx
};

struct MdsDescriptorProbe {
    MdsDescriptorKind kind = MdsDescriptorKind::alcohol_v1;
    std::uint8_t major = 0, minor = 0;
    std::uint64_t file_bytes = 0;
    // Only relevant to Daemon Tools MDSv2/MDX. This encrypted header is
    // NOT plaintext MDS metadata and must never be parsed as a track table.
    std::uint64_t encrypted_header_offset = 0;
    std::uint64_t packed_metadata_offset = 0;
    std::uint64_t packed_metadata_length = 0;
};

// Fail closed if magic, version, embedded-MDX sentinel or offsets are invalid.
[[nodiscard]] MdsDescriptorProbe probe_mds_descriptor(
    const std::filesystem::path& image);

} // namespace iso_wcx
