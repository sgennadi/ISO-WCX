#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "iso_wcx/wim_scratch_diagnostics.hpp"
#include "iso_wcx/wim_scratch_delete.hpp"
#include "iso_wcx/wim_scratch_acl.hpp"
#include <windows.h>
#include <aclapi.h>
#include <filesystem>
#include <vector>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
namespace fs = std::filesystem;

int wmain() {
    auto root=fs::temp_directory_path()/
        (L"iso-wcx-scratch-probe-"+std::to_wstring(GetCurrentProcessId())+
         L"-"+std::to_wstring(GetTickCount64()));
    if(!fs::create_directory(root))return 10;
    struct Cleanup {
        fs::path path;
        ~Cleanup(){std::error_code ec;fs::remove_all(path,ec);}
    } cleanup{root};
    const auto blocked=root/L"blocked.bin",free_file=root/L"free.bin";
    {
        std::ofstream x(blocked,std::ios::binary),y(free_file,std::ios::binary);
        x<<"synthetic";y<<"synthetic";
        if(!x || !y)return 11;
    }
    HANDLE handle=CreateFileW(blocked.c_str(),GENERIC_READ,FILE_SHARE_READ,
                              nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle==INVALID_HANDLE_VALUE)return 12;
    struct Held {
        HANDLE h;
        ~Held(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);}
    } held{handle};
    const auto locked=iso_wcx::wim_scratch::probe_directory(root);
    if(locked.survivors!=2 || locked.files!=2 || locked.folders!=0 ||
       locked.sharing!=1 || locked.delete_open_ok!=1 ||
       locked.scan_error!=0) {
        std::cerr<<"Scratch lock diagnostics incorrectly classified a "
                    "DELETE-sharing violation: "<<locked.counts()<<"\n";
        return 13;
    }
    CloseHandle(held.h);
    held.h=INVALID_HANDLE_VALUE;
    const auto released=iso_wcx::wim_scratch::probe_directory(root);
    if(released.survivors!=2 || released.files!=2 ||
       released.sharing!=0 || released.delete_open_ok!=2 ||
       released.scan_error!=0) {
        std::cerr<<"Scratch diagnostics did not observe handle release: "
                 <<released.counts()<<"\n";
        return 14;
    }
    const auto report=released.counts();
    if(report.find("blocked.bin")!=std::string::npos ||
       report.find("free.bin")!=std::string::npos)
        return 15;

    // A held delete-sharing NTFS reader must not keep a private file name
    // alive after POSIX disposition. The reader still owns valid file data.
    const auto held_file=root/L"read-while-unlinked.bin";
    {
        std::ofstream out(held_file,std::ios::binary);
        out<<"HELLO";
        if(!out)return 16;
    }
    Held reader{CreateFileW(held_file.c_str(),GENERIC_READ,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)};
    if(reader.h==INVALID_HANDLE_VALUE)return 17;
    if(!iso_wcx::wim_scratch::try_posix_unlink_private_entry(
           held_file,false))return 18;
    if(GetFileAttributesW(held_file.c_str())!=INVALID_FILE_ATTRIBUTES)
        return 19;
    char bytes[5]{};
    DWORD bytes_read=0;
    if(!ReadFile(reader.h,bytes,5,&bytes_read,nullptr) ||
       bytes_read!=5 || std::string(bytes,bytes+5)!="HELLO")
        return 20;
    CloseHandle(reader.h);
    reader.h=INVALID_HANDLE_VALUE;

    // A nonempty directory cannot be POSIX-unlinked. Never remove its
    // contents implicitly or convert the failure into a success result.
    const auto nonempty=root/L"not-empty";
    if(!fs::create_directory(nonempty))return 21;
    const auto child=nonempty/L"keep.bin";
    {
        std::ofstream out(child,std::ios::binary);
        out<<"UNTOUCHED";
        if(!out)return 22;
    }
    if(iso_wcx::wim_scratch::try_posix_unlink_private_entry(
           nonempty,true))return 23;
    if(!fs::exists(child) || !fs::exists(nonempty))return 24;

    // A separate, empty directory held open by a delete-sharing reader
    // can be unlinked without closing its handle on modern NTFS.
    const auto empty=root/L"open-empty";
    if(!fs::create_directory(empty))return 25;
    Held directory_reader{CreateFileW(empty.c_str(),FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr)};
    if(directory_reader.h==INVALID_HANDLE_VALUE)return 26;
    if(!iso_wcx::wim_scratch::try_posix_unlink_private_entry(
           empty,true))return 27;
    if(GetFileAttributesW(empty.c_str())!=INVALID_FILE_ATTRIBUTES)
        return 28;
    if(!fs::exists(child))return 29;

    // A missing entry and a file/directory mismatch must never be treated
    // as a successful delete of another object.
    if(iso_wcx::wim_scratch::try_posix_unlink_private_entry(
           root/L"missing.bin",false))return 30;
    if(iso_wcx::wim_scratch::try_posix_unlink_private_entry(
           child,true))return 31;
    if(!fs::exists(child))return 32;

    // Windows directory enumeration can see names that disappear before
    // GetFileAttributesW. This models the field report precisely:
    // survivors_up_to_32=2, vanished=2, files=folders=sharing=0.
    // The finalizer must confirm actual directory removal with the OS.
    iso_wcx::wim_scratch::Snapshot two_vanished{};
    two_vanished.survivors=2;
    two_vanished.vanished=2;
    const auto ghost_empty=root/L"vanished-entries-empty";
    if(!fs::create_directory(ghost_empty))return 33;
    if(!iso_wcx::wim_scratch::retry_vanished_private_directory(
           ghost_empty,two_vanished))return 34;
    if(fs::exists(ghost_empty))return 35;

    // Do NOT infer that the directory is empty just because a previous
    // snapshot said everything vanished: an unrelated live file can appear
    // after that snapshot. The OS must reject the attempted directory unlink.
    const auto ghost_live=root/L"vanished-entries-raced";
    if(!fs::create_directory(ghost_live))return 36;
    const auto new_child=ghost_live/L"live-child.bin";
    {
        std::ofstream out(new_child,std::ios::binary);
        out<<"DO NOT DELETE";
        if(!out)return 37;
    }
    if(iso_wcx::wim_scratch::retry_vanished_private_directory(
           ghost_live,two_vanished,2))return 38;
    if(!fs::exists(new_child) ||
       !fs::exists(ghost_live))return 39;
    fs::remove(new_child);
    if(!iso_wcx::wim_scratch::retry_vanished_private_directory(
           ghost_live,two_vanished))return 40;
    if(fs::exists(ghost_live))return 41;

    // Partial or errored probes are not authorization to perform the
    // special last-chance unlink, even when the test directory is empty.
    const auto partial=root/L"partly-vanished";
    if(!fs::create_directory(partial))return 42;
    auto incomplete=two_vanished;
    incomplete.vanished=1;
    if(iso_wcx::wim_scratch::retry_vanished_private_directory(
           partial,incomplete,1))return 43;
    if(!fs::exists(partial))return 44;
    incomplete=two_vanished;
    incomplete.scan_error=ERROR_ACCESS_DENIED;
    if(iso_wcx::wim_scratch::retry_vanished_private_directory(
           partial,incomplete,1))return 45;
    if(!fs::exists(partial))return 46;
    fs::remove(partial);

    // Unlike a simulated vanished-only Snapshot, these are REAL NTFS files.
    // Their final dot/space is preserved through the \\?\\ file namespace,
    // but ordinary DOS-path normalization may make GetFileAttributesW say
    // FILE_NOT_FOUND while FindFirstFileW still returns both directory entries.
    // This matches the exact "survivors=2, vanished=2, Win32 145" pattern.
    const auto misleading=root/L"win32-namespace";
    if(!fs::create_directory(misleading))return 47;
    const auto literal=iso_wcx::wim_scratch::literal_private_ntfs_path(misleading);
    const fs::path odd_names[]={L"ghost.",L"ghost "};
    for(const auto& part:odd_names) {
        const auto file=literal/part;
        HANDLE h=CreateFileW(file.c_str(),GENERIC_WRITE,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(h==INVALID_HANDLE_VALUE) {
            std::cerr<<"Could not create synthetic literal NTFS entry: "
                     <<GetLastError()<<"\n";
            return 48;
        }
        CloseHandle(h);
    }
    // A real nonempty directory produces the exact Win32 145 reported
    // from the domain host, not a synthetic/mocked error code.
    if(RemoveDirectoryW(misleading.c_str()) ||
       GetLastError()!=ERROR_DIR_NOT_EMPTY)return 62;
    const auto old_namespace=iso_wcx::wim_scratch::probe_directory(misleading);
    if(old_namespace.survivors!=2 || old_namespace.vanished!=2 ||
       old_namespace.trailing_dot_or_space!=2 ||
       old_namespace.scan_error!=0) {
        std::cerr<<"Synthetic Win32-normalized name mismatch: "
                 <<old_namespace.counts()<<"\n";
        return 49;
    }
    const auto literal_namespace=iso_wcx::wim_scratch::probe_directory(literal);
    if(literal_namespace.survivors!=2 || literal_namespace.files!=2 ||
       literal_namespace.trailing_dot_or_space!=2 ||
       literal_namespace.vanished!=0 || literal_namespace.scan_error!=0) {
        std::cerr<<"Literal namespace failed to resolve NTFS files: "
                 <<literal_namespace.counts()<<"\n";
        return 50;
    }
    for(const auto& part:odd_names) {
        const auto file=literal/part;
        if(!iso_wcx::wim_scratch::try_posix_unlink_private_entry(file,false))
            return 51;
        if(GetFileAttributesW(file.c_str())!=INVALID_FILE_ATTRIBUTES)
            return 52;
    }
    if(!RemoveDirectoryW(literal.c_str()))return 53;
    if(fs::exists(misleading))return 54;

    // Deep WIM Apply paths are another documented DOS-vs-literal mismatch:
    // the Total Commander host may not have opted into process-wide long
    // paths. Explicit prefixing works independent of that host manifest.
    const auto deep=iso_wcx::wim_scratch::literal_private_ntfs_path(
        root/L"long-path-case");
    if(!CreateDirectoryW(deep.c_str(),nullptr))return 55;
    const auto first=deep/std::wstring(120,L'a');
    const auto second=first/std::wstring(120,L'b');
    if(!CreateDirectoryW(first.c_str(),nullptr) ||
       !CreateDirectoryW(second.c_str(),nullptr))return 56;
    const auto long_file=second/L"this-name-must-not-be-normalized.bin";
    if(long_file.native().size()<=MAX_PATH)return 57;
    HANDLE deep_handle=CreateFileW(long_file.c_str(),GENERIC_WRITE,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(deep_handle==INVALID_HANDLE_VALUE)return 58;
    CloseHandle(deep_handle);
    if(GetFileAttributesW(long_file.c_str())==INVALID_FILE_ATTRIBUTES)
        return 59;
    if(!iso_wcx::wim_scratch::try_posix_unlink_private_entry(
           long_file,false))return 60;
    if(!RemoveDirectoryW(second.c_str()) ||
       !RemoveDirectoryW(first.c_str()) ||
       !RemoveDirectoryW(deep.c_str()))return 61;

    // Model Windows' domain-admin failure *without* modifying a real
    // user's ACL, owner or WIMMount. When protected DACL propagation is
    // denied, plain NO_INHERITANCE must succeed without an owner change.
    {
        unsigned calls=0,owners=0,opened=0;
        const auto outcome=iso_wcx::wim_scratch::recover_private_scratch_acl(
            [&](bool protect) -> DWORD {
                ++calls;
                return protect?ERROR_ACCESS_DENIED:ERROR_SUCCESS;
            },
            [&]() -> DWORD {++owners;return ERROR_SUCCESS;},
            [&](bool) -> DWORD {++opened;return ERROR_SUCCESS;},
            [&](bool) -> DWORD {++opened;return ERROR_ACCESS_DENIED;});
        if(!outcome.ok()||outcome.protected_first!=ERROR_ACCESS_DENIED ||
           outcome.plain_first!=ERROR_SUCCESS || calls!=2 ||
           owners!=0 || opened!=0)return 63;
    }
    // Before the new branch, an owner change was followed by the same
    // protected operation that could fail again with Win32 5. Reopening
    // with the new owner must be available after both modes are denied.
    {
        unsigned calls=0,owners=0,reopened=0;
        const auto outcome=iso_wcx::wim_scratch::recover_private_scratch_acl(
            [&](bool) -> DWORD {++calls;return ERROR_ACCESS_DENIED;},
            [&]() -> DWORD {++owners;return ERROR_SUCCESS;},
            [&](bool protect) -> DWORD {
                ++reopened;
                return protect?ERROR_ACCESS_DENIED:ERROR_SUCCESS;
            },
            [](bool) -> DWORD {return ERROR_ACCESS_DENIED;});
        if(!outcome.ok()||calls!=4||owners!=1||reopened!=1 ||
           outcome.owner_change!=ERROR_SUCCESS ||
           outcome.reopened_plain!=ERROR_SUCCESS)return 64;
        const auto message=outcome.counts();
        if(message.find("Win32")!=std::string::npos ||
           message.find("C:\\")!=std::string::npos)return 65;
    }
    // No false success, and no retries that could mutate unrelated files
    // if Windows denies every DACL attempt or the owner cannot be changed.
    {
        unsigned owners=0,reopened=0;
        const auto outcome=iso_wcx::wim_scratch::recover_private_scratch_acl(
            [](bool) -> DWORD {return ERROR_ACCESS_DENIED;},
            [&]() -> DWORD {++owners;return ERROR_ACCESS_DENIED;},
            [&](bool) -> DWORD {++reopened;return ERROR_SUCCESS;},
            [](bool) -> DWORD {return ERROR_ACCESS_DENIED;});
        if(outcome.ok()||owners!=1||reopened!=0 ||
           outcome.final_error!=ERROR_ACCESS_DENIED)return 66;
    }
    {
        unsigned owners=0;
        const auto outcome=iso_wcx::wim_scratch::recover_private_scratch_acl(
            [](bool protect) -> DWORD {
                return protect?ERROR_INVALID_PARAMETER:ERROR_SUCCESS;
            },
            [&]() -> DWORD {++owners;return ERROR_SUCCESS;},
            [](bool) -> DWORD {return ERROR_ACCESS_DENIED;},
            [](bool) -> DWORD {return ERROR_ACCESS_DENIED;});
        if(!outcome.ok()||owners!=0)return 67;
    }
    {
        const auto outcome=iso_wcx::wim_scratch::recover_private_scratch_acl(
            [](bool) -> DWORD {return ERROR_ACCESS_DENIED;},
            []() -> DWORD {return ERROR_SUCCESS;},
            [](bool) -> DWORD {return ERROR_INVALID_DATA;},
            [](bool) -> DWORD {return ERROR_ACCESS_DENIED;});
        if(outcome.ok()||outcome.final_error!=ERROR_INVALID_DATA)
            return 68;
    }

    // Exact field failure: owner_change=0 while every ordinary (and
    // reopened) SetSecurityInfo DACL attempt returns Win32 5. A special
    // MAXIMUM_ALLOWED handle must recover without modifying children.
    {
        unsigned max_calls=0,owner_calls=0,normal_calls=0,reopens=0;
        const auto result=iso_wcx::wim_scratch::recover_private_scratch_acl(
            [&](bool) -> DWORD {++normal_calls;return ERROR_ACCESS_DENIED;},
            [&]() -> DWORD {++owner_calls;return ERROR_SUCCESS;},
            [&](bool) -> DWORD {++reopens;return ERROR_ACCESS_DENIED;},
            [&](bool) -> DWORD {
                ++max_calls;
                return max_calls==3?ERROR_SUCCESS:ERROR_ACCESS_DENIED;
            });
        if(!result.ok() || owner_calls!=1 || normal_calls!=4 ||
           reopens!=2 || max_calls!=3 ||
           result.max_before_plain!=ERROR_ACCESS_DENIED ||
           result.max_before_protected!=ERROR_ACCESS_DENIED ||
           result.max_after_plain!=ERROR_SUCCESS)return 69;
    }
    {
        // Recovered using MAXIMUM_ALLOWED BEFORE taking ownership:
        // never alter ownership of a disposable entry unnecessarily.
        unsigned owner_calls=0,normal_calls=0,max_calls=0;
        const auto result=iso_wcx::wim_scratch::recover_private_scratch_acl(
            [&](bool) -> DWORD {++normal_calls;return ERROR_ACCESS_DENIED;},
            [&]() -> DWORD {++owner_calls;return ERROR_SUCCESS;},
            [](bool) -> DWORD {return ERROR_ACCESS_DENIED;},
            [&](bool) -> DWORD {++max_calls;return ERROR_SUCCESS;});
        if(!result.ok() || owner_calls!=0 || normal_calls!=2 ||
           max_calls!=1 || result.owner_change!=
           iso_wcx::wim_scratch::AclRecovery::unused)return 70;
    }
    {
        // File-ID mismatch on the fresh MAXIMUM_ALLOWED handle is fatal,
        // never followed by ownership changes or another ACL attempt.
        unsigned owner_calls=0;
        const auto result=iso_wcx::wim_scratch::recover_private_scratch_acl(
            [](bool) -> DWORD {return ERROR_ACCESS_DENIED;},
            [&]() -> DWORD {++owner_calls;return ERROR_SUCCESS;},
            [](bool) -> DWORD {return ERROR_ACCESS_DENIED;},
            [](bool) -> DWORD {return ERROR_INVALID_DATA;});
        if(result.ok() || result.final_error!=ERROR_INVALID_DATA ||
           owner_calls!=0)return 71;
    }
    {
        // A true Win32 5 on both MAXIMUM_ALLOWED handles must remain
        // a failure. No false success even if owner_change succeeded.
        const auto result=iso_wcx::wim_scratch::recover_private_scratch_acl(
            [](bool) -> DWORD {return ERROR_ACCESS_DENIED;},
            []() -> DWORD {return ERROR_SUCCESS;},
            [](bool) -> DWORD {return ERROR_ACCESS_DENIED;},
            [](bool) -> DWORD {return ERROR_ACCESS_DENIED;});
        if(result.ok() || result.final_error!=ERROR_ACCESS_DENIED ||
           result.max_after_protected!=ERROR_ACCESS_DENIED)return 72;
    }

    // Real Windows API integration: for a disposable scratch parent
    // containing an already existing child, SetSecurityInfo on a handle
    // originally opened with MAXIMUM_ALLOWED must NOT propagate the new
    // inheritable parent ACE into that child's DACL.
    {
        const auto parent=root/L"max-allowed-no-inheritance";
        if(!fs::create_directory(parent))return 73;
        const auto protected_child=parent/L"child.txt";
        {
            std::ofstream out(protected_child,std::ios::binary);
            out<<"CHILD UNCHANGED";
            if(!out)return 74;
        }
        const auto child_dacl=[&]() {
            std::vector<BYTE> descriptor(4096);
            DWORD required=0;
            if(!GetFileSecurityW(protected_child.c_str(),
                    DACL_SECURITY_INFORMATION,descriptor.data(),
                    static_cast<DWORD>(descriptor.size()),&required)) {
                if(GetLastError()!=ERROR_INSUFFICIENT_BUFFER)
                    throw std::runtime_error("read disposable child ACL");
                descriptor.resize(required);
                if(!GetFileSecurityW(protected_child.c_str(),
                        DACL_SECURITY_INFORMATION,descriptor.data(),
                        static_cast<DWORD>(descriptor.size()),&required))
                    throw std::runtime_error("resize disposable child ACL");
            }
            BOOL present=FALSE,defaulted=FALSE;
            PACL dacl=nullptr;
            if(!GetSecurityDescriptorDacl(descriptor.data(),&present,
                                          &dacl,&defaulted) ||
               !present || !dacl)
                throw std::runtime_error("disposable child has no DACL");
            const auto* bytes=reinterpret_cast<const BYTE*>(dacl);
            return std::vector<BYTE>(bytes,bytes+dacl->AclSize);
        };
        const auto child_before=child_dacl();

        HANDLE raw_token=nullptr;
        if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw_token))
            return 75;
        Held token{raw_token};
        DWORD token_bytes=0;
        GetTokenInformation(token.h,TokenUser,nullptr,0,&token_bytes);
        if(!token_bytes)return 76;
        std::vector<BYTE> token_data(token_bytes);
        if(!GetTokenInformation(token.h,TokenUser,token_data.data(),
                                token_bytes,&token_bytes))return 77;
        const PSID user_sid=
            reinterpret_cast<TOKEN_USER*>(token_data.data())->User.Sid;
        BYTE world_data[SECURITY_MAX_SID_SIZE]{};
        DWORD world_bytes=sizeof(world_data);
        if(!CreateWellKnownSid(WinWorldSid,nullptr,world_data,&world_bytes))
            return 78;
        std::vector<BYTE> acl_buffer(1024);
        PACL new_acl=reinterpret_cast<PACL>(acl_buffer.data());
        if(!InitializeAcl(new_acl,static_cast<DWORD>(acl_buffer.size()),
                          ACL_REVISION) ||
           !AddAccessAllowedAceEx(new_acl,ACL_REVISION,0,FILE_ALL_ACCESS,user_sid) ||
           !AddAccessAllowedAceEx(new_acl,ACL_REVISION,
                OBJECT_INHERIT_ACE|CONTAINER_INHERIT_ACE,
                FILE_READ_ATTRIBUTES,world_data))return 79;
        Held parent_handle{CreateFileW(parent.c_str(),MAXIMUM_ALLOWED,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|
                FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
        if(parent_handle.h==INVALID_HANDLE_VALUE)return 80;
        const DWORD acl_status=SetSecurityInfo(parent_handle.h,
            SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,
            nullptr,nullptr,new_acl,nullptr);
        if(acl_status!=ERROR_SUCCESS) {
            std::cerr<<"MAXIMUM_ALLOWED scratch-parent ACL update failed: "
                     <<acl_status<<"\n";
            return 81;
        }
        if(child_dacl()!=child_before) {
            std::cerr<<"MAXIMUM_ALLOWED parent ACE reached an existing child\n";
            return 82;
        }
        // The parent's directory entry is not a reparse point and its
        // existing child remains byte-for-byte intact.
        if(!fs::exists(protected_child) ||
           fs::file_size(protected_child)!=15)return 83;
    }

    std::cout<<"PASS: private WIM scratch POSIX file/directory unlink, "
                "held delete-sharing readers, nonempty-directory refusal, "
                "vanished-only confirmed retry, real NTFS literal names, "
                "MAX_PATH bypass, live-file race protection, "
                "ACL protected/plain/owner/reopen fail-closed paths, "
                "type mismatch, and anonymous lock diagnostics\n";
    return 0;
}
#else
#error "Windows-only WIM scratch cleanup diagnostic regression"
#endif
