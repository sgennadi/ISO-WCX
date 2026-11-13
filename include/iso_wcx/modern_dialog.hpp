#pragma once
// Native Windows TaskDialog interface for ISO-WCX user-facing prompts.
// Shared by updater, diagnostics and Total Commander association repair.
#ifdef _WIN32
#include <windows.h>
#include <string_view>

namespace iso_wcx::modern_dialog {
enum class Tone { information, success, warning, error, question };

// Returns true only when the explicit primary action is selected.
// Escape, closing the window and all unavailable-dialog paths are safe
// (cancel) by default. A legacy MessageBox is a compatibility fallback.
[[nodiscard]] bool confirm(
    HWND parent,Tone tone,std::wstring_view title,
    std::wstring_view heading,std::wstring_view explanation,
    std::wstring_view primary,std::wstring_view secondary,
    std::wstring_view details={}) noexcept;

// A one-button, informational or error dialog with optional expandable detail.
void notice(
    HWND parent,Tone tone,std::wstring_view title,
    std::wstring_view heading,std::wstring_view explanation,
    std::wstring_view details={}) noexcept;

// Drop-in adapter used to modernize the existing plugin warnings and
// confirmations without altering their YES/NO/OK control-flow contracts.
int show(HWND parent,const wchar_t* message,
                       const wchar_t* title,unsigned flags) noexcept;
} // namespace iso_wcx::modern_dialog
#endif
