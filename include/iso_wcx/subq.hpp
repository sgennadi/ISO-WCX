#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace iso_wcx {

// CloneCD .sub stores one 96-byte P-W packet per 2352-byte .img sector.
// Known sources differ on P-W ordering. CRC validation, not a file extension
// heuristic, decides which Q frame representation can be trusted.
enum class SubQLayout : std::uint8_t { deinterleaved, interleaved };

struct SubQPosition {
    std::uint8_t control{};
    std::uint8_t track{};
    std::uint8_t index{};
    std::uint32_t relative_frame{};
    std::uint32_t absolute_frame{};
};

[[nodiscard]] std::array<std::uint8_t,12> extract_subq(
    std::span<const std::uint8_t,96> packet, SubQLayout layout) noexcept;
[[nodiscard]] bool subq_crc_valid(std::span<const std::uint8_t,12> frame) noexcept;
[[nodiscard]] std::optional<SubQPosition> decode_subq_position(
    std::span<const std::uint8_t,12> frame) noexcept;

// Scans in constant memory, rejects badly sized files and bounds the work to
// 1,000,000 sectors. Invalid CRC is counted, NOT treated as a disc error:
// deliberately unusual Q data on copy-protected media must remain accessible.
// Returns a UTF-8 CSV report suitable for an optional read-only WCX entry.
[[nodiscard]] std::string subq_index_report(const std::filesystem::path& sub_file);

} // namespace iso_wcx
