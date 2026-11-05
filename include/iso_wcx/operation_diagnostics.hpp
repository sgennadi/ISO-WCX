#pragma once
// Lightweight, privacy-preserving WCX diagnostics. All functions are noexcept:
// file operation results must never depend on log I/O or telemetry availability.
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
[[nodiscard]] bool sending_enabled() noexcept;
void set_sending_enabled(bool enabled) noexcept;
[[nodiscard]] std::wstring endpoint() noexcept;
[[nodiscard]] bool set_endpoint(std::wstring_view https_url) noexcept;
[[nodiscard]] std::filesystem::path log_directory() noexcept;
void request_upload(bool force=false) noexcept;
void clear_pending() noexcept;
[[nodiscard]] std::wstring upload_status() noexcept;
} // namespace iso_wcx::diagnostics
#endif
