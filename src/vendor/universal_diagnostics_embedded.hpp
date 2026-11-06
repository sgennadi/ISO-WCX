#pragma once
// UniversalDiagnostics v1 embedded C++ API.
// Canonical source: sgennadi/UniversalDiagnocticsModule/embedded/cpp/
// Privacy-first local queue, no HTTP/GitHub, process spawn or external EXE.
#include <string>
#include <string_view>

namespace universal_diagnostics {
struct Receipt {
    bool success = false;
    std::string id;
    std::wstring message;
};

// Redacted metadata only. Caller must explicitly obtain user consent.
// Writes the same universal-diagnostics/v1 event/delivery record as core.py.
Receipt capture(std::string_view project, std::string_view component,
                std::string_view version, std::string_view error_type,
                std::string_view code, std::string_view summary) noexcept;

// Per-project queue folder for tests and for deliberate local inspection.
// Empty string means unsafe project/root.
std::wstring queue_directory(std::string_view project) noexcept;
} // namespace universal_diagnostics
