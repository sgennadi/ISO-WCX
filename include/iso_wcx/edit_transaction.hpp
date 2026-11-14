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
#ifdef _WIN32
    // Cheap protection against an image of the same size being rewritten
    // while a multi-GB WIM/ISO transaction is in progress. LastWriteTime
    // alone can be restored by other software; NTFS ChangeTime tracks the
    // metadata/data change and file ID detects replacement of the pathname.
    struct SourceIdentity {
        DWORD volume{};
        DWORD file_high{};
        DWORD file_low{};
        LONGLONG change_time{};
        bool operator==(const SourceIdentity&) const = default;
    };
    SourceIdentity source_identity_{};
    static SourceIdentity inspect_source(const std::filesystem::path& image) {
        HANDLE h=CreateFileW(image.c_str(),FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(h==INVALID_HANDLE_VALUE)
            throw std::runtime_error("cannot inspect archive file identity (Win32 "+
                                     std::to_string(GetLastError())+")");
        BY_HANDLE_FILE_INFORMATION info{};
        FILE_BASIC_INFO basic{};
        const BOOL identified=GetFileInformationByHandle(h,&info);
        const DWORD id_error=identified?ERROR_SUCCESS:GetLastError();
        const BOOL timed=identified &&
            GetFileInformationByHandleEx(h,FileBasicInfo,&basic,sizeof(basic));
        const DWORD time_error=timed?ERROR_SUCCESS:GetLastError();
        CloseHandle(h);
        if(!identified||!timed)
            throw std::runtime_error("cannot read archive change metadata (Win32 "+
                                     std::to_string(identified?time_error:id_error)+")");
        if(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))
            throw EditUnsupported("archive is a directory or a reparse point");
        return {info.dwVolumeSerialNumber,info.nFileIndexHigh,
                info.nFileIndexLow,basic.ChangeTime.QuadPart};
    }
#endif
public:
    std::filesystem::path workspace, output;
    explicit EditTransaction(const std::filesystem::path& image):image_(std::filesystem::absolute(image)),
        size_(std::filesystem::file_size(image)),time_(std::filesystem::last_write_time(image)) {
        if(!std::filesystem::is_regular_file(std::filesystem::symlink_status(image_)))
            throw EditUnsupported("archive must be a regular non-symlink file");
#ifdef _WIN32
        source_identity_=inspect_source(image_);
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
    // The check is deliberately not a claim of an atomic OS-level lock:
    // it protects against routine external writers between stage and
    // commit without hashing huge WIM/ISO images a second time.
    void assert_source_unchanged() const {
        if(std::filesystem::file_size(image_)!=size_ ||
           std::filesystem::last_write_time(image_)!=time_)
            throw std::runtime_error("archive changed during edit (size/mtime)");
#ifdef _WIN32
        if(!(inspect_source(image_)==source_identity_))
            throw std::runtime_error("archive changed during edit (file ID/ChangeTime)");
#endif
    }
    ~EditTransaction() noexcept {
        if(workspace.empty())return;
        // Only the unique private sibling directory created by this
        // transaction may be removed. WIM handles / an antivirus scanner
        // can briefly lock staged files after an aborted Apply/Capture.
        // Bounded retry avoids leaving a locked scratch tree behind
        // permanently. Never scan for or delete previous transactions.
#ifdef _WIN32
        constexpr unsigned attempts=4;
#else
        constexpr unsigned attempts=1;
#endif
        for(unsigned attempt=0;attempt<attempts;++attempt) {
            try {
                std::error_code ec;
                std::filesystem::remove_all(workspace,ec);
                ec.clear();
                if(!std::filesystem::exists(workspace,ec) && !ec)return;
            }catch(...){}
#ifdef _WIN32
            if(attempt+1<attempts)Sleep(150u*(attempt+1u));
#endif
        }
    }
    void commit(const EditProgress& progress) {
        edit_tick(progress);
#ifdef _WIN32
        HANDLE f=CreateFileW(output.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        if(f==INVALID_HANDLE_VALUE)throw std::runtime_error("cannot flush edited archive");
        const BOOL flushed=FlushFileBuffers(f);CloseHandle(f);
        if(!flushed)throw std::runtime_error("cannot flush edited archive");
        // Recheck after flushing the private stage: an external writer may
        // have changed or replaced the source during a slow Apply/Capture.
        edit_tick(progress);
        assert_source_unchanged();
        if(!MoveFileExW(output.c_str(),image_.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
            const DWORD failure=GetLastError();
            throw std::runtime_error("cannot replace archive (close other users of the image; Win32 "+
                                     std::to_string(failure)+")");
        }
#else
        assert_source_unchanged();
        std::filesystem::rename(output,image_);
#endif
    }
};
}
