#pragma once
#ifdef _WIN32
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <string>
#include <thread>

namespace iso_wcx {

// OpenArchive runs before Total Commander can supply a WCX per-archive
// SetProcessDataProc callback. A separate native Windows task-dialog thread
// is therefore necessary to show responsive WIM enumeration progress.
struct WimOpenCancelled final : std::exception {
    const char* what() const noexcept override { return "WIM opening cancelled by user"; }
};

class WimOpenProgress final {
public:
    explicit WimOpenProgress(bool enabled) noexcept;
    ~WimOpenProgress() noexcept;
    WimOpenProgress(const WimOpenProgress&) = delete;
    WimOpenProgress& operator=(const WimOpenProgress&) = delete;

    void set_image_count(std::uint32_t count) noexcept;
    void on_scan(std::uint32_t index,std::size_t entries);
    [[nodiscard]] bool cancelled() const noexcept { return cancelled_.load(); }

private:
    void run() noexcept;
    [[nodiscard]] std::wstring describe() const;
    static HRESULT CALLBACK dialog_callback(
        HWND window,UINT message,WPARAM wparam,LPARAM lparam,
        LONG_PTR context) noexcept;

    std::atomic<bool> finished_{false};
    std::atomic<bool> cancelled_{false};
    std::atomic<std::uint32_t> image_{0};
    std::atomic<std::uint32_t> image_count_{0};
    std::atomic<std::uint64_t> entries_{0};
    const ULONGLONG started_=GetTickCount64();
    std::mutex wait_mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    std::wstring last_message_;
};
} // namespace iso_wcx
#endif
