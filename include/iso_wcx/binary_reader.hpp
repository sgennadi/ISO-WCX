#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace iso_wcx {
class BinaryReader {
public:
    explicit BinaryReader(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] std::optional<std::uint8_t> u8(std::size_t offset) const noexcept {
        if (offset >= bytes_.size()) return std::nullopt;
        return std::to_integer<std::uint8_t>(bytes_[offset]);
    }

    [[nodiscard]] std::optional<std::uint16_t> le16(std::size_t offset) const noexcept {
        if (offset > bytes_.size() || bytes_.size() - offset < 2) return std::nullopt;
        const auto a = std::to_integer<std::uint8_t>(bytes_[offset]);
        const auto b = std::to_integer<std::uint8_t>(bytes_[offset + 1]);
        return static_cast<std::uint16_t>(a | (static_cast<std::uint16_t>(b) << 8));
    }

    [[nodiscard]] std::optional<std::uint32_t> le32(std::size_t offset) const noexcept {
        if (offset > bytes_.size() || bytes_.size() - offset < 4) return std::nullopt;
        std::uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) {
            value |= std::uint32_t(std::to_integer<std::uint8_t>(bytes_[offset + i])) << (8 * i);
        }
        return value;
    }

    [[nodiscard]] std::optional<std::span<const std::byte>> slice(
        std::size_t offset, std::size_t length) const noexcept {
        if (offset > bytes_.size() || length > bytes_.size() - offset) return std::nullopt;
        return bytes_.subspan(offset, length);
    }

private:
    std::span<const std::byte> bytes_;
};
} // namespace iso_wcx
