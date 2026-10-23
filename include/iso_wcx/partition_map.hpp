#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace iso_wcx {

// Read-only partition discovery for *virtual hard disk images*.
// Deliberately separate from optical ISO/UDF session/track readers.
class VirtualDiskReader;
enum class PartitionTableKind : std::uint8_t { mbr, gpt };

struct PartitionEntry {
    PartitionTableKind scheme{};
    std::uint64_t first_lba{};
    std::uint64_t sector_count{};
    std::uint8_t mbr_type{}; // meaningful for MBR only
    std::array<std::uint8_t,16> gpt_type{}; // GUID raw bytes, GPT only
};

// Read primary MBR partitions, bounded linked EBR logical partitions, or GPT.
// Validate GPT primary CRC32 and partition-array CRC, extent bounds, overlaps,
// EBR metadata and cycle prevention. Hybrid GPT/MBR is rejected.
// No physical-drive access, mounting, writes or external parent-file traversal.
// If primary GPT is damaged, a fully checked backup GPT may be used.
// Throws std::runtime_error for truncated, invalid, or unsupported layouts.
// Reuse an already-open container (avoids re-opening libvhdi and re-reading
// metadata in the CLI or future filesystem adapters).
[[nodiscard]] std::vector<PartitionEntry> inspect_partition_table(
    VirtualDiskReader& disk, std::uint32_t logical_sector_size=512);

[[nodiscard]] std::vector<PartitionEntry> inspect_partition_table(
    const std::filesystem::path& raw_image,
    std::uint32_t logical_sector_size=512);

} // namespace iso_wcx
