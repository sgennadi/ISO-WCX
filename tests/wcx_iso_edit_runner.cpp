// Architecture-matched Windows helper for the ISO WCX ABI regression test.
// This EXE is a development test tool; it is not shipped with the WCX plugin.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <iostream>
#include <string>

using PackFn = int (__stdcall*)(wchar_t*,wchar_t*,wchar_t*,wchar_t*,int);
using DeleteFn = int (__stdcall*)(wchar_t*,wchar_t*);

int wmain(int argc,wchar_t** argv) {
    if(argc!=5) {
        std::cerr<<"Usage: wcx_iso_edit_runner DLL ISO pack|delete SOURCE_DIR\n";
        return 2;
    }
    HMODULE plugin=LoadLibraryW(argv[1]);
    if(!plugin) {
        std::cerr<<"Cannot load WCX DLL: Win32 "<<GetLastError()<<"\n";
        return 3;
    }
    const auto pack=reinterpret_cast<PackFn>(GetProcAddress(plugin,"PackFilesW"));
    const auto erase=reinterpret_cast<DeleteFn>(GetProcAddress(plugin,"DeleteFilesW"));
    if(!pack||!erase) {
        std::cerr<<"Missing WCX PackFilesW/DeleteFilesW exports\n";
        FreeLibrary(plugin);
        return 4;
    }
    wchar_t names[]=L"EDIT.TXT\0\0";
    wchar_t sub[]=L"";
    int rc=0;
    if(std::wstring(argv[3])==L"pack")
        rc=pack(argv[2],sub,argv[4],names,0);
    else if(std::wstring(argv[3])==L"delete")
        rc=erase(argv[2],names);
    else {
        std::cerr<<"Unknown edit operation\n";
        FreeLibrary(plugin);
        return 5;
    }
    std::cout<<rc<<"\n";
    FreeLibrary(plugin);
    return 0;
}
#endif
