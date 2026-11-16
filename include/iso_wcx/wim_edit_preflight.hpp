#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "iso_wcx/archive_edit.hpp"
#include <algorithm>
#include <array>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace iso_wcx::wim_preflight {
namespace fs = std::filesystem;

struct FileIdentity {
    DWORD volume = 0;
    DWORD index_high = 0;
    DWORD index_low = 0;
    bool operator==(const FileIdentity&) const = default;
};

// A native file ID identifies the image across hard links and DOS-path aliases.
// Opening is metadata-only; no data bytes, WIM indexes or security are changed.
inline FileIdentity image_identity(const fs::path& image) {
    HANDLE raw=CreateFileW(image.c_str(),FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(raw==INVALID_HANDLE_VALUE)
        throw std::runtime_error("inspect WIM edit file identity (Win32 "+
                                 std::to_string(GetLastError())+")");
    struct File { HANDLE h;~File(){CloseHandle(h);} } file{raw};
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(file.h,&info))
        throw std::runtime_error("read WIM edit file identity (Win32 "+
                                 std::to_string(GetLastError())+")");
    if(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))
        throw EditUnsupported("WIM edit source must be a regular non-reparse file");
    return {info.dwVolumeSerialNumber,info.nFileIndexHigh,info.nFileIndexLow};
}

struct MountedInfo {
    WCHAR WimPath[MAX_PATH];
    WCHAR MountPath[MAX_PATH];
    DWORD ImageIndex;
    DWORD MountFlags;
};
static_assert(sizeof(MountedInfo)==2*sizeof(WCHAR)*MAX_PATH+2*sizeof(DWORD));

// Shared, read-only mount-state preflight adapted from DISM-GUI's Wim360Reader.
// Never unmount, repair, discard, modify or delete an external DISM mount.
inline void reject_registered_mounts(HMODULE wimgapi,const fs::path& image) {
    using GetMountedInfo=BOOL(WINAPI*)(DWORD,PDWORD,PVOID,DWORD,PDWORD);
    auto get=reinterpret_cast<GetMountedInfo>(
        GetProcAddress(wimgapi,"WIMGetMountedImageInfo"));
    if(!get)throw EditUnsupported("WIM mount state API unavailable");
    DWORD count=0,needed=0;
    SetLastError(ERROR_SUCCESS);
    const BOOL first=get(1u,&count,nullptr,0u,&needed);
    const DWORD first_error=GetLastError();
    if(count==0 && needed==0 &&
       (first || first_error==ERROR_SUCCESS ||
        first_error==ERROR_INSUFFICIENT_BUFFER))
        return;
    if(!first && first_error!=ERROR_INSUFFICIENT_BUFFER)
        throw std::runtime_error("read WIM registered mount state (Win32 "+
                                 std::to_string(first_error)+")");
    if(needed==0 || needed>1024u*1024u)
        throw EditUnsupported("WIM mount registry size is invalid");

    std::vector<MountedInfo> entries(
        (static_cast<std::size_t>(needed)+sizeof(MountedInfo)-1u)/sizeof(MountedInfo));
    const DWORD capacity=static_cast<DWORD>(entries.size()*sizeof(MountedInfo));
    if(!get(1u,&count,entries.data(),capacity,&needed))
        throw std::runtime_error("enumerate WIM registered mounts (Win32 "+
                                 std::to_string(GetLastError())+")");
    if(count>512u || needed>capacity ||
       static_cast<std::size_t>(count)*sizeof(MountedInfo)>needed)
        throw EditUnsupported("WIM mount registry dimensions are invalid");

    const auto current=image_identity(image);
    const auto normalized=fs::absolute(image).lexically_normal().wstring();
    for(DWORD i=0;i<count;++i) {
        const auto& entry=entries[i];
        const auto end=std::find(std::begin(entry.WimPath),
                                 std::end(entry.WimPath),L'\0');
        if(end==std::begin(entry.WimPath))continue;
        if(end==std::end(entry.WimPath))
            throw EditUnsupported("WIM registered mount has an invalid source path");
        const fs::path registered(std::wstring(entry.WimPath,end));
        try {
            if(_wcsicmp(normalized.c_str(),
                fs::absolute(registered).lexically_normal().c_str())==0)
                throw EditUnsupported("WIM edit refused: image is registered as mounted");
        }catch(const EditUnsupported&) {
            throw;
        }catch(const fs::filesystem_error&) {
            // An unrelated stale mount may contain a missing path. Compare
            // identity if possible; never clean up someone else's registry.
        }
        // A registry source may be a hardlink or a symlink to the same
        // NTFS file, so resolve the target for identity comparison. If an
        // unrelated stale record is inaccessible, do not delete or repair it.
        bool same_file=false;
        try {
            same_file=current==image_identity(registered);
        }catch(const std::exception&) {
            // Unreadable unrelated mount source: only the exact pathname
            // match above can be established without modifying anything.
        }
        if(same_file)
            throw EditUnsupported("WIM edit refused: image is registered as mounted");
    }
}

// Port of DISM-GUI's machine-local exclusive image operation lease concept.
// Lock key uses stable NTFS file identity, so 32/64-bit TC and hardlink aliases
// synchronize on the same key. Keep the zero-byte file (never unlink a live
// locked inode); Windows releases the handle automatically on process exit.
class ImageLease {
    HANDLE lock_ = INVALID_HANDLE_VALUE;
public:
    explicit ImageLease(const fs::path& image,
                        const fs::path& optional_root={}) {
        const auto identity=image_identity(image);
        fs::path root=optional_root;
        if(root.empty()) {
            wchar_t program_data[32768]{};
            const DWORD size=GetEnvironmentVariableW(
                L"ProgramData",program_data,32768);
            if(size==0 || size>=32768)
                throw EditUnsupported("WIM machine-local lock root unavailable");
            root=fs::path(program_data)/L"ISO-WCX"/L"ImageLocks";
        }
        std::error_code ec;
        fs::create_directories(root,ec);
        if(ec)throw std::runtime_error(
            "create WIM image operation lock directory (Win32 "+
            std::to_string(ec.value())+")");
        wchar_t name[96]{};
        swprintf_s(name,L"wim-%08lx-%08lx-%08lx.lock",
            static_cast<unsigned long>(identity.volume),
            static_cast<unsigned long>(identity.index_high),
            static_cast<unsigned long>(identity.index_low));
        const auto lock_name=root/name;
        lock_=CreateFileW(lock_name.c_str(),GENERIC_READ|GENERIC_WRITE,
            0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(lock_==INVALID_HANDLE_VALUE) {
            const DWORD error=GetLastError();
            if(error==ERROR_SHARING_VIOLATION || error==ERROR_LOCK_VIOLATION)
                throw EditUnsupported("WIM edit already active on this computer");
            throw std::runtime_error("acquire WIM image operation lock (Win32 "+
                                     std::to_string(error)+")");
        }
    }
    ImageLease(const ImageLease&)=delete;
    ImageLease& operator=(const ImageLease&)=delete;
    ~ImageLease(){if(lock_!=INVALID_HANDLE_VALUE)CloseHandle(lock_);}
};
}
#endif
