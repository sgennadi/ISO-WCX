#pragma once
// Pure, bounded WIM transaction disk-space preflight.
//
// A standard WIMGAPI Apply/Capture edit creates a complete staged WIM and
// expands one or more affected image indexes under the same NTFS volume.
// Checking only the size of source files is unsafe: even a folder creation
// can run out of space late in a multi-minute transaction. This is a lower
// bound, NOT an estimate of expanded image size.
#include <algorithm>
#include <cstdint>
#include <limits>

namespace iso_wcx::wim_detail {
inline constexpr std::uint64_t wim_mib=1024ull*1024ull;
inline constexpr std::uint64_t wim_gib=1024ull*wim_mib;

struct WimStagingBudget {
    std::uint64_t source_bytes=0;
    std::uint64_t available_bytes=0;
    std::uint64_t reserve_bytes=0;
    std::uint64_t required_bytes=0;
    bool arithmetic_overflow=false;
    bool enough=false;
};

// Reserve the compressed output size plus 12.5% headroom (128 MiB to 1 GiB).
// Apply also needs *uncompressed* scratch, which may be much larger, so a
// successful check never guarantees completion. No user files are touched.
[[nodiscard]] constexpr WimStagingBudget wim_staging_budget(
        std::uint64_t compressed_wim_bytes,
        std::uint64_t available_to_caller_bytes) noexcept {
    WimStagingBudget budget{};
    budget.source_bytes=compressed_wim_bytes;
    budget.available_bytes=available_to_caller_bytes;
    const auto proportional=compressed_wim_bytes/8u;
    constexpr std::uint64_t minimum_reserve=128u*wim_mib;
    constexpr std::uint64_t maximum_reserve=1u*wim_gib;
    budget.reserve_bytes=std::clamp(proportional,
        minimum_reserve,maximum_reserve);
    if(compressed_wim_bytes >
       std::numeric_limits<std::uint64_t>::max()-budget.reserve_bytes) {
        budget.arithmetic_overflow=true;
        return budget;
    }
    budget.required_bytes=compressed_wim_bytes+budget.reserve_bytes;
    budget.enough=available_to_caller_bytes>=budget.required_bytes;
    return budget;
}

[[nodiscard]] constexpr std::uint64_t wim_mib_ceil(
        std::uint64_t bytes) noexcept {
    return bytes/wim_mib + (bytes%wim_mib!=0u);
}
} // namespace iso_wcx::wim_detail
