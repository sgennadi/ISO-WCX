#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "iso_wcx/wim_scratch_diagnostics.hpp"
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
    std::cout<<"PASS: WIM scratch probe, share-violation vs DELETE access, "
                "anonymous count-only report and handle-release recovery\n";
    return 0;
}
#else
#error "Windows-only WIM scratch cleanup diagnostic regression"
#endif
