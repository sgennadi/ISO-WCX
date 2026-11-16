#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "iso_wcx/wim_edit_preflight.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <stdexcept>

namespace fs=std::filesystem;
int wmain() {
    const auto root=fs::temp_directory_path()/
        (L"iso-wcx-wim-lease-"+std::to_wstring(GetCurrentProcessId())+L"-"+
         std::to_wstring(GetTickCount64()));
    try {
        fs::create_directories(root);
        const auto image=root/L"disposable.wim";
        const auto alias=root/L"same-inode.wim";
        const auto locks=root/L"private-lock-tests";
        {
            std::ofstream input(image,std::ios::binary);
            input<<"synthetic fixture, never mounted";
        }
        fs::create_hard_link(image,alias);
        if(!(iso_wcx::wim_preflight::image_identity(image)==
             iso_wcx::wim_preflight::image_identity(alias)))
            throw std::runtime_error("WIM hardlink file identity changed");

        auto first=std::make_unique<iso_wcx::wim_preflight::ImageLease>(
            image,locks);
        bool blocked=false;
        try {
            iso_wcx::wim_preflight::ImageLease second(alias,locks);
        }catch(const iso_wcx::EditUnsupported&) {blocked=true;}
        if(!blocked)
            throw std::runtime_error("concurrent WIM edit lease was not blocked");

        // Other paths point to distinct NTFS file IDs and can be edited.
        const auto independent=root/L"unrelated.wim";
        {
            std::ofstream out(independent,std::ios::binary);
            out<<"different file";
        }
        {
            iso_wcx::wim_preflight::ImageLease other(independent,locks);
        }
        first.reset();
        {
            iso_wcx::wim_preflight::ImageLease after_release(alias,locks);
        }

        // Actual system API, but no DISM mount is created or modified.
        HMODULE system=LoadLibraryExW(
            L"wimgapi.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!system)throw std::runtime_error("system WIMGAPI unavailable");
        try {
            iso_wcx::wim_preflight::reject_registered_mounts(system,image);
        }catch(...) {FreeLibrary(system);throw;}
        FreeLibrary(system);
        fs::remove_all(root);
        std::cout<<"PASS: machine-local WIM file-ID lease, hardlink alias, "
                    "release and read-only registered mount preflight\n";
        return 0;
    }catch(const std::exception& error) {
        std::cerr<<"FAIL WIM edit preflight: "<<error.what()<<"\n";
        std::error_code ec;
        fs::remove_all(root,ec);
        return 1;
    }
}
#endif
