#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace iso_wcx {

// The Rock Ridge (RRIP) layer only interprets SUSP records. It does not follow
// symlinks and never writes to the host filesystem.
struct RockRidgeEntry {
    std::optional<std::string> alternate_name;
    bool extension = false; // recognized RRIP ER identifier
    bool symlink = false;
    bool relocated = false;
    bool child_link = false;
    std::optional<std::uint32_t> child_lba; // RRIP CL relocated directory block
};

// Returns SUSP skip length when a valid SP entry is present on the root "."
// record. A missing SP means this volume should use ISO9660/Joliet names.
std::optional<std::uint8_t> rock_ridge_skip(std::span<const std::uint8_t> root_dot);

using RockRidgeReader = std::function<std::vector<std::uint8_t>(std::uint64_t, std::size_t)>;
RockRidgeEntry rock_ridge_entry(std::span<const std::uint8_t> record,
                                std::uint8_t susp_skip,
                                const RockRidgeReader& read);

} // namespace iso_wcx
