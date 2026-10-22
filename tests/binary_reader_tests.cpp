#include "iso_wcx/binary_reader.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

int main() {
    constexpr std::array<std::byte, 4> bytes{
        std::byte{0x78}, std::byte{0x56}, std::byte{0x34}, std::byte{0x12}};
    const iso_wcx::BinaryReader reader(bytes);
    if (reader.u8(0) != std::uint8_t{0x78}) return 1;
    if (reader.le16(0) != std::uint16_t{0x5678}) return 2;
    if (reader.le32(0) != std::uint32_t{0x12345678}) return 3;
    if (reader.le16(3).has_value()) return 4;
    if (reader.le32(1).has_value()) return 5;
    if (reader.slice(3, 2).has_value()) return 6;
    if (!reader.slice(4, 0).has_value()) return 7;
    if (reader.u8(4).has_value()) return 8;
    return 0;
}
