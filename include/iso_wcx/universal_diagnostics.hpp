#pragma once

#include <string>
#include <string_view>

namespace iso_wcx::universal_diagnostics {

// Shared UniversalDiagnostics capture-json schema v1.
// Only a finite operator-selected category is transported: never include
// image paths, archive entries, account data, raw logs, or user identifiers.
enum class Category {
    iso_write,
    wim_write,
    udf_edit,
    mount_commit,
    read_extract,
    other
};

inline const char* component(Category category) {
    switch (category) {
    case Category::iso_write: return "ISO F5 write";
    case Category::wim_write: return "WIM F5 write";
    case Category::udf_edit: return "ISO UDF modify";
    case Category::mount_commit: return "WIM mount or commit";
    case Category::read_extract: return "Archive read or extract";
    default: return "ISO-WCX Settings";
    }
}
inline std::string payload(Category category, int major, int minor, int patch) {
    // The inputs are an enum and compile-time version components. Values
    // are guaranteed printable ASCII; no user-supplied JSON gets through.
    return std::string{"{\"project\":\"sgennadi/ISO-WCX\",\"component\":\""}+
           component(category)+
           "\",\"version\":\""+std::to_string(major)+"."+
           std::to_string(minor)+"."+std::to_string(patch)+
           "\",\"error_type\":\"USER_REPORT\",\"code\":\"MANUAL\","
           "\"summary\":\"User-selected ISO-WCX operation category; "
           "no image path, raw log or personal data included.\",\"level\":\"error\"}";
}

#ifdef _WIN32
// Invoked only from the explicit Settings button, never from WCX I/O
// callbacks. The canonical core is compiled inside WCX (no helper EXE).
bool capture_local(Category category, int major, int minor, int patch,
                   std::wstring& status) noexcept;
#endif

} // namespace iso_wcx::universal_diagnostics
