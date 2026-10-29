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
    if(!GetProcAddress(module,"OpenArchiveW") ||
       !GetProcAddress(module,"DeleteFilesW") ||
       !GetProcAddress(module,"GetPackerCaps")) {
        FreeLibrary(module);
        std::wcerr << L"WCX_MISSING_EXPORT" << std::endl;
        return 3;
    }
    if(!FreeLibrary(module))return 4;
    std::wcerr << L"WCX_UNLOAD_OK" << std::endl;
    return 0;
}
#endif
