#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace iso_wcx {

// A separate read-only byte device for virtual *hard* disks. Optical sector
// parsing remains in iso_disc and never uses this interface.
enum class VirtualDiskFormat : std::uint8_t { raw_image, fixed_vhd, dynamic_vhd, vhdx };

class VirtualDiskReader {
public:
    explicit VirtualDiskReader(const std::filesystem::path& image);
    ~VirtualDiskReader() noexcept;
    VirtualDiskReader(const VirtualDiskReader&) = delete;
    VirtualDiskReader& operator=(const VirtualDiskReader&) = delete;

    [[nodiscard]] std::uint64_t size() const noexcept { return capacity_; }
    [[nodiscard]] VirtualDiskFormat format() const noexcept { return format_; }

    // Strictly bounded random-access logical bytes, with VHD footer hidden.
    // No writes, mounting, host-device handles, or parent file resolution.
    [[nodiscard]] std::vector<std::uint8_t> read(std::uint64_t offset,
                                                  std::size_t length);

private:
    void open_optional_backend(const std::filesystem::path& image,
                               VirtualDiskFormat expected);
    std::ifstream input_;
    void* library_backend_ = nullptr; // Only populated when libvhdi is opt-in enabled.
    std::uint64_t capacity_{};
    VirtualDiskFormat format_ = VirtualDiskFormat::raw_image;
};

} // namespace iso_wcx
