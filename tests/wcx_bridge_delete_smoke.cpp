// Direct, architecture-matched Total Commander WCX ABI regression.
// Input is a temporary valid ISO/UDF Bridge created independently by Python.
// The module must remove a shared file in BOTH views, not just ISO or UDF.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
namespace fs=std::filesystem;
struct OpenW {
    wchar_t* ArcName; int OpenMode,OpenResult;
    wchar_t* CmtBuf; int CmtBufSize,CmtSize,CmtState;
};
struct HeaderExW {
    wchar_t ArcName[1024],FileName[1024]; int Flags;
    unsigned PackSize,PackSizeHigh,UnpSize,UnpSizeHigh;
    int HostOS,FileCRC,FileTime,UnpVer,Method,FileAttr;
    char* CmtBuf; int CmtBufSize,CmtSize,CmtState; char Reserved[1024];
};
using Open=HANDLE (__stdcall*)(OpenW*);
using Read=int (__stdcall*)(HANDLE,HeaderExW*);
using Process=int (__stdcall*)(HANDLE,int,wchar_t*,wchar_t*);
using Close=int (__stdcall*)(HANDLE);
using Delete=int (__stdcall*)(wchar_t*,wchar_t*);
static void need(bool ok,const char* message) {
    if(!ok)throw std::runtime_error(message);
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=3)return 1;
    try {
        auto* dll=LoadLibraryW(fs::absolute(argv[1]).c_str());
        need(dll!=nullptr,"cannot load architecture-matched ISO-WCX DLL");
        const auto sym=[&](const char* name)->FARPROC {
            FARPROC result=GetProcAddress(dll,name);
            need(result!=nullptr,"missing WCX export");
            return result;
        };
        auto open=reinterpret_cast<Open>(sym("OpenArchiveW"));
        auto read=reinterpret_cast<Read>(sym("ReadHeaderExW"));
        auto process=reinterpret_cast<Process>(sym("ProcessFileW"));
        auto close=reinterpret_cast<Close>(sym("CloseArchive"));
        auto remove=reinterpret_cast<Delete>(sym("DeleteFilesW"));
        auto name=fs::absolute(argv[2]).wstring();
        const auto listing=[&]() {
            OpenW params{};
            params.ArcName=name.data();
            HANDLE handle=open(&params);
            need(handle!=nullptr && params.OpenResult==0,
                 "cannot open ISO/UDF Bridge via WCX");
            std::set<std::wstring> paths;
            bool ended=false;
            try {
                for(unsigned count=0;count<64;++count) {
                    HeaderExW entry{};
                    int rc=read(handle,&entry);
                    if(rc==10){ended=true;break;}
                    need(rc==0,"WCX ReadHeaderExW failed on ISO/UDF Bridge");
                    paths.insert(entry.FileName);
                    need(process(handle,0,nullptr,nullptr)==0,
                         "WCX ProcessFileW PK_SKIP failed");
                }
                need(ended,"WCX bridge listing did not reach end");
            }catch(...){close(handle);throw;}
            close(handle);
            return paths;
        };
        const auto before=listing();
        need(before.contains(L"hello") && before.contains(L"other"),
             "bridge initial UDF namespace is missing test files");
        wchar_t file[]=L"hello\0\0";
        need(remove(name.data(),file)==0,
             "DeleteFilesW refused a valid shared-extent bridge file");
        const auto after=listing();
        need(!after.contains(L"hello") && after.contains(L"other"),
             "WCX DeleteFilesW did not update UDF view correctly");
        FreeLibrary(dll);
        std::cout<<"PASS: ISO-WCX DeleteFilesW synchronized ISO/UDF Bridge "
                    "delete with retained neighbor file\n";
        return 0;
    }catch(const std::exception& e){
        std::cerr<<"FAIL: WCX Bridge DeleteFilesW: "<<e.what()<<"\n";
        return 2;
    }
}
#endif
