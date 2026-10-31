#pragma once
// NTFS staged extraction compatibility for firmware with the AMI UEFI NTFS
// multi-run read bug. Reserve the entire file BEFORE its first data write;
// do not mistake a successful reservation for a guarantee of one NTFS extent.
// No external DLL or executable is needed by Total Commander.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace iso_wcx::ntfs_preallocation {
namespace fs = std::filesystem;

struct Stage {
    bool created=false;
    bool ntfs=false;
    bool preallocated=false;
};

inline bool enabled() noexcept {
    // Escape hatch for unusual Windows filesystems and troubleshooting.
    wchar_t value[8]{};
    const DWORD length=GetEnvironmentVariableW(
        L"ISO_WCX_NTFS_PREALLOCATE",value,8);
    return !(length==1 && value[0]==L'0');
}

inline bool diagnostic_enabled() noexcept {
    wchar_t value[8]{};
    return GetEnvironmentVariableW(L"ISO_WCX_NTFS_EXTENTS",value,8)==1
        && value[0]==L'1';
}

inline bool is_ntfs(const fs::path& parent) noexcept {
    try {
        wchar_t volume[MAX_PATH]{};
        if(!GetVolumePathNameW(parent.c_str(),volume,MAX_PATH))
            return false;
        // The AMI firmware issue concerns booting from a local NTFS disk.
        // A UNC/SMB share may also report NTFS, but reserving a large
        // remote file has no firmware-read benefit and can hurt performance.
        const UINT drive=GetDriveTypeW(volume);
        if(drive!=DRIVE_FIXED && drive!=DRIVE_REMOVABLE)return false;
        wchar_t filesystem[32]{};
        if(!GetVolumeInformationW(volume,nullptr,0,nullptr,nullptr,nullptr,
                                  filesystem,32))
            return false;
        return _wcsicmp(filesystem,L"NTFS")==0;
    }catch(...) {return false;}
}

// Create a NEW randomly named sibling file with CREATE_NEW, never CREATE_ALWAYS
// or TRUNCATE_EXISTING. Even a racing symlink cannot replace a destination.
// Always leaves a valid (possibly empty) file for a non-truncating std::ofstream.
// A failed speculative SetEndOfFile falls back to an empty stage; the original
// destination remains untouched until the separately verified final rename.
inline Stage create_stage(const fs::path& candidate,
                          std::uint64_t expected_size) noexcept {
    Stage stage{};
    try {
        HANDLE h=CreateFileW(candidate.c_str(),GENERIC_WRITE,0,nullptr,
                             CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(h==INVALID_HANDLE_VALUE)return stage;
        stage.created=true;
        stage.ntfs=is_ntfs(candidate.parent_path());
        if(stage.ntfs && enabled() && expected_size>0 &&
           expected_size<=static_cast<std::uint64_t>(
               std::numeric_limits<LONGLONG>::max())) {
            LARGE_INTEGER length{};
            length.QuadPart=static_cast<LONGLONG>(expected_size);
            if(SetFilePointerEx(h,length,nullptr,FILE_BEGIN) &&
               SetEndOfFile(h)) {
                stage.preallocated=true;
            }else {
                // If an unusual NTFS driver rejected the reservation, do not
                // leave a misleading partial file size. An ordinary streamed
                // extraction can still work correctly on that volume.
                LARGE_INTEGER zero{};
                if(!SetFilePointerEx(h,zero,nullptr,FILE_BEGIN) ||
                   !SetEndOfFile(h)) {
                    CloseHandle(h);
                    (void)DeleteFileW(candidate.c_str());
                    return {};
                }
            }
        }
        if(!CloseHandle(h)) {
            (void)DeleteFileW(candidate.c_str());
            return {};
        }
        return stage;
    }catch(...) {return {};}
}

// Optional read-only NTFS extent diagnostic (debug output only). FSCTL may
// be unavailable on some volumes, for remote shares and in non-admin shells.
// It must NEVER make extraction fail or claim that preallocation guarantees
// contiguous clusters. Query after closing the writer, before final rename.
inline void log_extents_if_requested(const fs::path& path,
                                    bool was_preallocated) noexcept {
    if(!diagnostic_enabled() || !is_ntfs(path.parent_path()))return;
    try {
        HANDLE h=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                             nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(h==INVALID_HANDLE_VALUE)return;
        constexpr std::size_t capacity=64u*1024u;
        std::vector<std::uint64_t> result(capacity/sizeof(std::uint64_t));
        STARTING_VCN_INPUT_BUFFER input{};
        std::uint64_t total=0;
        bool complete=false;
        for(unsigned iteration=0;iteration<256;++iteration) {
            DWORD returned=0;
            SetLastError(ERROR_SUCCESS);
            const BOOL ok=DeviceIoControl(h,FSCTL_GET_RETRIEVAL_POINTERS,
                    &input,static_cast<DWORD>(sizeof(input)),
                    result.data(),static_cast<DWORD>(capacity),
                    &returned,nullptr);
            const DWORD error=ok?ERROR_SUCCESS:GetLastError();
            constexpr std::size_t header=offsetof(
                RETRIEVAL_POINTERS_BUFFER,Extents);
            if(!ok && error!=ERROR_MORE_DATA)break;
            if(returned<header)break;
            const auto* block=reinterpret_cast<
                const RETRIEVAL_POINTERS_BUFFER*>(result.data());
            const std::size_t count=block->ExtentCount;
            if(count==0 || count>
                (static_cast<std::size_t>(returned)-header)/
                 sizeof(block->Extents[0]))
                break;
            total+=count;
            if(ok){complete=true;break;}
            const auto next=block->Extents[count-1].NextVcn.QuadPart;
            if(next<=input.StartingVcn.QuadPart)break;
            input.StartingVcn.QuadPart=next;
        }
        CloseHandle(h);
        const auto status=complete?
            (total<=1?L"contiguous":L"fragmented"):L"unknown";
        const auto note=L"ISO-WCX NTFS extraction: "+
            path.wstring()+L" extents="+std::to_wstring(total)+
            L", status="+status+
            L", size preallocated="+(was_preallocated?std::wstring(L"yes"):
                                                        std::wstring(L"no"))+
            L" (AMI UEFI compatibility, not a guarantee)\n";
        OutputDebugStringW(note.c_str());
    }catch(...) {}
}
} // namespace iso_wcx::ntfs_preallocation
#endif
