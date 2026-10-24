#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>
namespace iso_wcx {
[[nodiscard]] std::array<std::uint8_t,20> mds_ripemd160(std::span<const std::uint8_t> input);
// Decrypt/inflate the bounded MDS v2/MDX descriptor. Data sectors are not decoded.
[[nodiscard]] std::vector<std::uint8_t> decode_mds_v2_descriptor(const std::filesystem::path& image);
}
