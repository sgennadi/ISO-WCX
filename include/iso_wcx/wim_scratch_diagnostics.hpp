#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>
#include <string>
#include <system_error>
#include <algorithm>
#include "iso_wcx/wim_scratch_delete.hpp"

// Read-only and path-free diagnostics for a private ISO-WCX WIM scratch
// directory. Only the immediate children are inspected; no links are
// followed and no files or mount registry entries are changed.
namespace iso_wcx::wim_scratch {
namespace fs = std::filesystem;
struct Snapshot {
    unsigned survivors = 0, files = 0, folders = 0, reparse = 0;
    unsigned readonly = 0, delete_open_ok = 0, sharing = 0;
    unsigned access_denied = 0, delete_pending = 0;
    unsigned vanished = 0, other = 0;
    int scan_error = 0;

    std::string counts() const {
        return " (survivors_up_to_32=" + std::to_string(survivors) +
            ", files=" + std::to_string(files) +
            ", folders=" + std::to_string(folders) +
            ", reparse=" + std::to_string(reparse) +
            ", readonly=" + std::to_string(readonly) +
            ", delete_open_ok=" + std::to_string(delete_open_ok) +
            ", sharing=" + std::to_string(sharing) +
            ", access_denied=" + std::to_string(access_denied) +
            ", delete_pending=" + std::to_string(delete_pending) +
            ", vanished=" + std::to_string(vanished) +
            ", other=" + std::to_string(other) +
            ", scan_error=" + std::to_string(scan_error) + ")";
    }
};

inline Snapshot probe_directory(const fs::path& directory) noexcept {
    Snapshot state;
    try {
        std::error_code ec;
        auto it=fs::directory_iterator(directory,ec);
        const auto end=fs::directory_iterator{};
        while(!ec && it!=end && state.survivors<32u) {
            ++state.survivors;
            const auto path=it->path();
            const DWORD attr=GetFileAttributesW(path.c_str());
            if(attr==INVALID_FILE_ATTRIBUTES) {
                const DWORD error=GetLastError();
                if(error==ERROR_DELETE_PENDING)++state.delete_pending;
                else if(error==ERROR_FILE_NOT_FOUND ||
                        error==ERROR_PATH_NOT_FOUND)++state.vanished;
                else ++state.other;
            } else {
                if(attr&FILE_ATTRIBUTE_DIRECTORY)++state.folders;
                else ++state.files;
                if(attr&FILE_ATTRIBUTE_REPARSE_POINT)++state.reparse;
                if(attr&FILE_ATTRIBUTE_READONLY)++state.readonly;
                // Probe DELETE access without requesting a disposition.
                // This distinguishes an external sharing lock (32) from
                // ACL denial (5) and an outstanding delete-pending name
                // (303). OPEN_REPARSE_POINT prevents following links.
                HANDLE h=CreateFileW(path.c_str(),
                    DELETE|FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                    nullptr,OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,
                    nullptr);
                if(h==INVALID_HANDLE_VALUE) {
                    const DWORD error=GetLastError();
                    if(error==ERROR_SHARING_VIOLATION ||
                       error==ERROR_LOCK_VIOLATION)++state.sharing;
                    else if(error==ERROR_ACCESS_DENIED)++state.access_denied;
                    else if(error==ERROR_DELETE_PENDING)++state.delete_pending;
                    else if(error==ERROR_FILE_NOT_FOUND ||
                            error==ERROR_PATH_NOT_FOUND)++state.vanished;
                    else ++state.other;
                } else {
                    FILE_STANDARD_INFO info{};
                    if(GetFileInformationByHandleEx(h,FileStandardInfo,
                                                     &info,sizeof(info))) {
                        if(info.DeletePending)++state.delete_pending;
                        else ++state.delete_open_ok;
                    } else ++state.other;
                    CloseHandle(h);
                }
            }
            it.increment(ec);
        }
        state.scan_error=ec.value();
    } catch(...) {
        state.scan_error=ERROR_GEN_FAILURE;
    }
    return state;
}

// On NTFS, a FindFirstFile enumeration can report a name that has become
// unavailable by the time GetFileAttributesW opens it. At the same moment,
// RemoveDirectoryW may still report ERROR_DIR_NOT_EMPTY (145). This is NOT
// proof that the directory is gone. When every bounded probe entry vanished,
// close the probe iterator (above) and retry the exact private directory,
// accepting completion ONLY when Windows confirms the pathname is absent.
//
// Never sweep children here, do not follow reparse points, do not modify
// ACLs and do not touch the WIM source or any WIMMount registration.
inline bool retry_vanished_private_directory(
    const fs::path& directory,const Snapshot& probe,
    unsigned max_attempts=8) noexcept {
    if(probe.scan_error!=0 || probe.survivors==0 ||
       probe.survivors>=32u || probe.vanished!=probe.survivors ||
       probe.files || probe.folders || probe.reparse || probe.readonly ||
       probe.delete_open_ok || probe.sharing || probe.access_denied ||
       probe.delete_pending || probe.other ||
       max_attempts==0 || max_attempts>16)
        return false;
    const auto missing=[](const fs::path& value) noexcept {
        if(GetFileAttributesW(value.c_str())!=INVALID_FILE_ATTRIBUTES)
            return false;
        const DWORD error=GetLastError();
        return error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND;
    };
    for(unsigned pass=0;pass<max_attempts;++pass) {
        const DWORD attr=GetFileAttributesW(directory.c_str());
        if(attr==INVALID_FILE_ATTRIBUTES) {
            if(missing(directory))return true;
            const DWORD code=GetLastError();
            // The last close of a delete-sharing scan handle can leave a
            // transient DELETE_PENDING/ACCESS_DENIED pathname. Never
            // report success based on either code alone.
            if(code!=ERROR_DELETE_PENDING && code!=ERROR_ACCESS_DENIED)
                return false;
        } else {
            if(!(attr&FILE_ATTRIBUTE_DIRECTORY) ||
               (attr&FILE_ATTRIBUTE_REPARSE_POINT))
                return false;
            // POSIX disposition removes an empty directory name immediately
            // even when a FILE_SHARE_DELETE reader is still open. Legacy
            // RemoveDirectoryW is a safe fallback when POSIX is unavailable.
            if(try_posix_unlink_private_entry(directory,true) ||
               RemoveDirectoryW(directory.c_str())) {
                if(missing(directory))return true;
            }
            const DWORD code=GetLastError();
            if(code!=ERROR_DIR_NOT_EMPTY && code!=ERROR_DELETE_PENDING &&
               code!=ERROR_ACCESS_DENIED && code!=ERROR_SHARING_VIOLATION &&
               code!=ERROR_SUCCESS)
                return false;
        }
        if(pass+1<max_attempts)Sleep(std::min(700u,120u*(pass+1u)));
    }
    return missing(directory);
}
}
#endif
