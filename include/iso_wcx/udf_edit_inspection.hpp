#pragma once
// Read-only UDF Type-1 writer preflight, for Export Image Structure.
// This never opens the image for writing, creates a mount, or starts DISM.
#include <filesystem>
#include <string>

namespace iso_wcx {
struct UdfEditInspection {
    bool parser_compatible = false;
    bool allocation_bitmap = false;
    std::string reason;
};
// allow_bridge only changes how the parser treats ISO9660 descriptors.
// A positive result never bypasses the separate protected El Torito/ISO
// staging validator, and it does not prove a particular file is writable.
[[nodiscard]] UdfEditInspection inspect_udf_edit(
    const std::filesystem::path& image, bool allow_bridge = false);
} // namespace iso_wcx
