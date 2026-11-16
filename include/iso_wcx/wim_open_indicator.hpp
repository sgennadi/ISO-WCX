#pragma once

#ifdef _WIN32
#include "iso_wcx/disc.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>

namespace iso_wcx {

// WCX OpenArchive executes before TC can register SetProcessDataProcW.
// This independent, delayed native dialog is therefore the only reliable
// opening indicator for multi-gigabyte WIM/ESD/SWM metadata enumeration.
// No fake percentage, no helper EXE, no WIM mount and no background work
// after OpenArchive returns.
class WimOpenIndicator final {
public:
    explicit WimOpenIndicator(const std::filesystem::path& image) noexcept;
    ~WimOpenIndicator() noexcept;
    WimOpenIndicator(const WimOpenIndicator&) = delete;
    WimOpenIndicator& operator=(const WimOpenIndicator&) = delete;

    // Called synchronously by the WIMGAPI enumeration thread.
    // false means the user requested Cancel; no Win32 GUI calls here.
    bool report(const ArchiveOpenStatus& status) noexcept;

private:
    static HRESULT CALLBACK task_callback(
        HWND dialog, UINT message, WPARAM wparam,
        LPARAM lparam, LONG_PTR context) noexcept;
    void run() noexcept;
    // Compatibility window when the hosting Total Commander process has
    // comctl32 v5 (TaskDialogIndirect is unavailable). Never hide a long
    // WIM load just because the OS modern-dialog API cannot be activated.
    void run_fallback() noexcept;
    static LRESULT CALLBACK fallback_proc(
        HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept;
    void refresh(HWND dialog) noexcept;
    [[nodiscard]] std::wstring status_text() const;

    std::wstring title_;
    std::wstring heading_;
    std::wstring last_text_; // UI thread only.
    bool russian_ = false;
    bool active_ = false;
    std::atomic<unsigned> phase_{0};
    std::atomic<std::uint32_t> index_{0};
    std::atomic<std::uint32_t> total_{0};
    std::atomic<std::uint64_t> scanned_{0};
    std::atomic<bool> cancelled_{false};
    std::atomic<HWND> dialog_{nullptr};
    std::atomic<bool> fallback_window_{false};
    unsigned fallback_frame_ = 0; // fallback UI thread only
    HANDLE stop_event_ = nullptr;
    std::thread thread_;
};

} // namespace iso_wcx
#endif
