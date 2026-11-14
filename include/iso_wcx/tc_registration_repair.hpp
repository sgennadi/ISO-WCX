#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>
#include <string>
#include <string_view>

namespace iso_wcx {
struct AssociationFixPlan {
    std::filesystem::path ini;
    std::wstring summary;
    unsigned changes=0;
    unsigned conflicts=0;
    bool legacy_iso_available=false;
};

// Read-only preview of primary image associations. Preview builds additionally
// include read-only FDI/HDI/NHD without adding them to Stable registrations.
// Uses the same standalone core as the offline worker and unit tests.
AssociationFixPlan preview_association_fix(
    const std::filesystem::path& ini, const std::filesystem::path& plugin32,
    bool replace_legacy_iso=false);

// This API deliberately fails while any Total Commander process is running.
// An unmodified byte-for-byte backup is created before an atomic INI swap.
bool apply_association_fix_offline(
    const std::filesystem::path& ini, const std::filesystem::path& plugin32,
    std::wstring& summary, bool replace_legacy_iso=false);

// Called from ConfigurePacker. Starts an OS-provided rundll32 worker using
// this very WCX DLL, which waits until Total Commander is fully closed.
// Neither scripts nor an auxiliary EXE/DLL are distributed.
bool offer_association_fix(HWND parent, HINSTANCE plugin, bool russian,
                           std::wstring& status);

// The DLL is loaded by rundll32, which passes an *argument fragment*, not
// GetCommandLineW(). CommandLineToArgvW interprets a leading space as an
// empty argv[0], so parse a synthetic program name first. The parser is
// independently regression-tested using real Windows argv semantics.
struct AssociationWorkerArguments {
    std::filesystem::path ini;
    std::filesystem::path plugin32;
    bool replace_legacy_iso=false;
};
AssociationWorkerArguments parse_association_worker_arguments(std::wstring_view);

// Called only by the exported rundll32 entrypoint, not during normal WCX I/O.
void run_association_fix_worker(HINSTANCE plugin, std::wstring_view arguments) noexcept;
} // namespace iso_wcx
#endif
