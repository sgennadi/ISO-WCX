#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "iso_wcx/wim_scratch_diagnostics.hpp"
#include "iso_wcx/wim_scratch_delete.hpp"
#include <windows.h>
#include <filesystem>
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
    const auto old_namespace=iso_wcx::wim_scratch::probe_directory(misleading);
    if(old_namespace.survivors!=2 || old_namespace.vanished!=2 ||
       old_namespace.scan_error!=0) {
        std::cerr<<"Synthetic Win32-normalized name mismatch: "
                 <<old_namespace.counts()<<"\n";
        return 49;
    }
    const auto literal_namespace=iso_wcx::wim_scratch::probe_directory(literal);
    if(literal_namespace.survivors!=2 || literal_namespace.files!=2 ||
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

    std::cout<<"PASS: private WIM scratch POSIX file/directory unlink, "
                "held delete-sharing readers, nonempty-directory refusal, "
                "vanished-only confirmed retry, real NTFS literal names, "
                "live-file race protection, type mismatch, and "
                "anonymous lock diagnostics\n";
    return 0;
}
#else
#error "Windows-only WIM scratch cleanup diagnostic regression"
#endif
