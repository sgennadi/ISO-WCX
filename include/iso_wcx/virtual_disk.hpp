#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

namespace iso_wcx {

class VhdxReader;
class DynamicVhdReader;

// Native read-only virtual HDD byte stream. Optical CD sector parsing
// remains separate. No external disk libraries or runtime plugin DLLs.
enum class VirtualDiskFormat : std::uint8_t { raw_image, compressed_floppy, fixed_vhd, dynamic_vhd, vhdx };

class VirtualDiskReader {
public:
    explicit VirtualDiskReader(const std::filesystem::path& image);
    ~VirtualDiskReader() noexcept;
    VirtualDiskReader(const VirtualDiskReader&) = delete;
    VirtualDiskReader& operator=(const VirtualDiskReader&) = delete;

    [[nodiscard]] std::uint64_t size() const noexcept { return capacity_; }
    [[nodiscard]] VirtualDiskFormat format() const noexcept { return format_; }
    [[nodiscard]] std::uint32_t logical_sector_size() const noexcept;
    [[nodiscard]] const DynamicVhdReader* dynamic_vhd_layout() const noexcept {
        return dynamic_vhd_.get();
    }

    // Logical bytes: fixed VHD footer, dynamic VHD BAT/metadata, and VHDX
    // allocation metadata are hidden.
    // Neither mounts host volumes, writes, nor resolves snapshot parents.
    [[nodiscard]] std::vector<std::uint8_t> read(std::uint64_t offset,
                                                  std::size_t length);

private:
    std::ifstream input_;
    // IMZ is a bounded, CRC-verified in-memory raw floppy.
    std::vector<std::uint8_t> imz_bytes_;
    std::unique_ptr<VhdxReader> vhdx_;
    std::unique_ptr<DynamicVhdReader> dynamic_vhd_;
    std::uint64_t capacity_ = 0;
    VirtualDiskFormat format_ = VirtualDiskFormat::raw_image;
};

} // namespace iso_wcx
