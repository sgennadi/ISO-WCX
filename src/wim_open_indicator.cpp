// Native WIM-open progress. WCX's per-handle progress callbacks are not
// registered until OpenArchive returns, so they cannot cover WIMLoadImage
// or metadata enumeration. A separate UI thread keeps the indicator
// responsive even while the calling TC thread is inside WIMGAPI.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "iso_wcx/wim_open_indicator.hpp"
#include <commctrl.h>
#include <algorithm>
#include <cwctype>
#include <utility>

namespace iso_wcx {
namespace {
using TaskDialogProc = HRESULT (WINAPI*)(
    const TASKDIALOGCONFIG*, int*, int*, BOOL*);

bool is_wim_path(const std::filesystem::path& image) {
    auto ext=image.extension().wstring();
    std::transform(ext.begin(),ext.end(),ext.begin(),
        [](wchar_t c){return static_cast<wchar_t>(std::towlower(c));});
    return ext==L".wim" || ext==L".esd" || ext==L".swm";
}
bool stopped(HANDLE event) noexcept {
    return event && WaitForSingleObject(event,0)==WAIT_OBJECT_0;
}
} // namespace

WimOpenIndicator::WimOpenIndicator(
    const std::filesystem::path& image) noexcept {
    try {
        if(!is_wim_path(image))return;
        russian_=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
        title_=L"ISO-WCX v"+
            std::to_wstring(ISO_WCX_VERSION_MAJOR)+L"."+
            std::to_wstring(ISO_WCX_VERSION_MINOR)+L"."+
            std::to_wstring(ISO_WCX_VERSION_PATCH);
#if ISO_WCX_RC_NUMBER > 0
        title_+=L" RC"+std::to_wstring(ISO_WCX_RC_NUMBER);
#endif
#if ISO_WCX_BUILD_NUMBER > 0
        title_+=L" B"+std::to_wstring(ISO_WCX_BUILD_NUMBER);
#endif
        title_+=russian_?L" | Открытие WIM":L" | Opening WIM";
        auto filename=image.filename().wstring();
        if(filename.size()>88)filename=filename.substr(0,85)+L"...";
        heading_=(russian_?L"Открытие образа ":L"Opening image ")+filename;
        stop_event_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!stop_event_)return;
        thread_=std::thread([this]{run();});
        active_=true;
    }catch(...) {
        if(stop_event_) {
            CloseHandle(stop_event_);
            stop_event_=nullptr;
        }
    }
}

WimOpenIndicator::~WimOpenIndicator() noexcept {
    if(stop_event_)SetEvent(stop_event_);
    if(const HWND dialog=dialog_.load(std::memory_order_acquire))
        PostMessageW(dialog,TDM_CLICK_BUTTON,IDCANCEL,0);
    if(thread_.joinable())thread_.join();
    if(stop_event_)CloseHandle(stop_event_);
}

bool WimOpenIndicator::report(const ArchiveOpenStatus& status) noexcept {
    if(!active_)return true; // GUI unavailable: never block reading.
    scanned_.store(status.entries_scanned,std::memory_order_relaxed);
    index_.store(status.image_index,std::memory_order_relaxed);
    total_.store(status.image_count,std::memory_order_relaxed);
    phase_.store(static_cast<unsigned>(status.phase),
                 std::memory_order_relaxed);
    return !cancelled_.load(std::memory_order_acquire);
}

std::wstring WimOpenIndicator::status_text() const {
    if(cancelled_.load(std::memory_order_acquire))
        return russian_
            ?L"Отмена запрошена. Ожидание завершения операции WIMGAPI..."
            :L"Cancelling... waiting for the current Windows Imaging API call.";
    const auto phase=static_cast<ArchiveOpenStatus::Phase>(
        phase_.load(std::memory_order_relaxed));
    const auto index=index_.load(std::memory_order_relaxed);
    const auto total=total_.load(std::memory_order_relaxed);
    const auto entries=scanned_.load(std::memory_order_relaxed);
    std::wstring result;
    switch(phase) {
    case ArchiveOpenStatus::Phase::opening:
        result=russian_?L"Чтение заголовка и частей образа..."
                       :L"Reading image header and split parts...";
        break;
    case ArchiveOpenStatus::Phase::loading_index:
        result=russian_?L"Загрузка метаданных WIM":L"Loading WIM metadata";
        break;
    case ArchiveOpenStatus::Phase::enumerating:
        result=russian_?L"Сканирование файлов WIM":L"Enumerating WIM files";
        break;
    case ArchiveOpenStatus::Phase::complete:
        result=russian_?L"Сканирование завершено":L"Enumeration complete";
        break;
    }
    if(total) {
        result+=L"\n";
        result+=russian_?L"Индекс ":L"Image ";
        result+=std::to_wstring(index);
        result+=L" / "+std::to_wstring(total);
    }
    if(entries) {
        result+=L"\n";
        result+=russian_?L"Обработано элементов: ":L"Entries scanned: ";
        result+=std::to_wstring(entries);
    }
    return result;
}

void WimOpenIndicator::refresh(HWND dialog) noexcept {
    try {
        const auto text=status_text();
        if(text==last_text_)return;
        last_text_=text;
        SendMessageW(dialog,TDM_SET_ELEMENT_TEXT,
                     TDE_CONTENT,reinterpret_cast<LPARAM>(text.c_str()));
    }catch(...) {
        // A progress label must never interrupt WIMGAPI or WCX.
    }
}

HRESULT CALLBACK WimOpenIndicator::task_callback(
    HWND dialog,UINT message,WPARAM wparam,
    LPARAM,LONG_PTR context) noexcept {
    auto* self=reinterpret_cast<WimOpenIndicator*>(context);
    if(!self)return S_OK;
    switch(message) {
    case TDN_CREATED:
        self->dialog_.store(dialog,std::memory_order_release);
        SendMessageW(dialog,TDM_SET_PROGRESS_BAR_MARQUEE,TRUE,50);
        self->refresh(dialog);
        if(stopped(self->stop_event_))
            PostMessageW(dialog,TDM_CLICK_BUTTON,IDCANCEL,0);
        break;
    case TDN_TIMER:
        if(stopped(self->stop_event_))
            PostMessageW(dialog,TDM_CLICK_BUTTON,IDCANCEL,0);
        else
            self->refresh(dialog);
        break;
    case TDN_BUTTON_CLICKED:
        if(static_cast<int>(wparam)==IDCANCEL) {
            if(stopped(self->stop_event_))return S_OK;
            // WIMLoadImage/WIMFindNextImageFile cannot be forcibly
            // interrupted. Keep the dialog visible until the next safe
            // progress callback unwinds the native WIMGAPI handles.
            self->cancelled_.store(true,std::memory_order_release);
            self->refresh(dialog);
            return S_FALSE;
        }
        return S_FALSE;
    case TDN_DESTROYED:
        self->dialog_.store(nullptr,std::memory_order_release);
        break;
    }
    return S_OK;
}

void WimOpenIndicator::run() noexcept {
    try {
        // Short WIM opens must not flash a modal progress dialog.
        // An actual large WIM gets a visible marquee after 450 ms.
        if(WaitForSingleObject(stop_event_,450)!=WAIT_TIMEOUT)return;
        HMODULE controls=GetModuleHandleW(L"comctl32.dll");
        const bool loaded=controls==nullptr;
        if(!controls)controls=LoadLibraryW(L"comctl32.dll");
        if(!controls)return;
        const auto proc=reinterpret_cast<TaskDialogProc>(
            GetProcAddress(controls,"TaskDialogIndirect"));
        if(proc) {
            TASKDIALOGCONFIG cfg{};
            cfg.cbSize=sizeof(cfg);
            cfg.dwFlags=static_cast<TASKDIALOG_FLAGS>(
                TDF_SHOW_MARQUEE_PROGRESS_BAR|
                TDF_CALLBACK_TIMER|
                TDF_ALLOW_DIALOG_CANCELLATION|
                TDF_SIZE_TO_CONTENT);
            cfg.dwCommonButtons=TDCBF_CANCEL_BUTTON;
            cfg.pszWindowTitle=title_.c_str();
            cfg.pszMainInstruction=heading_.c_str();
            cfg.pszContent=russian_?L"Загрузка метаданных..."
                                   :L"Loading image metadata...";
            cfg.pszFooter=russian_
                ?L"Образ не монтируется. Большие WIM могут открываться несколько минут."
                :L"The image is not mounted. Large WIM files may take several minutes.";
            cfg.pfCallback=&WimOpenIndicator::task_callback;
            cfg.lpCallbackData=reinterpret_cast<LONG_PTR>(this);
            int clicked=0;
            const HRESULT result=proc(&cfg,&clicked,nullptr,nullptr);
            // A missing visual-styles activation context may cause
            // TaskDialogIndirect to fail. That is a GUI failure, NOT a
            // request to cancel a valid WIM open. Only a successfully
            // dismissed Cancel dialog can request cancellation.
            if(SUCCEEDED(result) && clicked==IDCANCEL &&
               !stopped(stop_event_))
                cancelled_.store(true,std::memory_order_release);
        }
        if(loaded)FreeLibrary(controls);
    }catch(...) {
        // GUI failure never becomes an archive-open failure.
    }
}
} // namespace iso_wcx
#endif
