#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <algorithm>
#include <cwctype>
#include <cwchar>
#include <initializer_list>
#include <filesystem>
#include <string>
#include <vector>
#include "iso_wcx/wim_mount_state.hpp"

namespace iso_wcx::workspace_cleanup {
namespace fs=std::filesystem;

// Only private, randomly named EditTransaction workspaces may be removed.
// In particular NEVER aim this helper at a Windows volume, a source WIM,
// or an arbitrary user-selected directory.
inline bool private_workspace_name(const fs::path& root) noexcept {
    try {
        const auto name=root.filename().wstring();
        constexpr wchar_t prefix[]=L".iso-wcx-edit-";
        if(!name.starts_with(prefix))return false;
        const auto rest=name.substr((sizeof(prefix)/sizeof(wchar_t))-1u);
        const auto dash=rest.find(L'-');
        if(dash==std::wstring::npos || !dash || dash+1>=rest.size())
            return false;
        for(wchar_t c:rest)
            if(c!=L'-' && (c<L'0'||c>L'9'))return false;
        return true;
    }catch(...) {return false;}
}
struct Result {
    bool clean=false;
    bool unsafe=false;
    DWORD error=ERROR_SUCCESS;
    fs::path blocked;
};
inline std::wstring extended_path(const fs::path& p) {
    const auto value=fs::absolute(p).lexically_normal().wstring();
    if(value.starts_with(L"\\\\?\\"))return value;
    if(value.starts_with(L"\\\\"))return L"\\\\?\\UNC\\"+value.substr(2);
    return L"\\\\?\\"+value;
}
inline DWORD attributes(const fs::path& p) {
    return GetFileAttributesW(extended_path(p).c_str());
}

class CleanupPrivileges {
    HANDLE source_=nullptr;
    HANDLE elevated_=nullptr;
    bool original_thread_token_=false;
public:
    CleanupPrivileges() noexcept {
        if(OpenThreadToken(GetCurrentThread(),TOKEN_QUERY|TOKEN_DUPLICATE|
                           TOKEN_IMPERSONATE,TRUE,&source_)) {
            original_thread_token_=true;
        }else if(GetLastError()!=ERROR_NO_TOKEN ||
                 !OpenProcessToken(GetCurrentProcess(),
                                   TOKEN_QUERY|TOKEN_DUPLICATE,&source_)) {
            return;
        }
        if(!DuplicateTokenEx(source_,TOKEN_QUERY|TOKEN_ADJUST_PRIVILEGES|
                             TOKEN_IMPERSONATE,nullptr,SecurityImpersonation,
                             TokenImpersonation,&elevated_))return;
        for(const wchar_t* privilege:{SE_RESTORE_NAME,SE_BACKUP_NAME}) {
            TOKEN_PRIVILEGES entry{};
            entry.PrivilegeCount=1;
            if(!LookupPrivilegeValueW(nullptr,privilege,
                                     &entry.Privileges[0].Luid))return;
            entry.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;
            SetLastError(ERROR_SUCCESS);
            if(!AdjustTokenPrivileges(elevated_,FALSE,&entry,0,nullptr,nullptr) ||
               GetLastError()!=ERROR_SUCCESS)return;
        }
        if(!SetThreadToken(nullptr,elevated_))return;
        active_=true;
    }
    ~CleanupPrivileges() {
        if(active_)SetThreadToken(nullptr,original_thread_token_?source_:nullptr);
        if(elevated_)CloseHandle(elevated_);
        if(source_)CloseHandle(source_);
    }
    CleanupPrivileges(const CleanupPrivileges&)=delete;
    CleanupPrivileges& operator=(const CleanupPrivileges&)=delete;
private:
    bool active_=false;
};

// A Windows image restores original NTFS ACLs. Those ACLs may deny delete
// even to the process which applied the image. Changing the ACL *after*
// capture is safe for the disposable expanded workspace only. Never grant
// access to Everyone or alter DACLs of reparse points / hardlinked files.
inline DWORD enable_temporary_directory_access(const fs::path& directory) {
    const DWORD attr=attributes(directory);
    if(attr==INVALID_FILE_ATTRIBUTES)return GetLastError();
    if((attr&FILE_ATTRIBUTE_DIRECTORY)==0 ||
       (attr&FILE_ATTRIBUTE_REPARSE_POINT)!=0)
        return ERROR_ACCESS_DENIED;
    HANDLE token=nullptr;
    if(!OpenThreadToken(GetCurrentThread(),TOKEN_QUERY,TRUE,&token)) {
        if(GetLastError()!=ERROR_NO_TOKEN ||
           !OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))
            return GetLastError();
    }
    DWORD length=0;
    (void)GetTokenInformation(token,TokenUser,nullptr,0,&length);
    if(length<sizeof(TOKEN_USER) || length>65536) {
        CloseHandle(token);
        return ERROR_INVALID_DATA;
    }
    std::vector<BYTE> buffer(length);
    if(!GetTokenInformation(token,TokenUser,buffer.data(),length,&length)) {
        const DWORD error=GetLastError();
        CloseHandle(token);
        return error;
    }
    CloseHandle(token);
    auto* user=reinterpret_cast<TOKEN_USER*>(buffer.data());
    EXPLICIT_ACCESSW ace{};
    ace.grfAccessPermissions=FILE_ALL_ACCESS;
    ace.grfAccessMode=GRANT_ACCESS;
    ace.grfInheritance=NO_INHERITANCE;  // no propagation outside this object
    ace.Trustee.TrusteeForm=TRUSTEE_IS_SID;
    ace.Trustee.TrusteeType=TRUSTEE_IS_USER;
    ace.Trustee.ptstrName=static_cast<LPWSTR>(user->User.Sid);
    PACL acl=nullptr;
    const DWORD prepared=SetEntriesInAclW(1,&ace,nullptr,&acl);
    if(prepared!=ERROR_SUCCESS)return prepared;
    auto path=extended_path(directory);
    const DWORD result=SetNamedSecurityInfoW(path.data(),SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr,nullptr,acl,nullptr);
    LocalFree(acl);
    return result;
}
inline void failure(Result& result,const fs::path& path,DWORD error) {
    if(result.blocked.empty()) {
        result.blocked=path;
        result.error=error?error:ERROR_GEN_FAILURE;
    }
}
inline bool delete_entry(const fs::path& path,Result& result) {
    const auto native=extended_path(path);
    DWORD attr=GetFileAttributesW(native.c_str());
    if(attr==INVALID_FILE_ATTRIBUTES) {
        const DWORD code=GetLastError();
        if(code==ERROR_FILE_NOT_FOUND||code==ERROR_PATH_NOT_FOUND)return true;
        failure(result,path,code);return false;
    }
    const bool directory=(attr&FILE_ATTRIBUTE_DIRECTORY)!=0;
    const bool reparse=(attr&FILE_ATTRIBUTE_REPARSE_POINT)!=0;
    if(directory&&!reparse) {
        const auto wildcard=native+L"\\*";
        WIN32_FIND_DATAW item{};
        HANDLE search=FindFirstFileW(wildcard.c_str(),&item);
        if(search==INVALID_HANDLE_VALUE && GetLastError()==ERROR_ACCESS_DENIED) {
            // Directory-level DACL adjustment only, *not* per-file changes:
            // source WIM security descriptors were already captured.
            if(enable_temporary_directory_access(path)==ERROR_SUCCESS)
                search=FindFirstFileW(wildcard.c_str(),&item);
        }
        if(search==INVALID_HANDLE_VALUE) {
            const DWORD code=GetLastError();
            if(code!=ERROR_FILE_NOT_FOUND) {
                failure(result,path,code);return false;
            }
        }else {
            bool done=true;
            do {
                if(wcscmp(item.cFileName,L".")==0 ||
                   wcscmp(item.cFileName,L"..")==0)continue;
                if(!delete_entry(path/item.cFileName,result)) {
                    done=false;break;
                }
            }while(FindNextFileW(search,&item));
            const DWORD find_error=GetLastError();
            FindClose(search);
            if(!done)return false;
            if(find_error!=ERROR_NO_MORE_FILES) {
                failure(result,path,find_error);return false;
            }
        }
    }
    if(attr&FILE_ATTRIBUTE_READONLY) {
        // Clearing the read-only bit is allowed only under our private
        // workspace. Do not modify a reparse-point target's permissions.
        if(reparse) {
            // Setting attrs on a reparse point can follow the target.
            // Try unlinking the directory entry without changing attrs.
        }else if(!SetFileAttributesW(native.c_str(),
                   attr&~FILE_ATTRIBUTE_READONLY)) {
            const DWORD code=GetLastError();
            if(code!=ERROR_ACCESS_DENIED || !directory) {
                failure(result,path,code);return false;
            }
            const DWORD acl=enable_temporary_directory_access(path);
            if(acl!=ERROR_SUCCESS) {
                failure(result,path,acl);return false;
            }
            if(!SetFileAttributesW(native.c_str(),attr&~FILE_ATTRIBUTE_READONLY)) {
                failure(result,path,GetLastError());return false;
            }
        }
    }
    const auto unlink=[&]() {
        return directory?RemoveDirectoryW(native.c_str()):
                         DeleteFileW(native.c_str());
    };
    if(unlink())return true;
    DWORD code=GetLastError();
    if(!reparse && directory && code==ERROR_ACCESS_DENIED &&
       enable_temporary_directory_access(path)==ERROR_SUCCESS) {
        if(unlink())return true;
        code=GetLastError();
    }
    // DELETE_CHILD granted on our private parent can authorize unlinking
    // protected NTFS files or reparse-point directory entries. Do not
    // rewrite ACLs on files: their hardlinks may have other paths.
    if(code==ERROR_ACCESS_DENIED) {
        const DWORD parent_attr=attributes(path.parent_path());
        if(parent_attr!=INVALID_FILE_ATTRIBUTES &&
           (parent_attr&FILE_ATTRIBUTE_DIRECTORY) &&
           !(parent_attr&FILE_ATTRIBUTE_REPARSE_POINT) &&
           enable_temporary_directory_access(path.parent_path())==
               ERROR_SUCCESS) {
            if(unlink())return true;
            code=GetLastError();
        }
    }
    failure(result,path,code);
    return false;
}
// A directory created by WIMApplyImage is only scratch storage. Unlike a
// mount, it is safe to discard its ACL-protected Windows folders AFTER
// WIMCaptureImage has finished. Use the same link-safe delete engine.
inline Result cleanup_expanded(
        const fs::path& root,const fs::path& expanded) noexcept {
    Result report{};
    try {
        const auto name=expanded.filename().wstring();
        if(!private_workspace_name(root) ||
           fs::absolute(expanded.parent_path()).lexically_normal() !=
           fs::absolute(root).lexically_normal() ||
           !(name==L"expanded" || name.starts_with(L"expanded-image-"))) {
            report.unsafe=true;
            failure(report,expanded,ERROR_INVALID_NAME);
            return report;
        }
        const DWORD mount_attr=attributes(root/L"mounted");
        if(mount_attr!=INVALID_FILE_ATTRIBUTES ||
           (GetLastError()!=ERROR_FILE_NOT_FOUND &&
            GetLastError()!=ERROR_PATH_NOT_FOUND)) {
            report.unsafe=true;
            failure(report,root/L"mounted",ERROR_BUSY);
            return report;
        }
        const DWORD root_attr=attributes(root);
        if(root_attr==INVALID_FILE_ATTRIBUTES ||
           !(root_attr&FILE_ATTRIBUTE_DIRECTORY) ||
           (root_attr&FILE_ATTRIBUTE_REPARSE_POINT)) {
            report.unsafe=true;
            failure(report,root,ERROR_ACCESS_DENIED);
            return report;
        }
        CleanupPrivileges privileges;
        report.clean=delete_entry(expanded,report);
    }catch(...) {
        failure(report,expanded,ERROR_GEN_FAILURE);
    }
    return report;
}

inline Result cleanup(const fs::path& root) noexcept {
    Result report{};
    try {
        if(!private_workspace_name(root)) {
            report.unsafe=true;
            failure(report,root,ERROR_INVALID_NAME);
            return report;
        }
        auto attr=attributes(root);
        if(attr==INVALID_FILE_ATTRIBUTES) {
            const DWORD code=GetLastError();
            if(code==ERROR_FILE_NOT_FOUND||code==ERROR_PATH_NOT_FOUND) {
                report.clean=true;return report;
            }
            failure(report,root,code);return report;
        }
        if(!(attr&FILE_ATTRIBUTE_DIRECTORY) ||
           (attr&FILE_ATTRIBUTE_REPARSE_POINT)) {
            report.unsafe=true;
            failure(report,root,ERROR_ACCESS_DENIED);
            return report;
        }
        // A genuine WIM mount is never equivalent to a disposable Windows
        // tree. DISM may list no mounts even though native WIMGAPI does.
        const auto mount=root/L"mounted";
        const DWORD mount_attr=attributes(mount);
        if(mount_attr!=INVALID_FILE_ATTRIBUTES) {
            if((mount_attr&FILE_ATTRIBUTE_REPARSE_POINT) ||
               !(mount_attr&FILE_ATTRIBUTE_DIRECTORY)) {
                report.unsafe=true;
                failure(report,mount,ERROR_ACCESS_DENIED);
                return report;
            }
            const HMODULE api=LoadLibraryExW(
                L"wimgapi.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
            if(!api) {
                report.unsafe=true;
                failure(report,mount,GetLastError());
                return report;
            }
            const auto state=wim_detail::inspect_private_mount(api,mount,root);
            FreeLibrary(api);
            if(state!=wim_detail::PrivateMountState::Unmounted) {
                report.unsafe=true;
                failure(report,mount,ERROR_BUSY);
                return report;
            }
            // A fresh, valid mount staging directory is always removed
            // explicitly by the fast writer AFTER unmount. A leftover
            // directory is suspect even if native enumeration is empty.
            report.unsafe=true;
            failure(report,mount,ERROR_BUSY);
            return report;
        }
        const DWORD missing=GetLastError();
        if(missing!=ERROR_FILE_NOT_FOUND && missing!=ERROR_PATH_NOT_FOUND) {
            failure(report,mount,missing);return report;
        }
        CleanupPrivileges privileges;
        report.clean=delete_entry(root,report);
    }catch(...) {
        failure(report,root,ERROR_GEN_FAILURE);
    }
    return report;
}
} // namespace iso_wcx::workspace_cleanup
#endif
