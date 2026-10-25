#pragma once
#include <filesystem>
#include <string>

namespace iso_wcx {
// Pure read-only metadata report. It never opens the source for writing,
// reads file payloads for export, mounts, or invokes external executables.
// UTF-8 text without BOM (the Windows save UI adds UTF-8 BOM for Notepad).
[[nodiscard]] std::string build_image_structure_report(
    const std::filesystem::path& image);
}
