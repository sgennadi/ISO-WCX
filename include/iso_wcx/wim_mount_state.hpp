#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <cstddef>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

namespace iso_wcx::wim_detail {

// DISM /Get-MountedWimInfo is not an authoritative check for native WIMGAPI
// MountImageHandle registrations. Query the same native API that mounted the
// private clone. Never call WIMDeleteImageMounts or touch global mounts.
enum class PrivateMountState { Unmounted, Registered, Unknown };

struct MountInfoLevel1 {
    wchar_t WimPath[MAX_PATH];
    wchar_t MountPath[MAX_PATH];
    DWORD ImageIndex;
    DWORD MountFlags;
};
static_assert(sizeof(MountInfoLevel1)==
              2u*MAX_PATH*sizeof(wchar_t)+2u*sizeof(DWORD));

inline std::wstring mount_key(const std::filesystem::path& input) {
    auto path=std::filesystem::absolute(input).lexically_normal().wstring();
    while(path.size()>3 && (path.back()==L'\\' || path.back()==L'/'))
        path.pop_back();
    std::transform(path.begin(),path.end(),path.begin(),
                   [](wchar_t ch){ return std::towlower(ch); });
    return path;
}

// Read-only. A registration in OUR private directory, an unexplained
// reparse point, an API failure, or an ambiguous result ALWAYS prevents
// cleanup. Other applications' mounts are never modified.
inline PrivateMountState inspect_private_mount(
        HMODULE wimgapi,const std::filesystem::path& mount,
        const std::filesystem::path& workspace) noexcept {
    try {
        const DWORD flags=GetFileAttributesW(mount.c_str());
        if(flags!=INVALID_FILE_ATTRIBUTES &&
           (flags&FILE_ATTRIBUTE_REPARSE_POINT))
            return PrivateMountState::Registered;
        if(flags==INVALID_FILE_ATTRIBUTES) {
            const DWORD error=GetLastError();
            if(error!=ERROR_FILE_NOT_FOUND && error!=ERROR_PATH_NOT_FOUND)
                return PrivateMountState::Unknown;
        } else if(!(flags&FILE_ATTRIBUTE_DIRECTORY)) {
            return PrivateMountState::Unknown;
        }
        using Query=BOOL (WINAPI*)(DWORD,PDWORD,PVOID,DWORD,PDWORD);
        auto fn=reinterpret_cast<Query>(
            GetProcAddress(wimgapi,"WIMGetMountedImageInfo"));
        if(!fn)return PrivateMountState::Unknown;

        DWORD entries=0,needed=0;
        SetLastError(ERROR_SUCCESS);
        BOOL ok=fn(1,&entries,nullptr,0,&needed);
        if(!ok && GetLastError()!=ERROR_INSUFFICIENT_BUFFER)
            return PrivateMountState::Unknown;
        // Use a few extra entries to tolerate concurrent mounts.
        std::size_t capacity=std::max<std::size_t>(
            needed,static_cast<std::size_t>(entries+4u)*
                   sizeof(MountInfoLevel1));
        if(capacity>4u*1024u*1024u)return PrivateMountState::Unknown;
        const auto target=mount_key(mount);
        const auto root=mount_key(workspace);
        for(int attempt=0;attempt<3;++attempt) {
            std::vector<MountInfoLevel1> buffer(
                (capacity+sizeof(MountInfoLevel1)-1u)/
                 sizeof(MountInfoLevel1));
            if(buffer.empty())buffer.resize(1);
            const auto bytes=buffer.size()*sizeof(MountInfoLevel1);
            if(bytes>4u*1024u*1024u)return PrivateMountState::Unknown;
            DWORD count=0,returned=0;
            SetLastError(ERROR_SUCCESS);
            if(!fn(1,&count,buffer.data(),static_cast<DWORD>(bytes),
                   &returned)) {
                if(GetLastError()!=ERROR_INSUFFICIENT_BUFFER)
                    return PrivateMountState::Unknown;
                capacity=std::max<std::size_t>(
                    capacity*2u,static_cast<std::size_t>(returned));
                if(capacity>4u*1024u*1024u)
                    return PrivateMountState::Unknown;
                continue;
            }
            if(count>buffer.size() ||
               returned>bytes)
                return PrivateMountState::Unknown;
            for(DWORD i=0;i<count;++i) {
                // The API fields have fixed MAX_PATH buffers. Fail closed
                // if a corrupted record is not NUL-terminated.
                const auto& m=buffer[i];
                if(!std::wmemchr(m.MountPath,L'\0',MAX_PATH) ||
                   !std::wmemchr(m.WimPath,L'\0',MAX_PATH))
                    return PrivateMountState::Unknown;
                const auto actual=mount_key(m.MountPath);
                if(actual==target || actual==root ||
                   (actual.size()>root.size() &&
                    actual.compare(0,root.size(),root)==0 &&
                    actual[root.size()]==L'\\'))
                    return PrivateMountState::Registered;
            }
            return PrivateMountState::Unmounted;
        }
    }catch(...) {}
    return PrivateMountState::Unknown;
}
} // namespace iso_wcx::wim_detail
#endif
