#pragma once
// Temporary local WCX operation journal until UniversalDiagnosticsModule
// integration. Never blocks file I/O, includes file paths or sends silently.
#ifdef _WIN32
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace iso_wcx::diagnostics {
class Operation final {
public:
    Operation(const char* action, const std::filesystem::path& image,
              unsigned file_count=0, unsigned image_index=0) noexcept;
    Operation(const Operation&)=delete;
    Operation& operator=(const Operation&)=delete;
    ~Operation() noexcept;
    void set_action(const char* action) noexcept; // directory pack -> mkdir
    void stage(std::wstring_view stage, unsigned percentage=0) noexcept;
    void complete(int wcx_code, std::string_view reason={}) noexcept;
private:
    std::string action_;
    std::string format_;
    std::string stage_;
    std::uint64_t id_=0, start_=0;
    unsigned count_=0, image_index_=0, last_stage_pct_=0;
    bool completed_=false;
};
// Logs: %LOCALAPPDATA%\ISO-WCX\Logs\operations.jsonl (rotated).
// User-controlled GitHub browser flow: no embedded GitHub token, and no
// unattended transmission of diagnostic data.
[[nodiscard]] std::filesystem::path log_directory() noexcept;
[[nodiscard]] std::filesystem::path export_report() noexcept;
[[nodiscard]] bool open_github_issue() noexcept;
[[nodiscard]] std::wstring status() noexcept;
} // namespace iso_wcx::diagnostics
#endif
