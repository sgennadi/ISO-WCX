#pragma once
#include "iso_wcx/archive_edit.hpp"
#include <chrono>
#include <fstream>
#include <random>
#include <mutex>
#include <vector>
#include <algorithm>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "iso_wcx/private_workspace_cleanup.hpp"
#endif
namespace iso_wcx {
#ifdef _WIN32
// A WCX listing may keep the old original WIM file open until CloseArchive.
// ReplaceFileW can therefore leave its private original-archive.backup busy
// until the user exits the WIM panel. Remember only THIS process's OWN
// workspaces; never scan unrelated filesystem directories or global mounts.
inline std::mutex& deferred_edit_cleanup_mutex() {
    static std::mutex lock;
    return lock;
}
inline std::vector<std::filesystem::path>& deferred_edit_cleanup_paths() {
    static std::vector<std::filesystem::path> paths;
    return paths;
}
inline void defer_edit_workspace_cleanup(
        const std::filesystem::path& path) noexcept {
    try {
        std::lock_guard lock(deferred_edit_cleanup_mutex());
        auto& list=deferred_edit_cleanup_paths();
        if(std::find(list.begin(),list.end(),path)==list.end())
            list.push_back(path);
    }catch(...) {}
}
inline std::wstring& edit_cleanup_warning_slot() {
    thread_local std::wstring warning;
    return warning;
}
inline void note_edit_cleanup_failure(
        const std::filesystem::path& workspace,
        const workspace_cleanup::Result& report) noexcept {
    try {
        auto issue=L"Temporary WIM workspace could not be fully removed:\n"+
                   workspace.wstring();
        if(!report.blocked.empty())
            issue+=L"\nBlocked entry: "+report.blocked.wstring();
        issue+=L"\nWindows error: "+std::to_wstring(report.error);
        if(report.unsafe)
            issue+=L"\nPossible active mount or unsafe path: no deletion attempted.";
        issue+=L"\nThe source image was not modified by cleanup.";
        edit_cleanup_warning_slot()=issue;
        OutputDebugStringW((L"ISO-WCX cleanup: "+issue+L"\n").c_str());
    }catch(...) {}
}
inline std::wstring take_edit_cleanup_warning() noexcept {
    try {
        auto& slot=edit_cleanup_warning_slot();
        auto result=std::move(slot);
        slot.clear();
        return result;
    }catch(...) {return {};}
}
inline void retry_deferred_edit_workspace_cleanup() noexcept {
    try {
        std::vector<std::filesystem::path> retry;
        {
            std::lock_guard lock(deferred_edit_cleanup_mutex());
            retry.swap(deferred_edit_cleanup_paths());
        }
        for(const auto& workspace:retry) {
            const auto state=workspace_cleanup::cleanup(workspace);
            if(state.clean)continue;
            // Unknown/registered WIM mounts are never deleted or added to
            // generic retry loops: manual mount recovery is required.
            if(!state.unsafe)
                defer_edit_workspace_cleanup(workspace);
            note_edit_cleanup_failure(workspace,state);
        }
    }catch(...) {}
}
#endif
inline void edit_tick(const EditProgress& progress) {
    if(progress && !progress()) throw EditCancelled();
}
// A sibling workspace keeps the final rename on the same volume. Never write
// through the original archive; unsupported input and cancellation leave it intact.
class EditTransaction {
    std::filesystem::path image_;
    std::uintmax_t size_;
    std::filesystem::file_time_type time_;
#ifdef _WIN32
    // Keep file identity and NTFS change time in addition to size/mtime.
    // They catch path replacement and same-size rewrites with restored mtime
    // without a costly multi-gigabyte content hash at commit.
    struct SourceIdentity {
        DWORD volume = 0;
        DWORD id_high = 0;
        DWORD id_low = 0;
        LONGLONG change_time = 0;
        bool operator==(const SourceIdentity&) const = default;
    };
    SourceIdentity source_identity_{};
    static SourceIdentity read_source_identity(const std::filesystem::path& path) {
        HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if(file == INVALID_HANDLE_VALUE)
            throw std::runtime_error("cannot inspect archive identity (Win32 " +
                                     std::to_string(GetLastError()) + ")");
        BY_HANDLE_FILE_INFORMATION info{};
        FILE_BASIC_INFO basic{};
        const BOOL id_ok = GetFileInformationByHandle(file, &info);
        const DWORD id_error = id_ok ? ERROR_SUCCESS : GetLastError();
        const BOOL basic_ok = id_ok &&
            GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic));
        const DWORD basic_error = basic_ok ? ERROR_SUCCESS : GetLastError();
        CloseHandle(file);
        if(!id_ok || !basic_ok)
            throw std::runtime_error("cannot read archive NTFS metadata (Win32 " +
                std::to_string(id_ok ? basic_error : id_error) + ")");
        if((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
            throw EditUnsupported("archive is a directory or reparse point");
        return {info.dwVolumeSerialNumber, info.nFileIndexHigh,
                info.nFileIndexLow, basic.ChangeTime.QuadPart};
    }
#endif
    bool preserve_workspace_ = false;
public:
    std::filesystem::path workspace, output;
    explicit EditTransaction(const std::filesystem::path& image):image_(std::filesystem::absolute(image)),
        size_(std::filesystem::file_size(image)),time_(std::filesystem::last_write_time(image)) {
        if(!std::filesystem::is_regular_file(std::filesystem::symlink_status(image_)))
            throw EditUnsupported("archive must be a regular non-symlink file");
#ifdef _WIN32
        source_identity_ = read_source_identity(image_);
#endif
        std::random_device random;
        for(unsigned i=0;i<32;++i) {
            auto candidate=image_.parent_path()/(".iso-wcx-edit-"+std::to_string(random())+"-"+std::to_string(random()));
            if(std::filesystem::create_directory(candidate)) { workspace=candidate;break; }
        }
        if(workspace.empty())throw std::runtime_error("cannot create edit workspace");
        output=workspace/(image_.filename());
    }
    EditTransaction(const EditTransaction&)=delete;
    // If a Windows WIM mount cannot be safely unmounted, do not recursively
    // remove the mount point or its backing WIM. An administrator can recover
    // the retained workspace and discard the mount explicitly.
    void preserve() noexcept { preserve_workspace_ = true; }
    // Cheap early rejection after a multi-GB stage clone, and again just
    // before replacement. It must not be mistaken for an atomic file lock.
    void assert_source_unchanged() const {
        if(std::filesystem::file_size(image_)!=size_ ||
           std::filesystem::last_write_time(image_)!=time_)
            throw std::runtime_error("archive changed during edit (size/mtime)");
#ifdef _WIN32
        if(!(read_source_identity(image_) == source_identity_))
            throw std::runtime_error("archive changed during edit (identity/change time)");
#endif
    }
    ~EditTransaction() {
        if(preserve_workspace_)return;
#ifdef _WIN32
        // WIMApplyImage preserves ACLs/ownership for the re-captured image.
        // Once WIMGAPI handles close, use a scoped privileged cleaner for
        // OUR disposable clone (including its expanded Windows tree).
        // The cleaner never follows reparse points or touches real mounts.
        workspace_cleanup::Result result{};
        for(unsigned retry=0;retry<3;++retry) {
            result=workspace_cleanup::cleanup(workspace);
            if(result.clean)return;
            if(result.unsafe)break;
            if(result.error!=ERROR_SHARING_VIOLATION &&
               result.error!=ERROR_LOCK_VIOLATION &&
               result.error!=ERROR_ACCESS_DENIED)break;
            Sleep(125u*(retry+1u));
        }
        note_edit_cleanup_failure(workspace,result);
        // A busy archive/backup may be released when TC leaves its WIM
        // panel. Only self-created, verified private paths are deferred.
        if(!result.unsafe)defer_edit_workspace_cleanup(workspace);
#else
        std::error_code ec;
        std::filesystem::remove_all(workspace,ec);
#endif
    }
    void commit(const EditProgress& progress) {
        edit_tick(progress);
#ifdef _WIN32
        HANDLE f=CreateFileW(output.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        if(f==INVALID_HANDLE_VALUE)throw std::runtime_error("cannot flush edited archive");
        const BOOL flushed=FlushFileBuffers(f);CloseHandle(f);
        if(!flushed)throw std::runtime_error("cannot flush edited archive");
        // Revalidate immediately before replacement. NTFS ChangeTime is
        // distinct from LastWriteTime; the latter can be set back by a writer.
        edit_tick(progress);
        assert_source_unchanged();
        // MoveFileExW drops the old file's NTFS DACL and any named streams
        // absent from the replacement. ReplaceFileW merges those properties,
        // including protected DACLs, and still performs a same-volume rename.
        // Do not use IGNORE_MERGE_ERRORS / IGNORE_ACL_ERRORS: a successful edit
        // must never silently weaken the original archive's permissions.
        // The private backup costs no extra data copy on NTFS, and protects
        // the original even if Windows fails between the rename operations.
        auto backup=workspace/"original-archive.backup";
        // The input archive can legally have the same name as our backup.
        // Never let the recovery pathname alias the edited output.
        // NTFS is normally case-insensitive: lexical path comparison is
        // insufficient for differently cased archive filenames.
        if(_wcsicmp(backup.c_str(),output.c_str())==0)
            backup=workspace/"original-archive.backup-recovery";
        if(!ReplaceFileW(image_.c_str(),output.c_str(),backup.c_str(),
                         0,nullptr,nullptr)) {
            const DWORD error=GetLastError();
            const auto original_attrs=GetFileAttributesW(image_.c_str());
            const auto backup_attrs=GetFileAttributesW(backup.c_str());
            // ERROR_UNABLE_TO_MOVE_REPLACEMENT_2 may leave the ORIGINAL
            // at the backup path rather than its usual pathname. Restore
            // only if that pathname is absent; never overwrite another file.
            if(original_attrs==INVALID_FILE_ATTRIBUTES &&
               backup_attrs!=INVALID_FILE_ATTRIBUTES) {
                // Never restore an unrelated replacement as the original.
                // Rename may update ChangeTime, so compare the stable NTFS
                // volume/file ID rather than the full pre-edit snapshot.
                SourceIdentity candidate{};
                try {
                    candidate=read_source_identity(backup);
                }catch(...) {
                    preserve();
                    throw;
                }
                if(candidate.volume!=source_identity_.volume ||
                   candidate.id_high!=source_identity_.id_high ||
                   candidate.id_low!=source_identity_.id_low) {
                    preserve();
                    throw std::runtime_error(
                        "archive replacement failed (Win32 "+std::to_string(error)+
                        "); backup file ID differs from original; keep "+
                        backup.string());
                }
                if(!MoveFileExW(backup.c_str(),image_.c_str(),
                                MOVEFILE_WRITE_THROUGH)) {
                    preserve();
                    throw std::runtime_error(
                        "archive replacement failed (Win32 "+std::to_string(error)+
                        "); original may be at "+backup.string()+
                        "; keep workspace for manual recovery");
                }
            }else if(original_attrs==INVALID_FILE_ATTRIBUTES ||
                     backup_attrs!=INVALID_FILE_ATTRIBUTES) {
                // An ambiguous partial replacement must retain its backup
                // and edited stage for inspection; never delete evidence.
                preserve();
            }
            throw std::runtime_error(
                "cannot safely replace archive (Win32 "+std::to_string(error)+
                "; close other users of the image)");
        }
#else
        assert_source_unchanged();
        std::filesystem::rename(output,image_);
#endif
    }
};
}
