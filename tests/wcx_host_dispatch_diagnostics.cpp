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
        require(contains(report,"| iso | pack | 24 | unsupported_layout | write | 1 |"),
                "existing aggregate operation results missing");
        require(!contains(report,"SECRET-USER-F5") &&
                report.find(fixture.folder.string())==std::string::npos,
                "private file/path leaked into exported report");
        require(!contains(read(fixture.folder/L"ISO-WCX"/L"Logs"/L"operations.jsonl"),
                          "SECRET-USER-F5"),
                "operation journal leaked a private image name");
        // Normalized WIM stage + ISO guard subtype make failures
        // actionable while private input paths remain invisible.
        {
            iso_wcx::diagnostics::Operation op(
                "pack",fixture.folder/L"SECRET-WIM-INDEX.wim",1,1);
            op.stage(L"WIM: Rebuild WIM image 1/2",65);
            op.complete(13,"WIMCaptureImage failed (Win32 5)");
        }
        {
            iso_wcx::diagnostics::Operation op(
                "pack",fixture.folder/L"PRIVATE-HYBRID-BOOT.iso",1);
            op.stage(L"write",0);
            op.complete(24,
                "hybrid MBR ISO/UDF only supports equal-length file replacement");
        }
        {
            iso_wcx::diagnostics::Operation op(
                "pack",fixture.folder/L"PRIVATE-ROCK-RIDGE.iso",1);
            op.complete(24,
                "Rock Ridge/system-use ISO editing requires a protected UDF bridge");
        }
        // Replacement lock and imaging privilege failures should never
        // be conflated with a corrupt WIM or send sensitive file paths.
        {
            iso_wcx::diagnostics::Operation op(
                "pack",fixture.folder/L"PRIVATE-LOCKED.wim",1,1);
            op.stage(L"Prepare NTFS staging",3);
            op.complete(24,"WIM archive is open without delete sharing (Win32 32)");
        }
        {
            iso_wcx::diagnostics::Operation op(
                "pack",fixture.folder/L"PRIVATE-REPLACE.wim",1,1);
            op.stage(L"Commit verified WIM",99);
            op.complete(13,
                "cannot replace archive (close other users of the image; Win32 32)");
        }
        {
            iso_wcx::diagnostics::Operation op(
                "pack",fixture.folder/L"PRIVATE-TOKEN.wim",1,1);
            op.stage(L"Prepare NTFS staging",3);
            op.complete(13,
                "WIM editing requires administrator backup/restore/security privileges (Win32 1300)");
        }
        // Distinguish protected WIM scratch ACL failure from unrelated
        // access errors without including private paths in shared reports.
        {
            iso_wcx::diagnostics::Operation op(
                "pack",fixture.folder/L"PRIVATE-SCRATCH-ACL.wim",1,1);
            op.stage(L"WIM: Cleanup WIM temporary files 1/1",91);
            op.complete(13,"restore WIM scratch cleanup access (Win32 5)");
        }
        const auto extended=read(iso_wcx::diagnostics::export_report());
        require(contains(extended,
                "| wim | pack | 13 | wim_scratch_acl_denied | cleanup | 1 |"),
                "protected WIM scratch ACL failure category missing");
        require(!contains(extended,"PRIVATE-SCRATCH-ACL"),
                "protected WIM scratch name leaked into report");
        require(contains(extended,
                "| wim | pack | 13 | wim_api_capture | capture | 1 |"),
                "WIM API failure stage or category missing");
        require(contains(extended,
                "| iso | pack | 24 | iso_hybrid_restricted | write | 1 |"),
                "hybrid ISO protection category missing");
        require(contains(extended,
                "| iso | pack | 24 | iso_rock_ridge_protected | unreported | 1 |"),
                "protected Rock Ridge category missing");
        require(contains(extended,
                "| wim | pack | 24 | archive_replace_locked | prepare | 1 |") &&
                contains(extended,
                "| wim | pack | 13 | archive_replace_locked | commit | 1 |"),
                "locked WIM was not distinguished from Apply/Capture errors");
        require(contains(extended,
                "| wim | pack | 13 | wim_privileges_unavailable | prepare | 1 |"),
                "missing WIM imaging privileges lacked a distinct category");
        require(!contains(extended,"PRIVATE-LOCKED") &&
                !contains(extended,"PRIVATE-REPLACE") &&
                !contains(extended,"PRIVATE-TOKEN"),
                "private WIM test paths leaked into diagnostic export");
        require(!contains(extended,"SECRET-WIM-INDEX") &&
                !contains(extended,"PRIVATE-HYBRID-BOOT") &&
                !contains(extended,"PRIVATE-ROCK-RIDGE"),
                "private image names leaked into diagnostic summary");
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
