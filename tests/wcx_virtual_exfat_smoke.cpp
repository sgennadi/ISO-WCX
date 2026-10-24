#ifdef _WIN32
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <string>
namespace fs=std::filesystem;
struct OpenW {wchar_t* ArcName;int OpenMode,OpenResult;wchar_t* CmtBuf;int CmtBufSize,CmtSize,CmtState;};
struct HeaderExW {wchar_t ArcName[1024],FileName[1024];int Flags;unsigned PackSize,PackSizeHigh,UnpSize,UnpSizeHigh;int HostOS,FileCRC,FileTime,UnpVer,Method,FileAttr;char* CmtBuf;int CmtBufSize,CmtSize,CmtState;char Reserved[1024];};
using Open=HANDLE(__stdcall*)(OpenW*);
using Head=int(__stdcall*)(HANDLE,HeaderExW*);
using Process=int(__stdcall*)(HANDLE,int,wchar_t*,wchar_t*);
using Close=int(__stdcall*)(HANDLE);
int wmain(int argc,wchar_t**argv) {
    if(argc!=4)return 2;
    HMODULE dll=LoadLibraryW(argv[1]);
    if(!dll)return 3;
    auto open=reinterpret_cast<Open>(GetProcAddress(dll,"OpenArchiveW"));
    auto head=reinterpret_cast<Head>(GetProcAddress(dll,"ReadHeaderExW"));
    auto process=reinterpret_cast<Process>(GetProcAddress(dll,"ProcessFileW"));
    auto close=reinterpret_cast<Close>(GetProcAddress(dll,"CloseArchive"));
    if(!open||!head||!process||!close){FreeLibrary(dll);return 4;}
    std::wstring name=argv[2];
    OpenW param{};param.ArcName=name.data();
    HANDLE h=open(&param);
    if(!h||param.OpenResult){FreeLibrary(dll);return 5;}
    bool found=false;int rc=6;
    for(unsigned i=0;i<100000;++i) {
        HeaderExW item{};int status=head(h,&item);
        if(status==10){rc=found?0:7;break;}
        if(status){rc=8;break;}
        if(std::wstring(item.FileName)==L"partition1\\README.TXT") {
            found=true;
            if(item.UnpSize!=700){rc=9;break;}
            auto output=(fs::path(argv[3])/L"README.TXT").wstring();
            fs::create_directories(fs::path(output).parent_path());
            if(process(h,2,nullptr,output.data())!=0){rc=9;break;}
        } else if(process(h,0,nullptr,nullptr)!=0) {rc=10;break;}
    }
    close(h);FreeLibrary(dll);
    if(rc)std::wcerr<<L"exFAT WCX test failed: "<<rc<<L"\n";
    return rc;
}
#endif
