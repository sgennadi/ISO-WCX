#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "wcx_wim_open_progress.hpp"
#include <commctrl.h>
#include <algorithm>
#include <chrono>
#include <string>

namespace iso_wcx {

WimOpenProgress::WimOpenProgress(bool enabled) noexcept {
    if (!enabled) return;
    try {
        worker_ = std::thread([this] { run(); });
    } catch (...) {
        // Native WIM reading must still work if the optional UI thread
        // cannot start (e.g. an automation or low-resource Windows host).
    }
}

WimOpenProgress::~WimOpenProgress() noexcept {
    finished_.store(true);
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void WimOpenProgress::set_image_count(std::uint32_t count) noexcept {
    image_count_.store(count);
}

void WimOpenProgress::on_scan(std::uint32_t index,std::size_t entries) {
    if (cancelled_.load()) throw WimOpenCancelled{};
    image_.store(index);
    entries_.store(static_cast<std::uint64_t>(entries));
    if (cancelled_.load()) throw WimOpenCancelled{};
}

std::wstring WimOpenProgress::describe() const {
    std::wstring result;
    const auto image=image_.load();
    const auto total=image_count_.load();
    if (!image) {
        result=L"Opening Windows Imaging API and reading WIM metadata...";
    } else {
        result=L"Reading image "+std::to_wstring(image);
        if (total) result+=L" of "+std::to_wstring(total);
        result+=L"\nMetadata entries discovered: "+
                std::to_wstring(entries_.load());
    }
    const auto seconds=(GetTickCount64()-started_)/1000u;
    result+=L"\nElapsed: "+std::to_wstring(seconds)+L" seconds";
    if (cancelled_.load()) {
        result+=L"\nCancellation requested. Waiting for the current "
                L"Windows Imaging API operation to return...";
    } else {
        result+=L"\nScanning directory metadata. File contents are not "
                L"being extracted or hashed.";
    }
    return result;
}

HRESULT CALLBACK WimOpenProgress::dialog_callback(
    HWND window,UINT message,WPARAM wparam,LPARAM,LONG_PTR context) noexcept {
    auto* self=reinterpret_cast<WimOpenProgress*>(context);
    if (!self) return S_OK;
    try {
        switch (message) {
        case TDN_CREATED:
            // WCX OpenArchive may be running on Total Commander's UI thread:
            // show the native dialog on our separate responsive UI thread.
            SetWindowPos(window,HWND_TOPMOST,0,0,0,0,
                         SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
            SendMessageW(window,TDM_SET_PROGRESS_BAR_MARQUEE,TRUE,75);
            break;
        case TDN_TIMER:
            if (self->finished_.load()) {
                PostMessageW(window,TDM_CLICK_BUTTON,IDCANCEL,0);
                break;
            }
            {
                auto message_text=self->describe();
                if (message_text!=self->last_message_) {
                    self->last_message_=message_text;
                    SendMessageW(window,TDM_SET_ELEMENT_TEXT,TDE_CONTENT,
                                 reinterpret_cast<LPARAM>(message_text.c_str()));
                }
            }
            break;
        case TDN_BUTTON_CLICKED:
            if (static_cast<int>(wparam)==IDCANCEL) {
                if (self->finished_.load()) return S_OK;
                // The synchronous WIMGAPI call may not be interruptible;
                // keep the progress window open until the next safe callback.
                self->cancelled_.store(true);
                const auto text=self->describe();
                SendMessageW(window,TDM_SET_ELEMENT_TEXT,TDE_CONTENT,
                             reinterpret_cast<LPARAM>(text.c_str()));
                return S_FALSE;
            }
            break;
        }
    } catch (...) {
        // UI failures must never escape from a Windows TaskDialog callback.
    }
    return S_OK;
}

void WimOpenProgress::run() noexcept {
    try {
        {
            std::unique_lock lock(wait_mutex_);
            // Don't flash a dialog for tiny boot.wim or immediately cached
            // archives. Both sides of this wait are woken by the destructor.
            if (wake_.wait_for(lock,std::chrono::milliseconds(1200),
                               [this]{return finished_.load();}))
                return;
        }
        if (finished_.load()) return;

        TASKDIALOGCONFIG config{};
        config.cbSize=sizeof(config);
        config.dwFlags=TDF_CALLBACK_TIMER|
                       TDF_SHOW_MARQUEE_PROGRESS_BAR|
                       TDF_ALLOW_DIALOG_CANCELLATION;
        config.dwCommonButtons=TDCBF_CANCEL_BUTTON;
        config.pszWindowTitle=L"ISO-WCX - Opening WIM";
        config.pszMainInstruction=L"Reading Windows image indexes";
        config.pszContent=L"Reading WIM metadata...";
        config.pszFooter=L"Metadata-only operation. The WIM remains unchanged.";
        config.pfCallback=&WimOpenProgress::dialog_callback;
        config.lpCallbackData=reinterpret_cast<LONG_PTR>(this);
        (void)TaskDialogIndirect(&config,nullptr,nullptr,nullptr);
    } catch (...) {
        // Closing the indicator cannot invalidate a successful WIM read.
    }
}
} // namespace iso_wcx
#endif
