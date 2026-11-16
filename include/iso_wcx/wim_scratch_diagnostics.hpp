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
}
#endif
