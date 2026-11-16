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
    if(const HWND dialog=dialog_.load(std::memory_order_acquire)) {
        // Legacy fallback owns a normal window; TaskDialog uses a private
        // cancellation message. In both cases the stop event means *close*,
        // not a new user cancellation request.
        if(fallback_window_.load(std::memory_order_acquire))
            PostMessageW(dialog,WM_CLOSE,0,0);
        else
            PostMessageW(dialog,TDM_CLICK_BUTTON,IDCANCEL,0);
    }
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

LRESULT CALLBACK WimOpenIndicator::fallback_proc(
    HWND window,UINT message,WPARAM wparam,LPARAM lparam) noexcept {
    if(message==WM_NCCREATE) {
        const auto* create=reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window,GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    auto* self=reinterpret_cast<WimOpenIndicator*>(
        GetWindowLongPtrW(window,GWLP_USERDATA));
    if(!self)return DefWindowProcW(window,message,wparam,lparam);
    switch(message) {
    case WM_TIMER:
        if(stopped(self->stop_event_)) {
            DestroyWindow(window);
        }else{
            self->refresh(window);
            ++self->fallback_frame_;
            const UINT dpi=std::max(96u,GetDpiForSystem());
            const auto scale=[&](int value){
                return MulDiv(value,static_cast<int>(dpi),96);
            };
            RECT bar{scale(18),scale(129),scale(518),scale(148)};
            // Repaint just the animated track; large index labels and
            // the Cancel button must not flicker at 200% DPI.
            InvalidateRect(window,&bar,FALSE);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC dc=BeginPaint(window,&paint);
        if(dc) {
            // An indeterminate animation uses only USER32/GDI32. It works
            // even with legacy comctl32 v5, where PBS_MARQUEE is unavailable.
            const UINT dpi=std::max(96u,GetDpiForSystem());
            const auto scale=[&](int value){
                return MulDiv(value,static_cast<int>(dpi),96);
            };
            RECT outer{scale(18),scale(129),scale(518),scale(148)};
            FillRect(dc,&outer,GetSysColorBrush(COLOR_3DFACE));
            FrameRect(dc,&outer,GetSysColorBrush(COLOR_3DSHADOW));
            RECT inner{outer.left+1,outer.top+1,
                       outer.right-1,outer.bottom-1};
            const int width=inner.right-inner.left;
            const int block=std::max(1,width/4);
            const int travel=width-block;
            const unsigned tick=self->fallback_frame_%128u;
            const unsigned sweep=tick<64u?tick:127u-tick;
            const int offset=static_cast<int>(sweep)*travel/63;
            RECT moving{inner.left+offset,inner.top,
                        inner.left+offset+block,inner.bottom};
            FillRect(dc,&moving,GetSysColorBrush(COLOR_HIGHLIGHT));
        }
        EndPaint(window,&paint);
        return 0;
    }
    case WM_COMMAND:
        if(LOWORD(wparam)==IDCANCEL) {
            if(stopped(self->stop_event_)) {
                DestroyWindow(window);
            }else{
                self->cancelled_.store(true,std::memory_order_release);
                EnableWindow(GetDlgItem(window,IDCANCEL),FALSE);
                self->refresh(window);
            }
            return 0;
        }
        break;
    case WM_CLOSE:
        // Closing the window is Cancel, not an unsafe interruption of
        // WIMLoadImage. Keep the dialog until the native operation unwinds.
        if(stopped(self->stop_event_)) {
            DestroyWindow(window);
        }else{
            self->cancelled_.store(true,std::memory_order_release);
            EnableWindow(GetDlgItem(window,IDCANCEL),FALSE);
            self->refresh(window);
        }
        return 0;
    case WM_DESTROY:
        self->dialog_.store(nullptr,std::memory_order_release);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}

void WimOpenIndicator::run_fallback() noexcept {
    try {
        // Per-instance class names avoid stale WNDPROC pointers when
        // Total Commander unloads/reloads the WCX DLL.
        const HINSTANCE instance=GetModuleHandleW(nullptr);
        const std::wstring class_name=L"ISO_WCX_WimProgress_"+std::to_wstring(
            reinterpret_cast<std::uintptr_t>(this));
        WNDCLASSEXW klass{};
        klass.cbSize=sizeof(klass);
        klass.lpfnWndProc=&WimOpenIndicator::fallback_proc;
        klass.hInstance=instance;
        klass.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        klass.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
        klass.lpszClassName=class_name.c_str();
        if(!RegisterClassExW(&klass))return;

        const UINT dpi=std::max(96u,GetDpiForSystem());
        const auto scale=[&](int value){
            return MulDiv(value,static_cast<int>(dpi),96);
        };
        const int width=scale(540),height=scale(235);
        const int x=(GetSystemMetrics(SM_CXSCREEN)-width)/2;
        const int y=(GetSystemMetrics(SM_CYSCREEN)-height)/2;
        HWND window=CreateWindowExW(WS_EX_DLGMODALFRAME,
            class_name.c_str(),title_.c_str(),
            WS_CAPTION|WS_SYSMENU,
            x,y,width,height,nullptr,nullptr,instance,this);
        if(!window) {
            UnregisterClassW(class_name.c_str(),instance);
            return;
        }
        const HWND heading=CreateWindowExW(0,L"STATIC",
            heading_.c_str(),WS_CHILD|WS_VISIBLE|SS_LEFT,
            scale(18),scale(16),scale(500),scale(42),
            window,reinterpret_cast<HMENU>(1000),instance,nullptr);
        const HWND status=CreateWindowExW(0,L"STATIC",
            status_text().c_str(),WS_CHILD|WS_VISIBLE|SS_LEFT,
            scale(18),scale(66),scale(500),scale(58),
            window,reinterpret_cast<HMENU>(1001),instance,nullptr);
        const HWND cancel=CreateWindowExW(0,L"BUTTON",
            russian_?L"Отмена":L"Cancel",
            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,
            scale(408),scale(159),scale(110),scale(32),
            window,reinterpret_cast<HMENU>(IDCANCEL),instance,nullptr);
        const HFONT font=CreateFontW(-MulDiv(9,static_cast<int>(dpi),72),
            0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,
            DEFAULT_PITCH,L"Segoe UI");
        if(font) {
            for(HWND child:{heading,status,cancel}) {
                if(child)SendMessageW(child,WM_SETFONT,
                                     reinterpret_cast<WPARAM>(font),TRUE);
            }
        }
        // Publish kind before HWND so shutdown never sends a TaskDialog
        // message to the fallback window.
        fallback_window_.store(true,std::memory_order_release);
        dialog_.store(window,std::memory_order_release);
        refresh(window);
        if(stopped(stop_event_)) {
            DestroyWindow(window);
        }else{
            SetTimer(window,1u,150,nullptr);
            ShowWindow(window,SW_SHOW);
            UpdateWindow(window);
            MSG msg{};
            while(GetMessageW(&msg,nullptr,0,0)>0) {
                if(!IsDialogMessageW(window,&msg)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
        }
        if(IsWindow(window))DestroyWindow(window);
        dialog_.store(nullptr,std::memory_order_release);
        fallback_window_.store(false,std::memory_order_release);
        if(font)DeleteObject(font);
        UnregisterClassW(class_name.c_str(),instance);
    }catch(...) {
        // The indicator is optional. Never fail a valid WIM open because
        // the GUI cannot allocate a window or create a font.
    }
}

void WimOpenIndicator::run() noexcept {
    try {
        // Fast WIM opens should not flash any dialog. Slow WIM loads show
        // a real indeterminate indicator, regardless of comctl32 version.
        if(WaitForSingleObject(stop_event_,450)!=WAIT_TIMEOUT)return;
        HMODULE controls=GetModuleHandleW(L"comctl32.dll");
        const bool loaded=controls==nullptr;
        if(!controls)controls=LoadLibraryW(L"comctl32.dll");
        bool handled=false;
        if(controls) {
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
                if(SUCCEEDED(result)) {
                    handled=true;
                    if(clicked==IDCANCEL && !stopped(stop_event_))
                        cancelled_.store(true,std::memory_order_release);
                }
            }
        }
        if(loaded && controls)FreeLibrary(controls);
        // Some TC configurations load comctl32 v5; TaskDialogIndirect then
        // does not exist. A failed v6 activation used to leave the user
        // with a blank screen for several minutes on a large install.wim.
        if(!handled && !stopped(stop_event_))run_fallback();
    }catch(...) {
        // GUI failure never becomes a WIM read failure or cancellation.
    }
}
} // namespace iso_wcx
#endif
