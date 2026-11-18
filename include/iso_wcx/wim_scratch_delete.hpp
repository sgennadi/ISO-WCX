#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>

namespace iso_wcx::wim_scratch {

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
