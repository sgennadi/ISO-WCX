#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>
#include <cstring>
#include <iostream>

int wmain(int argc,wchar_t** argv) {
    if(argc!=2)return 1;
    std::wcerr << L"WCX_LOAD_BEGIN" << std::endl;
    const auto file=std::filesystem::absolute(argv[1]);
    const HMODULE module=LoadLibraryW(file.c_str());
    if(!module) {
        std::wcerr << L"WCX_LOAD_FAILED Win32=" << GetLastError()
                   << std::endl;
        return 2;
    }
    std::wcerr << L"WCX_LOAD_OK" << std::endl;
    // Verify the actual binary's packing exports before involving TC.
    // Previously this quick smoke skipped PackFilesW, allowing a host-only
    // F5 regression to be overlooked until the longer WIM integration test.
    for(const char* name : {"OpenArchiveW","ReadHeaderExW","ProcessFileW",
                            "PackFilesW","PackFiles","DeleteFilesW",
                            "GetPackerCaps","GetBackgroundFlags",
                            "PackSetDefaultParams","CloseArchive",
                            "SetProcessDataProcW"}) {
        if(!GetProcAddress(module,name)) {
            std::wcerr << L"WCX_MISSING_EXPORT: " << name << std::endl;
            FreeLibrary(module);
            return 3;
        }
    }
    using GetCapsFn=int (__stdcall*)();
    using InitFn=void (__stdcall*)(void*);
    struct PluginInit {
        int size;
        DWORD interfaceLow;
        DWORD interfaceHigh;
        char defaultIni[MAX_PATH];
    };
    static_assert(sizeof(PluginInit)==272);
    const auto init=reinterpret_cast<InitFn>(
        GetProcAddress(module,"PackSetDefaultParams"));
    const auto bg=reinterpret_cast<GetCapsFn>(
        GetProcAddress(module,"GetBackgroundFlags"));
    if(!init||!bg||bg()!=0) {
        std::wcerr << L"WCX_INIT_OR_BACKGROUND_ABI_INVALID" << std::endl;
        FreeLibrary(module);
        return 6;
    }
    // Total Commander invokes the ANSI-only initialization callback before
    // other WCX entrypoints. The plugin must accept a valid host structure,
    // a null pointer, and a short future/past structure safely.
    PluginInit params{};
    params.size=sizeof(params);
    params.interfaceHigh=2;
    params.interfaceLow=21;
    memcpy(params.defaultIni,"C:\\Temp\\iso-wcx-test.ini",25);
    init(nullptr);
    init(&params);
    params.size=0;
    init(&params);
    const auto caps=reinterpret_cast<GetCapsFn>(
        GetProcAddress(module,"GetPackerCaps"));
    if(!caps || caps()!=158 || (caps()&2)==0) {
        std::wcerr << L"WCX_CAPS_INVALID expected 158 with PK_CAPS_MODIFY" << std::endl;
        FreeLibrary(module);
        return 5;
    }
    std::wcerr << L"WCX_ABI_PACKING_EXPORTS_OK caps=158" << std::endl;
    if(!FreeLibrary(module))return 4;
    std::wcerr << L"WCX_UNLOAD_OK" << std::endl;
    return 0;
}
#endif
