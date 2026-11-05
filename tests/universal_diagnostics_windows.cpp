#ifdef _WIN32
#include <windows.h>
#include "iso_wcx/universal_diagnostics.hpp"
#include <iostream>
#include <string>

int wmain(int argc,wchar_t**argv) {
    if(argc!=2)return 2;
    // Verify no Github tokens are propagated into the child process.
    SetEnvironmentVariableW(L"UNIVERSAL_DIAGNOSTICS_EXE",argv[1]);
    SetEnvironmentVariableW(L"GH_TOKEN",L"MUST_NOT_REACH_COLLECTOR");
    SetEnvironmentVariableW(L"GITHUB_TOKEN",L"MUST_NOT_REACH_COLLECTOR");
    std::wstring report;
    const bool saved=iso_wcx::universal_diagnostics::capture_local(
        iso_wcx::universal_diagnostics::Category::iso_write,0,21,0,report);
    SetEnvironmentVariableW(L"GH_TOKEN",nullptr);
    SetEnvironmentVariableW(L"GITHUB_TOKEN",nullptr);
    SetEnvironmentVariableW(L"UNIVERSAL_DIAGNOSTICS_EXE",nullptr);
    if(!saved || report.find(L"7da17dae-d5f0-4db0-9e26-94f14120311d")==std::wstring::npos ||
       report.find(L"Nothing was sent to GitHub")==std::wstring::npos) {
        std::wcerr << report << L"\n";
        return 3;
    }
    std::wcout << L"PASS native WCX shared local capture-json handshake and no GH_TOKEN inheritance\n";
    return 0;
}
#endif
