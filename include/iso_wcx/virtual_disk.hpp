#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace iso_wcx {

// Native read-only virtual HDD bytes. Optical CD sector parsing is separate.
// No disk-library DLLs, mounts, host-device handles or parent image lookup.
enum class VirtualDiskFormat : std::uint8_t { raw_image, fixed_vhd };

class VirtualDiskReader {
public:
    explicit VirtualDiskReader(const std::filesystem::path& image);
    VirtualDiskReader(const VirtualDiskReader&) = delete;
    VirtualDiskReader& operator=(const VirtualDiskReader&) = delete;

    [[nodiscard]] std::uint64_t size() const noexcept { return capacity_; }
    [[nodiscard]] VirtualDiskFormat format() const noexcept { return format_; }
    [[nodiscard]] std::vector<std::uint8_t> read(std::uint64_t offset,
                                                  std::size_t length);

private:
    std::ifstream input_;
    std::uint64_t capacity_ = 0;
    VirtualDiskFormat format_ = VirtualDiskFormat::raw_image;
};

} // namespace iso_wcx
