// TaskDialogIndirect is resolved at runtime from Windows Common Controls.
// It renders the OS's modern task dialog with native DPI/text sizing and
// keyboard navigation; we never deploy extra DLLs or graphical resources.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "iso_wcx/modern_dialog.hpp"
#include <commctrl.h>
#include <string>

namespace iso_wcx::modern_dialog {
namespace {
using TaskDialogProc=HRESULT (WINAPI*)(
    const TASKDIALOGCONFIG*,int*,int*,BOOL*);

PCWSTR icon(Tone tone) noexcept {
    switch(tone){
    case Tone::warning: return TD_WARNING_ICON;
    case Tone::error: return TD_ERROR_ICON;
    case Tone::question: return TD_INFORMATION_ICON;
    case Tone::success: return TD_INFORMATION_ICON;
    default: return TD_INFORMATION_ICON;
    }
}
UINT messagebox_icon(Tone tone) noexcept {
    switch(tone){
    case Tone::warning: return MB_ICONWARNING;
    case Tone::error: return MB_ICONERROR;
    case Tone::question: return MB_ICONQUESTION;
    default: return MB_ICONINFORMATION;
    }
}
int render(HWND parent,Tone tone,std::wstring_view caption,
           std::wstring_view instruction,std::wstring_view text,
           std::wstring_view primary,std::wstring_view secondary,
           std::wstring_view details,bool ask) noexcept {
    try {
        const std::wstring title(caption),heading(instruction),body(text);
        const std::wstring accept(primary),decline(secondary),extra(details);
        // TaskDialogIndirect is in comctl32 v6. Total Commander normally
        // activates visual styles. Dynamically resolve so older hosts still
        // load the WCX and never get a missing-export load failure.
        HMODULE dll=GetModuleHandleW(L"comctl32.dll");
        const bool loaded=dll==nullptr;
        if(!dll)dll=LoadLibraryW(L"comctl32.dll");
        if(dll){
            const auto proc=reinterpret_cast<TaskDialogProc>(
                GetProcAddress(dll,"TaskDialogIndirect"));
            if(proc){
                TASKDIALOGCONFIG cfg{};
                cfg.cbSize=sizeof(cfg);
                cfg.hwndParent=parent;
                cfg.dwFlags=static_cast<TASKDIALOG_FLAGS>(
                    TDF_ALLOW_DIALOG_CANCELLATION|TDF_SIZE_TO_CONTENT|
                    (parent?TDF_POSITION_RELATIVE_TO_WINDOW:0));
                cfg.pszWindowTitle=title.c_str();
                cfg.pszMainInstruction=heading.c_str();
                cfg.pszContent=body.c_str();
                cfg.pszMainIcon=icon(tone);
                if(!extra.empty())cfg.pszExpandedInformation=extra.c_str();
                TASKDIALOG_BUTTON choices[2]{};
                if(ask){
                    choices[0]={IDYES,accept.c_str()};
                    choices[1]={IDNO,decline.c_str()};
                    cfg.cButtons=2;
                    cfg.pButtons=choices;
                    cfg.nDefaultButton=IDNO; // no accidental destructive action
                    cfg.dwFlags=static_cast<TASKDIALOG_FLAGS>(
                        cfg.dwFlags|TDF_USE_COMMAND_LINKS);
                }else{
                    cfg.dwCommonButtons=TDCBF_OK_BUTTON;
                    cfg.nDefaultButton=IDOK;
                }
                int clicked=IDCANCEL;
                const HRESULT hr=proc(&cfg,&clicked,nullptr,nullptr);
                if(loaded)FreeLibrary(dll);
                if(SUCCEEDED(hr))return clicked;
            }else if(loaded)FreeLibrary(dll);
        }
        // Compatibility path for hosts with legacy comctl32 activation.
        // In the confirmation case the safe answer is the default button.
        std::wstring legacy=heading;
        if(!body.empty())legacy+=L"\n\n"+body;
        if(!extra.empty())legacy+=L"\n\n"+extra;
        const UINT buttons=ask?(MB_YESNO|MB_DEFBUTTON2):MB_OK;
        return MessageBoxW(parent,legacy.c_str(),title.c_str(),
                           buttons|messagebox_icon(tone));
    }catch(...){
        // Never throw through a WCX C ABI entrypoint or a callback.
        if(!ask)MessageBoxW(parent,L"ISO-WCX",L"ISO-WCX",MB_OK|MB_ICONERROR);
        return IDCANCEL;
    }
}
} // namespace

bool confirm(HWND parent,Tone tone,std::wstring_view title,
             std::wstring_view heading,std::wstring_view explanation,
             std::wstring_view primary,std::wstring_view secondary,
             std::wstring_view details) noexcept {
    return render(parent,tone,title,heading,explanation,primary,secondary,
                  details,true)==IDYES;
}
void notice(HWND parent,Tone tone,std::wstring_view title,
            std::wstring_view heading,std::wstring_view explanation,
            std::wstring_view details) noexcept {
    (void)render(parent,tone,title,heading,explanation,{},{},details,false);
}
int show(HWND parent,const wchar_t* message,
         const wchar_t* title,unsigned flags) noexcept {
    const Tone tone=(flags&MB_ICONMASK)==MB_ICONERROR?Tone::error:
        (flags&MB_ICONMASK)==MB_ICONWARNING?Tone::warning:
        (flags&MB_ICONMASK)==MB_ICONQUESTION?Tone::question:
        Tone::information;
    const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
    const auto caption=title?title:L"ISO-WCX";
    const auto body=message?message:L"";
    if((flags&MB_TYPEMASK)==MB_YESNO) {
        return confirm(parent,tone,caption,
            ru?L"Требуется подтверждение":L"Confirmation required",
            body,ru?L"Продолжить":L"Continue",
            ru?L"Отмена":L"Cancel")?IDYES:IDNO;
    }
    notice(parent,tone,caption,
        tone==Tone::error?(ru?L"Не удалось выполнить действие":L"Action failed"):
        tone==Tone::warning?(ru?L"Предупреждение":L"Attention"):
        (ru?L"ISO-WCX":L"ISO-WCX"),
        body);
    return IDOK;
}
} // namespace iso_wcx::modern_dialog
#endif
