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
        if(!MoveFileExW(output.c_str(),image_.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("cannot replace archive (close other users of the image)");
#else
        std::filesystem::rename(output,image_);
#endif
    }
};
}
