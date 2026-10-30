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
           "   IMPORTANT: DISM CAN BE EMPTY when a native WIMGAPI mount\n"
           "   still exists. The empty DISM list is NOT deletion permission.\n"
           "2. Inspect the EXACT mount directory above without writing:\n"
           "   fsutil reparsepoint query <quoted exact mount directory>\n"
           "   The plugin also checks WIMGetMountedImageInfo (native WIMGAPI).\n"
           "   A non-reparse directory alone does not prove unmounted.\n"
           "3. If DISM lists this exact mount AND the staged WIM path agrees,\n"
           "   only then consider DISM /Unmount-Image with /Discard.\n"
           "   If DISM is empty, do NOT blindly run DISM /Unmount-Image.\n"
           "4. Close Total Commander. For a locked private backup use\n"
           "   Windows Resource Monitor > CPU > Associated Handles,\n"
           "   searching for the private .iso-wcx-edit- workspace name.\n"
           "5. Remove a private workspace ONLY after a native WIMGAPI\n"
           "   mount-state check proves no active registration or reparse\n"
           "   point, and all file handles are released. Preserve otherwise.\n"
           "6. Revalidate the original WIM: an interrupted atomic rename\n"
           "   may already have installed the edited file.\n\n"
           "NEVER use global WIM_DELETE_MOUNTS_ALL, WIMDeleteImageMounts,\n"
           "or DISM /Cleanup-Mountpoints as automatic cleanup for this edit.\n"
           "No recovery command is executed automatically by ISO-WCX.\n";
}
} // namespace iso_wcx::wim_detail
