#pragma once
#include <filesystem>
#include <string>
namespace iso_wcx {
// Read-only WIM image-index metadata, similar to DISM /Get-WimInfo.
// Uses built-in Windows WIMGAPI and MSXML without mounting or external programs.
// The function is implemented only in Windows builds of ISO-WCX.
[[nodiscard]] std::string build_wim_index_summary(const std::filesystem::path& image);
}
