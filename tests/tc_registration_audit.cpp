// Read-only TC association audit regression. Uses a fully synthetic INI and
// WCX paths; never inspects or changes real Total Commander configuration.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "iso_wcx/tc_registration_audit.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace fs=std::filesystem;
int wmain() {
    fs::path folder;
    try {
        wchar_t temporary[MAX_PATH]{};
        if(!GetTempPathW(MAX_PATH,temporary))return 2;
        folder=fs::path(temporary)/
            (L"ISO-WCX-TC-REG-TEST-"+std::to_wstring(GetCurrentProcessId())+
             L"-"+std::to_wstring(GetTickCount64()));
        fs::create_directories(folder);
        const auto plugin=folder/L"plugins"/L"wcx"/L"ISO-WCX";
        fs::create_directories(plugin);
        {std::ofstream out(plugin/L"ISO-WCX.wcx");out<<"fake";}
        {std::ofstream out(plugin/L"ISO-WCX.wcx64");out<<"fake";}
        const auto ini=folder/L"wincmd.ini";
        {
            std::wofstream out(ini);
            out<<L"[PackerPlugins]\n"
               <<L"wim=192,"<<(plugin/L"ISO-WCX.wcx").wstring()<<L"\n"
               <<L"iso=158,"<<(plugin/L"ISO-WCX.wcx").wstring()<<L"\n"
               <<L"udf=222,"<<(plugin/L"ISO-WCX.wcx").wstring()<<L"\n"
               <<L"esd=192,"<<(plugin/L"ISO-WCX.wcx").wstring()<<L"\n"
               <<L"[PackerPlugins64]\n"
               <<L"exts=iso*wim*udf*esd*\n";
        }
        if(!SetEnvironmentVariableW(L"COMMANDER_INI",ini.c_str())||
           !SetEnvironmentVariableW(L"LOCALAPPDATA",folder.c_str())||
           !SetEnvironmentVariableW(L"COMMANDER_PATH",folder.c_str()))
            return 3;
        const auto report=iso_wcx::export_tc_registration_audit();
        if(report.empty()||!fs::is_regular_file(report))return 4;
        std::wifstream source(report);
        std::wstringstream stream;
        stream<<source.rdbuf();
        const auto text=stream.str();
        if(text.find(L"[wim]")==text.npos ||
           text.find(L"Capabilities: 192")==text.npos ||
           text.find(L"ERROR: WCX registration DISABLES F5/Delete")==text.npos ||
           text.find(L"[iso]")==text.npos ||
           text.find(L"Capabilities: 158")==text.npos ||
           text.find(L"[udf]")==text.npos ||
           text.find(L"Capabilities: 222")==text.npos ||
           text.find(L"Companion x64 WCX:")==text.npos ||
           text.find(L"ESD/SWM: read-only")==text.npos)
            return 5;
        if(text.find(L"STATUS: NOT REGISTERED")==text.npos)return 6; // SWM
        std::error_code ignored;
        fs::remove_all(folder,ignored);
        std::cout<<"PASS: TC association report classifies 192/158/222, flags missing SWM and checks x64 DLL\n";
        return 0;
    }catch(const std::exception& e) {
        std::cerr<<"TC audit test exception: "<<e.what()<<"\n";
        if(!folder.empty()){std::error_code ignored;fs::remove_all(folder,ignored);}
        return 7;
    }
}
