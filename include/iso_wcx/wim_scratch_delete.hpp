#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace iso_wcx::wim_scratch {

// Always use the literal Win32 file namespace for cleanup of the unique,
// private NTFS WIM workspace. WIMGAPI can apply filenames with trailing
// spaces/dots or very long names that FindFirstFileW enumerates, while the
// usual DOS name normalization makes GetFileAttributesW/DeleteFileW return
// ERROR_FILE_NOT_FOUND for those SAME directory entries. The parent then
// fails RemoveDirectoryW with ERROR_DIR_NOT_EMPTY (145).
//
// This helper never changes file contents, follows links, or grants access;
// the normal reparse-point and private-workspace checks remain mandatory.
// Do not apply it to source WIM paths, arbitrary TC selections, or WIMMount.
inline std::filesystem::path literal_private_ntfs_path(
    const std::filesystem::path& entry) {
    const auto full=std::filesystem::absolute(entry);
    const std::wstring native=full.wstring();
    if(native.starts_with(LR"(\\?\)"))
        return full;
    if(native.starts_with(LR"(\\.\)"))
        throw std::runtime_error("private WIM scratch must not be a device path");
    if(native.size()>=3 && native[1]==L':' && native[2]==L'\\')
        return std::filesystem::path(std::wstring(LR"(\\?\)")+native);
    if(native.starts_with(LR"(\\)"))
        return std::filesystem::path(
            std::wstring(LR"(\\?\UNC\)")+native.substr(2));
    throw std::runtime_error("private WIM scratch path is not fully qualified");
}

// This primitive is ONLY for an entry already verified to be inside the
// transaction's private WIM scratch directory. It never follows reparse
// points, never repairs ACLs, never touches WIMMount or the source image.
// POSIX disposition unlinks the directory entry immediately on NTFS even
// when another reader still holds a FILE_SHARE_DELETE handle. Windows can
// reject it for nonempty directories, unsupported filesystems or sharing
// restrictions; the caller must then try the normal Win32 deletion path.
inline bool try_posix_unlink_private_entry(
    const std::filesystem::path& entry,bool is_directory) noexcept {
    HANDLE handle=CreateFileW(entry.c_str(),
        DELETE|FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if(handle==INVALID_HANDLE_VALUE)return false;

    BY_HANDLE_FILE_INFORMATION info{};
    const bool matches=GetFileInformationByHandle(handle,&info) &&
        ((info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0)==is_directory;
    bool removed=false;
    if(matches) {
        FILE_DISPOSITION_INFO_EX disposition{};
        disposition.Flags=FILE_DISPOSITION_FLAG_DELETE|
            FILE_DISPOSITION_FLAG_POSIX_SEMANTICS|
            FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE;
        removed=SetFileInformationByHandle(handle,FileDispositionInfoEx,
                                           &disposition,sizeof(disposition))!=FALSE;
        if(!removed && GetLastError()==ERROR_INVALID_PARAMETER) {
            // Earlier Windows builds do not support IGNORE_READONLY_ATTRIBUTE.
            disposition.Flags=FILE_DISPOSITION_FLAG_DELETE|
                FILE_DISPOSITION_FLAG_POSIX_SEMANTICS;
            removed=SetFileInformationByHandle(handle,FileDispositionInfoEx,
                                               &disposition,sizeof(disposition))!=FALSE;
        }
    }
    CloseHandle(handle);
    return removed;
}
} // namespace iso_wcx::wim_scratch
#endif
