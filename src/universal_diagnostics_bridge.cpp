#ifdef _WIN32
#include "iso_wcx/universal_diagnostics.hpp"
#include "vendor/universal_diagnostics_embedded.hpp"

#include <string>
#include <string_view>

namespace iso_wcx::universal_diagnostics {

// The shared core is linked into ISO-WCX.wcx and ISO-WCX.wcx64.
// WCX operations and update checks never call this function; it is reached
// only after explicit operator review in the Settings window.
bool capture_local(Category category, int major, int minor, int patch,
                   std::wstring& status) noexcept {
    try {
        const auto version = std::to_string(major) + "." +
                             std::to_string(minor) + "." +
                             std::to_string(patch);
        const auto saved = ::universal_diagnostics::capture(
            "sgennadi/ISO-WCX", component(category), version,
            "USER_REPORT", "MANUAL",
            "User-selected ISO-WCX operation category; no attachments.");
        status = saved.message;
        if(saved.success) {
            status += L" Report ID: ";
            status.append(saved.id.begin(), saved.id.end());
        }
        return saved.success;
    } catch(...) {
        status = L"Embedded diagnostics failed. No upload was attempted.";
        return false;
    }
}

} // namespace iso_wcx::universal_diagnostics
#endif
