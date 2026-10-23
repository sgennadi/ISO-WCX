#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace iso_wcx {

// Read-only partition discovery for *virtual hard disk images*.
// Deliberately separate from optical ISO/UDF session/track readers.
enum class PartitionTableKind : std::uint8_t { mbr, gpt };

struct PartitionEntry {
    PartitionTableKind scheme{};
    std::uint64_t first_lba{};
    std::uint64_t sector_count{};
    std::uint8_t mbr_type{}; // meaningful for MBR only
    std::array<std::uint8_t,16> gpt_type{}; // GUID raw bytes, GPT only
};

// Parse primary MBR entries, or a GPT whose protective MBR is present.
// Validate both GPT CRC32 fields, declared lengths, bounds and overlaps.
// For safety it does NOT access physical drives, follow any partition chain,
// mount filesystems, modify images, or open VHD/VMDK sparse containers.
// EBR logical partitions and hybrid-MBR overrides remain unsupported.
// Throws std::runtime_error for truncated, invalid, or unsupported layouts.
[[nodiscard]] std::vector<PartitionEntry> inspect_partition_table(
    const std::filesystem::path& raw_image,
    std::uint32_t logical_sector_size=512);

} // namespace iso_wcx
