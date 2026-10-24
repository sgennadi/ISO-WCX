#pragma once

#include "iso_wcx/disc.hpp"
#include "iso_wcx/volume_reader.hpp"
#include "iso_wcx/virtual_disk.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace iso_wcx {

// All virtual-disk code is linked directly into the WCX plugin.
// This reader never mounts volumes, launches EXEs, or loads external DLLs.
class VirtualArchive {
public:
    explicit VirtualArchive(const std::filesystem::path& image);
    VirtualArchive(const VirtualArchive&) = delete;
    VirtualArchive& operator=(const VirtualArchive&) = delete;

    [[nodiscard]] const std::vector<Item>& items() const noexcept { return items_; }
    [[nodiscard]] const std::string& format() const noexcept { return format_; }

    bool read(std::size_t item_index,
              const std::function<bool(const std::uint8_t*, std::size_t)>& accept);

private:
    struct FileLocation {
        std::size_t volume_index = 0;
        std::string relative_path;
    };
    VirtualDiskReader disk_;
    std::vector<std::unique_ptr<VolumeReader>> volumes_;
    std::vector<Item> items_;
    std::vector<FileLocation> files_;
    std::string format_;
    std::mutex read_mutex_;
};

} // namespace iso_wcx
