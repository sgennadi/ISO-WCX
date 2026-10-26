#pragma once
// Passive per-operation recovery instructions; never clean global mountpoints.
#include <filesystem>
#include <string>
namespace iso_wcx::wim_detail {
inline std::string mount_path_utf8(const std::filesystem::path& path) {
    const auto bytes=path.u8string();
    return {bytes.begin(),bytes.end()};
}
inline std::string make_mount_recovery_note(
        const std::filesystem::path& original,
        const std::filesystem::path& staged,
        const std::filesystem::path& mount,
        unsigned image_index) {
    return "ISO-WCX WIM MOUNT RECOVERY (UTF-8)\n"
           "Prepared BEFORE attempting to mount a PRIVATE WIM CLONE.\n"
           "Do not delete the working folder while its mount may be active.\n"
           "Original WIM: "+mount_path_utf8(original)+"\n"
           "Staged WIM: "+mount_path_utf8(staged)+"\n"
           "Mount directory: "+mount_path_utf8(mount)+"\n"
           "Image index: "+std::to_string(image_index)+"\n\n"
           "MANUAL ADMINISTRATOR RECOVERY:\n"
           "1. Run: dism /Get-MountedImageInfo\n"
           "2. Identify the EXACT mount directory listed above and verify\n"
           "   that the backing image equals the staged WIM above.\n"
           "3. Only for that mount, use DISM /Unmount-Image with\n"
           "   /MountDir:<quoted exact mount directory> /Discard.\n"
           "   A mount marked 'Needs Remount' may require DISM /Remount-Image.\n"
           "4. Inspect mounted images again. Remove the staged workspace\n"
           "   manually ONLY after its registered mount has disappeared.\n"
           "5. Revalidate the original WIM; a crash after the final atomic\n"
           "   replacement may already have installed the edited file.\n\n"
           "NEVER use global WIM_DELETE_MOUNTS_ALL, WIMDeleteImageMounts,\n"
           "or DISM /Cleanup-Mountpoints as automatic cleanup for this edit.\n"
           "No recovery command is executed automatically by ISO-WCX.\n";
}
} // namespace iso_wcx::wim_detail
