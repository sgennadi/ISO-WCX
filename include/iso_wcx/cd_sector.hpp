#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace iso_wcx {
// ECMA-130 Annex B: CD-ROM sector scrambler, 15-bit LFSR, x^15+x+1.
// Reset to 1 after every 12-byte sync. Only sector bytes 12..2351 are XORed.
// This is a *reversible physical encoding*, not cryptographic encryption.
[[nodiscard]] inline const std::array<std::uint8_t,2340>& cd_scramble_mask() {
    static const auto mask=[] {
        std::array<std::uint8_t,2340> result{};
        std::uint16_t lfsr=1;
        for(auto& byte:result) {
            for(unsigned bit=0;bit<8;++bit) {
                byte|=static_cast<std::uint8_t>((lfsr&1u)<<bit);
                const std::uint16_t feedback=(lfsr^(lfsr>>1))&1u;
                lfsr=static_cast<std::uint16_t>((lfsr>>1)|(feedback<<14));
            }
        }
        return result;
    }();
    return mask;
}
} // namespace iso_wcx
