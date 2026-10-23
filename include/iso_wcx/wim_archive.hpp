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
    explicit WimArchive(const std::filesystem::path& image);
    ~WimArchive();
    WimArchive(const WimArchive&) = delete;
    WimArchive& operator=(const WimArchive&) = delete;

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
};

} // namespace iso_wcx
