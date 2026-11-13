// In-process, privacy-safe WCX dispatch counter regression.
// No Total Commander, network request, real image or external helper needed.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "iso_wcx/operation_diagnostics.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace fs=std::filesystem;
using iso_wcx::diagnostics::HostCall;

void require(bool result,const char* message) {
    if(!result)throw std::runtime_error(message);
}
std::string read(const fs::path& file) {
    std::ifstream input(file,std::ios::binary);
    require(bool(input),"cannot read generated diagnostic report");
    return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}
bool contains(const std::string& s,const char* needle) {
    return s.find(needle)!=std::string::npos;
}
struct Temp {
    fs::path folder;
    std::wstring previous;
    bool had_previous=false;
    Temp() {
        const DWORD needed=GetEnvironmentVariableW(L"LOCALAPPDATA",nullptr,0);
        if(needed && needed<32768) {
            std::wstring v(needed,L'\0');
            const DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",v.data(),needed);
            if(n>0 && n<needed) {v.resize(n);previous=std::move(v);had_previous=true;}
        }
        folder=fs::temp_directory_path()/
            (L"iso-wcx-dispatch-test-"+std::to_wstring(GetCurrentProcessId())+
             L"-"+std::to_wstring(GetTickCount64()));
        fs::create_directories(folder);
        require(SetEnvironmentVariableW(L"LOCALAPPDATA",folder.c_str())!=0,
                "cannot isolate local diagnostic fixture");
    }
    ~Temp() {
        SetEnvironmentVariableW(L"LOCALAPPDATA",
            had_previous?previous.c_str():nullptr);
        std::error_code ec;
        fs::remove_all(folder,ec);
    }
};
int wmain(){
    try {
        Temp fixture;
        auto before=iso_wcx::diagnostics::host_session_summary();
        require(contains(before,"GetPackerCaps: 0"),
                "dispatch counters must begin at zero");
        require(contains(before,"F5 dispatch: **NOT OBSERVED"),
                "zero F5 dispatch not shown");
        iso_wcx::diagnostics::note_host_call(HostCall::caps);
        iso_wcx::diagnostics::note_host_call(HostCall::caps);
        iso_wcx::diagnostics::note_host_call(HostCall::progress_w);
        iso_wcx::diagnostics::note_host_call(HostCall::progress_a);
        iso_wcx::diagnostics::note_host_call(HostCall::pack_w);
        iso_wcx::diagnostics::note_host_call(HostCall::pack_a);
        iso_wcx::diagnostics::note_host_call(HostCall::delete_w);
        iso_wcx::diagnostics::note_host_call(HostCall::delete_a);
        iso_wcx::diagnostics::note_host_call(HostCall::count); // invalid sentinel
        const auto after=iso_wcx::diagnostics::host_session_summary();
        require(contains(after,"GetPackerCaps: 2"),"caps count incorrect");
        require(contains(after,"PackFilesW: 1; PackFiles ANSI: 1"),
                "Unicode/ANSI pack calls were conflated");
        require(contains(after,"DeleteFilesW: 1; DeleteFiles ANSI: 1"),
                "Unicode/ANSI delete calls were conflated");
        require(contains(after,"Packing progress registration W: 1; ANSI: 1"),
                "progress callback registration not recorded");
        require(contains(after,"F5 dispatch: **OBSERVED**"),
                "nonzero F5 dispatch not shown");
        const auto private_image=fixture.folder/L"SECRET-USER-F5.iso";
        {
            iso_wcx::diagnostics::Operation operation("pack",private_image,1);
            operation.stage(L"write",20);
            operation.complete(24,"private path to SECRET-USER-F5.iso not supported");
        }
        const auto exported=iso_wcx::diagnostics::export_report();
        require(!exported.empty(),"diagnostic export failed");
        const auto report=read(exported);
        require(contains(report,"F5 / Delete host dispatch"),
                "report omitted in-process host dispatch triage");
        require(contains(report,"PackFilesW: 1; PackFiles ANSI: 1"),
                "report omitted WCX entry point counters");
        require(contains(report,"| iso | pack | 24 | unsupported_layout | 1 |"),
                "existing aggregate operation results missing");
        require(!contains(report,"SECRET-USER-F5") &&
                report.find(fixture.folder.string())==std::string::npos,
                "private file/path leaked into exported report");
        require(!contains(read(fixture.folder/L"ISO-WCX"/L"Logs"/L"operations.jsonl"),
                          "SECRET-USER-F5"),
                "operation journal leaked a private image name");
        std::cout<<"PASS: F5 dispatch counters, ANSI/Unicode attribution, "
                    "zero vs observed host triage, private report and journal\n";
        return 0;
    }catch(const std::exception& e){
        std::cerr<<"FAIL WCX F5 dispatch diagnostics: "<<e.what()<<"\n";
        return 1;
    }
}
#else
#error "Windows-only Total Commander host diagnostic test"
#endif
