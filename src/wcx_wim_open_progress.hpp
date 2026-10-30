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
    // Reuse the responsive, delayed TaskDialog thread for a WIM edit.
    // TC does not reliably refresh its own pack progress during F7.
    // The edit dialog is never shown by tests, services or non-TC hosts.
    enum class Mode { Opening, Editing };
    explicit WimOpenProgress(bool enabled,Mode mode=Mode::Opening) noexcept;
    ~WimOpenProgress() noexcept;
    WimOpenProgress(const WimOpenProgress&) = delete;
    WimOpenProgress& operator=(const WimOpenProgress&) = delete;

    void set_image_count(std::uint32_t count) noexcept;
    void on_scan(std::uint32_t index,std::size_t entries);
    // Once OpenArchive returns, TC still iterates every entry through
    // ReadHeader/PK_SKIP. Keep the dialog alive until this step finishes.
    void on_listing(std::size_t emitted,std::size_t total) noexcept;
    // WIMGAPI Apply/Capture and fast Mount/Commit report stage checkpoints.
    // False means the user requested cancellation; the edit must roll back.
    [[nodiscard]] bool on_edit(const wchar_t* stage,unsigned percent) noexcept;
    [[nodiscard]] bool cancelled() const noexcept { return cancelled_.load(); }

private:
    void run() noexcept;
    [[nodiscard]] std::wstring describe() const;
    static HRESULT CALLBACK dialog_callback(
        HWND window,UINT message,WPARAM wparam,LPARAM lparam,
        LONG_PTR context) noexcept;

    const Mode mode_;
    mutable std::mutex edit_mutex_;
    std::wstring edit_stage_;
    std::atomic<unsigned> edit_percent_{0};
    std::atomic<ULONGLONG> edit_last_update_{0};
    std::atomic<bool> finished_{false};
    std::atomic<bool> cancelled_{false};
    std::atomic<std::uint32_t> image_{0};
    std::atomic<std::uint32_t> image_count_{0};
    std::atomic<std::uint64_t> entries_{0};
    std::atomic<std::uint64_t> published_{0};
    std::atomic<std::uint64_t> total_entries_{0};
    std::atomic<bool> publishing_{false};
    const ULONGLONG started_=GetTickCount64();
    std::mutex wait_mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    std::wstring last_message_;
    bool progress_is_determinate_=false;
};
} // namespace iso_wcx
#endif
