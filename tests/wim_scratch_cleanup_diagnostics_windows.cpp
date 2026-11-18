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

    std::cout<<"PASS: private WIM scratch POSIX file/directory unlink, "
                "held delete-sharing readers, nonempty-directory refusal, "
                "type mismatch, and anonymous lock diagnostics\n";
    return 0;
}
#else
#error "Windows-only WIM scratch cleanup diagnostic regression"
#endif
