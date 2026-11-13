#pragma once
#ifdef _WIN32
#include <filesystem>

namespace iso_wcx {
// Reports exactly which TC WCX associations are active. Read-only: the
// plugin cannot rewrite wincmd.ini while Total Commander is running.
[[nodiscard]] std::filesystem::path export_tc_registration_audit() noexcept;
} // namespace iso_wcx
#endif
