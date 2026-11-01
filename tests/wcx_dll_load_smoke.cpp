#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>
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
                            "GetPackerCaps","CloseArchive",
                            "SetProcessDataProcW"}) {
        if(!GetProcAddress(module,name)) {
            std::wcerr << L"WCX_MISSING_EXPORT: " << name << std::endl;
            FreeLibrary(module);
            return 3;
        }
    }
    using GetCapsFn=int (__stdcall*)();
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
