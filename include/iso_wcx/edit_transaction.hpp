#pragma once
#include "iso_wcx/archive_edit.hpp"
#include <chrono>
#include <fstream>
#include <random>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace iso_wcx {
inline void edit_tick(const EditProgress& progress) {
    if(progress && !progress()) throw EditCancelled();
}
// A sibling workspace keeps the final rename on the same volume. Never write
// through the original archive; unsupported input and cancellation leave it intact.
class EditTransaction {
    std::filesystem::path image_;
    std::uintmax_t size_;
    std::filesystem::file_time_type time_;
    bool preserve_workspace_ = false;
public:
    std::filesystem::path workspace, output;
    explicit EditTransaction(const std::filesystem::path& image):image_(std::filesystem::absolute(image)),
        size_(std::filesystem::file_size(image)),time_(std::filesystem::last_write_time(image)) {
        if(!std::filesystem::is_regular_file(std::filesystem::symlink_status(image_)))
            throw EditUnsupported("archive must be a regular non-symlink file");
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
    ~EditTransaction() {
        if(preserve_workspace_)return;
        std::error_code ec;
        std::filesystem::remove_all(workspace,ec);
    }
    void commit(const EditProgress& progress) {
        edit_tick(progress);
        if(std::filesystem::file_size(image_)!=size_ || std::filesystem::last_write_time(image_)!=time_)
            throw std::runtime_error("archive changed during edit");
#ifdef _WIN32
        HANDLE f=CreateFileW(output.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        if(f==INVALID_HANDLE_VALUE)throw std::runtime_error("cannot flush edited archive");
        const BOOL flushed=FlushFileBuffers(f);CloseHandle(f);
        if(!flushed)throw std::runtime_error("cannot flush edited archive");
        // MoveFileExW drops the old file's NTFS DACL and any named streams
        // absent from the replacement. ReplaceFileW merges those properties,
        // including protected DACLs, and still performs a same-volume rename.
        // Do not use IGNORE_MERGE_ERRORS / IGNORE_ACL_ERRORS: a successful edit
        // must never silently weaken the original archive's permissions.
        // The private backup costs no extra data copy on NTFS, and protects
        // the original even if Windows fails between the rename operations.
        const auto backup=workspace/"original-archive.backup";
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
        std::filesystem::rename(output,image_);
#endif
    }
};
}
