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

WimOpenProgress::WimOpenProgress(bool enabled,Mode mode) noexcept
    : mode_(mode) {
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

void WimOpenProgress::on_listing(std::size_t emitted,std::size_t total) noexcept {
    total_entries_.store(static_cast<std::uint64_t>(total));
    published_.store(static_cast<std::uint64_t>(emitted));
    publishing_.store(true);
}

bool WimOpenProgress::on_edit(const wchar_t* stage,unsigned percent) noexcept {
    if (cancelled_.load()) return false;
    try {
        {
            std::lock_guard lock(edit_mutex_);
            edit_stage_=stage?stage:L"Processing Windows image";
        }
        edit_percent_.store(std::min(percent,99u));
        return !cancelled_.load();
    } catch (...) {
        // A failed optional UI update must not corrupt an otherwise valid WIM.
        return !cancelled_.load();
    }
}

void WimOpenProgress::on_scan(std::uint32_t index,std::size_t entries) {
    if (cancelled_.load()) throw WimOpenCancelled{};
    image_.store(index);
    entries_.store(static_cast<std::uint64_t>(entries));
    if (cancelled_.load()) throw WimOpenCancelled{};
}

namespace {
std::wstring elapsed_label(ULONGLONG milliseconds) {
    const auto seconds=milliseconds/1000u;
    const auto pad=[](ULONGLONG value) {
        return (value<10?std::wstring(L"0"):std::wstring())+
               std::to_wstring(value);
    };
    if(seconds>=3600u)
        return std::to_wstring(seconds/3600u)+L":"+
               pad((seconds/60u)%60u)+L":"+pad(seconds%60u);
    return pad(seconds/60u)+L":"+pad(seconds%60u);
}
}

std::wstring WimOpenProgress::headline() const {
    if(cancelled_.load())return L"Stopping safely...";
    if(mode_!=Mode::Opening) {
        std::lock_guard lock(edit_mutex_);
        if(!edit_stage_.empty())return edit_stage_;
        return mode_==Mode::Deleting?L"Preparing WIM deletion":
                                     L"Preparing WIM update";
    }
    if(publishing_.load())return L"Preparing the file list";
    const auto index=image_.load(),count=image_count_.load();
    if(!index)return L"Opening Windows image";
    return L"Reading image "+std::to_wstring(index)+
        (count?L" of "+std::to_wstring(count):std::wstring());
}

std::wstring WimOpenProgress::describe() const {
    std::wstring result;
    if(mode_!=Mode::Opening) {
        result=L"Checkpoint  "+std::to_wstring(edit_percent_.load())+L"%";
    } else if(publishing_.load()) {
        const auto total=total_entries_.load();
        const auto done=std::min(published_.load(),total);
        result=L"Entries  "+std::to_wstring(done)+L" / "+std::to_wstring(total);
    } else {
        result=L"Entries found  "+std::to_wstring(entries_.load());
    }
    result+=L"    |    Elapsed  "+elapsed_label(GetTickCount64()-started_);
    // Keep the same number of lines during long API calls so the layout
    // and Cancel button do not jump while the user is trying to click.
    if(cancelled_.load())
        result+=L"\n\nWaiting for Windows to reach a safe stopping point.";
    else if(mode_!=Mode::Opening)
        result+=L"\n\nWorking on this step. Large images can take several minutes.";
    else
        result+=L"\n\nReading directory information from the image.";
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
                if(self->mode_!=Mode::Opening || self->publishing_.load()) {
                    if(!self->progress_is_determinate_) {
                        SendMessageW(window,TDM_SET_MARQUEE_PROGRESS_BAR,FALSE,0);
                        SendMessageW(window,TDM_SET_PROGRESS_BAR_RANGE,0,
                                     MAKELPARAM(0,100));
                        self->progress_is_determinate_=true;
                    }
                    const auto total=self->total_entries_.load();
                    if(self->mode_!=Mode::Opening) {
                        SendMessageW(window,TDM_SET_PROGRESS_BAR_POS,
                                     self->edit_percent_.load(),0);
                    } else if(total) {
                        const auto done=self->published_.load();
                        const auto percent=static_cast<WPARAM>(
                            std::min<std::uint64_t>(100,done*100/total));
                        SendMessageW(window,TDM_SET_PROGRESS_BAR_POS,percent,0);
                    }
                }
                const auto heading=self->headline();
                if(heading!=self->last_headline_) {
                    self->last_headline_=heading;
                    SendMessageW(window,TDM_SET_ELEMENT_TEXT,TDE_MAIN_INSTRUCTION,
                                 reinterpret_cast<LPARAM>(heading.c_str()));
                }
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
                SendMessageW(window,TDM_SET_PROGRESS_BAR_STATE,PBST_PAUSED,0);
                const auto heading=self->headline();
                SendMessageW(window,TDM_SET_ELEMENT_TEXT,TDE_MAIN_INSTRUCTION,
                             reinterpret_cast<LPARAM>(heading.c_str()));
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
            if (wake_.wait_for(lock,std::chrono::milliseconds(
                    mode_!=Mode::Opening?700:1200),
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
        config.cxWidth=360;
        config.pszMainIcon=TD_INFORMATION_ICON;
        config.pszWindowTitle=mode_==Mode::Deleting?
            L"ISO-WCX | Deleting from WIM":mode_==Mode::Editing?
            L"ISO-WCX | Updating WIM":L"ISO-WCX | Opening WIM";
        const auto initial_heading=headline();
        const auto initial_content=describe();
        config.pszMainInstruction=initial_heading.c_str();
        config.pszContent=initial_content.c_str();
        config.pszFooter=mode_!=Mode::Opening?
            L"Original image protected until verification completes.":
            L"Read-only operation. Your image stays unchanged.";
        config.pszExpandedControlText=L"Details";
        config.pszCollapsedControlText=L"Hide details";
        config.pszExpandedInformation=mode_!=Mode::Opening?
            L"Changes are prepared in a private working copy and verified "
            L"before replacing the original image.\n\n"
            L"The bar shows reported operation checkpoints, not a time estimate. "
            L"It can pause while Windows processes a large image. Cancel takes "
            L"effect at the next safe Windows Imaging API checkpoint.":
            L"Only directory metadata is listed. File contents are not extracted "
            L"or hashed. The bar becomes determinate when the entry count is known.";
        config.pfCallback=&WimOpenProgress::dialog_callback;
        config.lpCallbackData=reinterpret_cast<LONG_PTR>(this);
        // Defer loading ComCtl32 until an actual Total Commander UI is
        // required. A static TaskDialogIndirect import makes EVERY WCX
        // LoadLibrary pull GUI dependencies into command-line readers and
        // headless CI processes, where it can deadlock under loader lock.
        // The native WCX must remain loadable without a desktop session.
        using TaskDialogIndirectFn=HRESULT (WINAPI*)(
            const TASKDIALOGCONFIG*,int*,int*,BOOL*);
        const HMODULE controls=LoadLibraryExW(
            L"comctl32.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!controls)return;
        const auto open_dialog=reinterpret_cast<TaskDialogIndirectFn>(
            GetProcAddress(controls,"TaskDialogIndirect"));
        if(open_dialog) {
            (void)open_dialog(&config,nullptr,nullptr,nullptr);
        }
        FreeLibrary(controls);
    } catch (...) {
        // Closing the indicator cannot invalidate a successful WIM read.
    }
}
} // namespace iso_wcx
#endif
