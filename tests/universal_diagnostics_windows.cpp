#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "iso_wcx/universal_diagnostics.hpp"
#include "vendor/universal_diagnostics_embedded.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

static void require(bool condition, const char* message) {
    if(!condition)throw std::runtime_error(message);
}

int wmain() {
    namespace fs=std::filesystem;
    wchar_t buffer[32768]{};
    const auto n=GetTempPathW(static_cast<DWORD>(std::size(buffer)),buffer);
    if(n==0 || n>=std::size(buffer))return 2;
    const auto state=fs::path(buffer) /
        (L"iso-wcx-udx-" + std::to_wstring(GetCurrentProcessId()) +
         L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(state);
    SetEnvironmentVariableW(L"UDX_STATE_DIR",state.c_str());
    SetEnvironmentVariableW(L"GH_TOKEN",L"SHOULD_NOT_BE_WRITTEN");
    SetEnvironmentVariableW(L"GITHUB_TOKEN",L"ALSO_NOT_WRITTEN");

    int code=0;
    try {
        std::wstring status;
        require(iso_wcx::universal_diagnostics::capture_local(
            iso_wcx::universal_diagnostics::Category::iso_write,
            0,21,0,status),"embedded WCX local capture failed");
        require(status.find(L"Nothing was sent to GitHub")!=std::wstring::npos,
                "local capture must never report automatic upload");
        const auto folder=::universal_diagnostics::queue_directory(
            "sgennadi/ISO-WCX");
        require(!folder.empty(),"missing shared project queue directory");
        require(fs::exists(folder),"embedded queue folder not created");
        int jsonCount=0;
        for(const auto& item:fs::directory_iterator(folder)) {
            require(item.path().extension()==L".json",
                    "temporary files must not remain in diagnostics queue");
            std::ifstream file(item.path(),std::ios::binary);
            const std::string raw{
                std::istreambuf_iterator<char>(file),
                std::istreambuf_iterator<char>()};
            require(raw.find("\"schema\":\"universal-diagnostics/v1\"")!=
                     std::string::npos,"universal v1 schema absent");
            require(raw.find("\"project\":\"sgennadi/ISO-WCX\"")!=
                     std::string::npos,"project incorrect");
            require(raw.find("\"component\":\"ISO F5 write\"")!=
                     std::string::npos,"enum category incorrect");
            require(raw.find("\"version\":\"0.21.0\"")!=
                     std::string::npos,"app version unexpectedly changed");
            require(raw.find("\"fingerprint\":\"")!=
                     std::string::npos,"fingerprint missing");
            require(raw.find("\"status\":\"pending\"")!=
                     std::string::npos,"local pending delivery absent");
            require(raw.find("\"url\":\"\"")!=
                     std::string::npos,"delivery URL should be empty");
            require(raw.find("SHOULD_NOT_BE_WRITTEN")==std::string::npos &&
                    raw.find("ALSO_NOT_WRITTEN")==std::string::npos &&
                    raw.find("excerpt")==std::string::npos,
                    "secret, excerpt or token leaked to local report");
            ++jsonCount;
        }
        require(jsonCount==1,"expected exactly one embedded JSON record");
        std::wcout<<L"PASS ISO-WCX embedded shared v1 diagnostics queue (no helper EXE)\n";
    }catch(const std::exception& e) {
        std::cerr<<"Embedded local queue failed: "<<e.what()<<"\n";
        code=3;
    }
    SetEnvironmentVariableW(L"UDX_STATE_DIR",nullptr);
    SetEnvironmentVariableW(L"GH_TOKEN",nullptr);
    SetEnvironmentVariableW(L"GITHUB_TOKEN",nullptr);
    std::error_code ec;
    fs::remove_all(state,ec);
    return code;
}
#endif
