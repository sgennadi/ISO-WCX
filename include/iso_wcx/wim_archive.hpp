#pragma once

#include "iso_wcx/disc.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace iso_wcx {

// Windows WIMGAPI is provided by Windows itself. This implementation is
// inside ISO-WCX.wcx/.wcx64: no wimlib-imagex.exe, WIM mount, driver,
// redistributable codec DLL, or external helper is required at runtime.
class WimArchive final {
public:
    // An optional one-based index limits WIMGAPI metadata enumeration and
    // decompressed stream reads to a single image. Zero preserves the normal
    // WCX behavior of exposing all images. The optional diagnostic callback
    // may reject a long-running enumeration and is never used by normal WCX.
    using ScanProgress = std::function<void(std::uint32_t,std::size_t)>;
    explicit WimArchive(const std::filesystem::path& image,
                        std::uint32_t only_image_index = 0,
                        ScanProgress scan_progress = {});
    ~WimArchive();
    WimArchive(const WimArchive&) = delete;
    WimArchive& operator=(const WimArchive&) = delete;

    // Before atomically replacing a WIM, release read handles retained by
    // open Total Commander panels for this exact source file. Their existing
    // listings become stale and must not extract against the new image.
    // Concurrent stream reads finish before handles are released.
    static void release_open_readers(const std::filesystem::path& image);

    [[nodiscard]] const std::vector<Item>& items() const noexcept { return entries_; }
    [[nodiscard]] const std::string& format() const noexcept { return format_; }

    // Read the entire original file through WIMGAPI to the WCX transaction
    // callback. false means the WCX progress callback requested cancellation.
    bool read(std::size_t item_index,
              const std::function<bool(const std::uint8_t*, std::size_t)>& consumer);

private:
    struct Impl;
    struct Location {
        std::uint32_t image_index = 0;
        std::wstring image_path;
    };
    std::unique_ptr<Impl> api_;
    std::vector<Item> entries_;
    std::vector<Location> locations_;
    std::string format_;
    std::mutex read_mutex_;
    bool invalidated_by_edit_ = false;
    // Shared under read_mutex_: reusing one read buffer avoids zero-filling
    // and allocating 1 MiB for every small file in large install.wim images.
    std::vector<std::uint8_t> read_buffer_;
};

} // namespace iso_wcx
