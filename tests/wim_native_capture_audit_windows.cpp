#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "iso_wcx/wim_resource_audit.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs=std::filesystem;
using namespace iso_wcx::wim_detail;
using WimCreate=HANDLE(WINAPI*)(PCWSTR,DWORD,DWORD,DWORD,DWORD,PDWORD);
using WimCapture=HANDLE(WINAPI*)(HANDLE,PCWSTR,DWORD);
using WimSetTemp=BOOL(WINAPI*)(HANDLE,PCWSTR);
using WimClose=BOOL(WINAPI*)(HANDLE);

static void require(bool pass,const char* why) {
    if(!pass)throw std::runtime_error(why);
}
struct Wimgapi {
    HMODULE library=LoadLibraryExW(
        L"wimgapi.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    WimCreate create=nullptr;
    WimCapture capture=nullptr;
    WimSetTemp temp=nullptr;
    WimClose close=nullptr;
    Wimgapi() {
        require(library!=nullptr,"Windows WIMGAPI unavailable");
        create=reinterpret_cast<WimCreate>(
            GetProcAddress(library,"WIMCreateFile"));
        capture=reinterpret_cast<WimCapture>(
            GetProcAddress(library,"WIMCaptureImage"));
        temp=reinterpret_cast<WimSetTemp>(
            GetProcAddress(library,"WIMSetTemporaryPath"));
        close=reinterpret_cast<WimClose>(
            GetProcAddress(library,"WIMCloseHandle"));
        require(create && capture && temp && close,
                "Windows WIMGAPI capture exports unavailable");
    }
    ~Wimgapi() {if(library)FreeLibrary(library);}
};
struct WimHandle {
    WimClose close=nullptr;
    HANDLE value=nullptr;
    WimHandle(WimClose fn,HANDLE h):close(fn),value(h) {
        require(value && value!=INVALID_HANDLE_VALUE,
                "WIMGAPI could not create native test handle");
    }
    WimHandle(const WimHandle&)=delete;
    WimHandle& operator=(const WimHandle&)=delete;
    ~WimHandle(){if(value)close(value);}
    void release(){if(value){close(value);value=nullptr;}}
};
static void capture_test_wim(
        Wimgapi& api,const fs::path& source,
        const fs::path& destination,const fs::path& scratch,
        DWORD compression) {
    DWORD created=0;
    // WIM_COMPRESS_XPRESS = 1, WIM_COMPRESS_LZX = 2.
    WimHandle wim(api.close,api.create(destination.c_str(),
        GENERIC_READ|GENERIC_WRITE,CREATE_ALWAYS,0,compression,&created));
    require(api.temp(wim.value,scratch.c_str())!=FALSE,
            "WIMGAPI could not set test scratch path");
    WimHandle image(api.close,api.capture(wim.value,source.c_str(),0));
    image.release();
    wim.release();
}
static std::vector<std::uint8_t> read_image(const fs::path& path) {
    std::ifstream file(path,std::ios::binary);
    require(bool(file),"cannot read WIM after native capture");
    return {std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
}

int wmain() {
    fs::path root=fs::temp_directory_path()/
        (L"iso-wcx-real-xpress-audit-"+std::to_wstring(GetCurrentProcessId())+
         L"-"+std::to_wstring(GetTickCount64()));
    try {
        fs::create_directories(root);
        const auto source=root/L"source";
        fs::create_directories(source);
        // Native WIMGAPI serialization, not a hand-crafted WIM fixture:
        // multiple directories and compressible files exercise real
        // metadata records, reference hashes and XPRESS chunk tables.
        for(unsigned i=0;i<72u;++i) {
            const auto folder=source/(L"group-"+std::to_wstring(i%6u));
            fs::create_directories(folder);
            const auto file=folder/(L"fixture-file-"+
                std::to_wstring(i)+L".txt");
            std::ofstream out(file,std::ios::binary);
            require(bool(out),"cannot create disposable WIM fixture file");
            for(unsigned k=0;k<4u;++k) {
                out<<"WIMGAPI compression and SHA-1 metadata audit "
                   <<i<<" resource "<<k
                   <<" WIMGAPI compression and SHA-1 metadata audit\n";
            }
            require(bool(out),"cannot finish disposable fixture file");
        }
        Wimgapi api;
        const auto xp=root/L"xp.wim";
        const auto lx=root/L"lzx.wim";
        capture_test_wim(api,source,xp,root,1u);
        capture_test_wim(api,source,lx,root,2u);
        const auto xp_before=read_image(xp);
        const auto lx_before=read_image(lx);

        const auto xp_probe=probe_fast_edit_metadata(xp);
        require(xp_probe.header_and_lookup_valid &&
                xp_probe.images==1u,
                "real XPRESS WIM lookup preflight failed");
        const auto xp_audit=audit_wim_resource_links(xp);
        std::cout<<"REAL XPRESS: metadata_resources="
                 <<xp_probe.metadata_resources
                 <<" compressed_metadata="
                 <<xp_probe.compressed_metadata_resources
                 <<" decoded="<<xp_audit.compressed_metadata_verified
                 <<" link_count="<<xp_audit.referenced_hashes
                 <<" payloads="<<xp_audit.compressed_payloads_verified
                 <<" unverified="<<xp_audit.unverified_payloads
                 <<" result="<<xp_audit.reason<<"\n";
        require(xp_audit.metadata_verified &&
                xp_audit.references_resolved &&
                xp_audit.metadata_resources_verified==1u,
                "real XPRESS WIM metadata or stream references did not validate");
        require(xp_audit.compressed_metadata_verified==
                xp_probe.compressed_metadata_resources,
                "real XPRESS metadata chunk digest verification missing");
        require(xp_audit.limited_audit_complete,
                "real XPRESS WIM includes unverified referenced data");

        const auto lx_probe=probe_fast_edit_metadata(lx);
        require(lx_probe.header_and_lookup_valid &&
                lx_probe.images==1u,
                "real LZX WIM lookup preflight failed");
        const auto lx_audit=audit_wim_resource_links(lx);
        std::cout<<"REAL LZX: compressed_metadata="
                 <<lx_probe.compressed_metadata_resources
                 <<" xpress_decoded="<<lx_audit.compressed_metadata_verified
                 <<" complete="<<lx_audit.limited_audit_complete
                 <<" result="<<lx_audit.reason<<"\n";
        if(lx_probe.compressed_metadata_resources!=0u) {
            require(lx_audit.requires_decompression &&
                    !lx_audit.metadata_verified &&
                    !lx_audit.limited_audit_complete,
                    "LZX metadata was incorrectly treated as XPRESS");
        }
        require(lx_audit.compressed_metadata_verified==0u &&
                lx_audit.compressed_payloads_verified==0u,
                "LZX content decoded through XPRESS by mistake");

        require(read_image(xp)==xp_before && read_image(lx)==lx_before,
                "read-only resource audits modified a native WIM");
        std::error_code ec;
        fs::remove_all(root,ec);
        std::cout<<"PASS real WIMGAPI XPRESS/LZX metadata, links, "
                    "SHA-1 and byte preservation\n";
        return 0;
    }catch(const std::exception& e) {
        std::cerr<<"FAIL real WIMGAPI WIM resource audit: "<<e.what()<<"\n";
        std::error_code ec;
        fs::remove_all(root,ec);
        return 1;
    }
}
#endif
