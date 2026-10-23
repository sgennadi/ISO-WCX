#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// WIMGAPI is resolved dynamically; the optional Windows ADK header is not needed.

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>
namespace fs = std::filesystem;

struct OpenW {
    wchar_t* ArcName;
    int OpenMode,OpenResult;
    wchar_t* CmtBuf;
    int CmtBufSize,CmtSize,CmtState;
};
struct HeaderExW {
    wchar_t ArcName[1024], FileName[1024];
    int Flags;
    unsigned PackSize,PackSizeHigh,UnpSize,UnpSizeHigh;
    int HostOS,FileCRC,FileTime,UnpVer,Method,FileAttr;
    char* CmtBuf;
    int CmtBufSize,CmtSize,CmtState;
    char Reserved[1024];
};
using OpenFn=HANDLE(__stdcall*)(OpenW*);
using ReadFn=int(__stdcall*)(HANDLE,HeaderExW*);
using ProcessFn=int(__stdcall*)(HANDLE,int,wchar_t*,wchar_t*);
using CloseFn=int(__stdcall*)(HANDLE);
using SetProgressFn=void(__stdcall*)(HANDLE,int(__stdcall*)(wchar_t*,int));
using WimCreateFn=HANDLE(WINAPI*)(PCWSTR,DWORD,DWORD,DWORD,DWORD,PDWORD);
using WimCaptureFn=HANDLE(WINAPI*)(HANDLE,PCWSTR,DWORD);
using WimTempFn=BOOL(WINAPI*)(HANDLE,PCWSTR);
using WimCloseFn=BOOL(WINAPI*)(HANDLE);

template<typename F>
F bind(HMODULE library,const char* export_name) {
    return reinterpret_cast<F>(GetProcAddress(library,export_name));
}
void write(const fs::path& filename,const std::string& data) {
    fs::create_directories(filename.parent_path());
    std::ofstream file(filename,std::ios::binary);
    file.write(data.data(),static_cast<std::streamsize>(data.size()));
    if(!file)throw std::runtime_error("cannot create synthetic WIM test file");
}
std::string load(const fs::path& filename) {
    std::ifstream file(filename,std::ios::binary);
    return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=2) return 1;
    try {
        const auto root=fs::temp_directory_path()/
            (L"iso-wcx-wim-smoke-"+std::to_wstring(GetCurrentProcessId())+L"-"+
             std::to_wstring(GetTickCount64()));
        fs::create_directories(root);
        const auto first=root/L"first";
        const auto second=root/L"second";
        write(first/L"HELLO.TXT","hello-one");
        write(first/L"Nested"/L"folder.bin","nested-data");
        fs::create_directories(first/L"Empty");
        write(second/L"HELLO.TXT","hello-two");
        write(second/L"SECOND.TXT","second-image");
        HMODULE system=LoadLibraryExW(L"wimgapi.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!system) return 2;
        const auto create=bind<WimCreateFn>(system,"WIMCreateFile");
        const auto set_temp=bind<WimTempFn>(system,"WIMSetTemporaryPath");
        const auto capture=bind<WimCaptureFn>(system,"WIMCaptureImage");
        const auto close_wim=bind<WimCloseFn>(system,"WIMCloseHandle");
        if(!create||!set_temp||!capture||!close_wim)return 3;
        const auto original=root/L"test.wim";
        DWORD creation=0;
        HANDLE archive=create(original.c_str(),GENERIC_READ|GENERIC_WRITE,
                              CREATE_ALWAYS,0,2u,&creation);
        if(!archive||archive==INVALID_HANDLE_VALUE)return 4;
        if(!set_temp(archive,root.c_str())){close_wim(archive);return 5;}
        for(const auto& folder:{first,second}) {
            const HANDLE image=capture(archive,folder.c_str(),0);
            if(!image||image==INVALID_HANDLE_VALUE){
                std::cerr<<"WIMCaptureImage failed: "<<GetLastError()<<"\n";
                close_wim(archive);return 6;
            }
            close_wim(image);
        }
        close_wim(archive);

        const HMODULE dll=LoadLibraryW(argv[1]);
        if(!dll)return 7;
        const auto open=bind<OpenFn>(dll,"OpenArchiveW");
        const auto read=bind<ReadFn>(dll,"ReadHeaderExW");
        const auto process=bind<ProcessFn>(dll,"ProcessFileW");
        const auto close=bind<CloseFn>(dll,"CloseArchive");
        if(!open||!read||!process||!close)return 8;
        auto verify=[&](const fs::path& file) {
            std::wstring path=file.wstring();
            OpenW args{};
            args.ArcName=path.data();
            const HANDLE handle=open(&args);
            if(!handle||args.OpenResult!=0) {
                std::cerr<<"WCX cannot open WIM "<<GetLastError()<<"\n";
                return false;
            }
            std::set<std::wstring> names;
            for(unsigned i=0;i<100000;i++) {
                HeaderExW header{};
                const int status=read(handle,&header);
                if(status==10)break;
                if(status!=0){close(handle);return false;}
                const std::wstring name(header.FileName);
                names.insert(name);
                if(name==L"image1\\HELLO.TXT"||
                   name==L"image1\\Nested\\folder.bin"||
                   name==L"image2\\HELLO.TXT"||
                   name==L"image2\\SECOND.TXT") {
                    const fs::path output=root/L"extracted"/fs::path(name);
                    fs::create_directories(output.parent_path());
                    std::wstring target=output.wstring();
                    if(process(handle,2,nullptr,target.data())!=0){
                        std::cerr<<"WCX WIM extraction failed for file\n";
                        close(handle);return false;
                    }
                }else if(process(handle,0,nullptr,nullptr)!=0){
                    close(handle);return false;
                }
            }
            close(handle);
            if(names.count(L"image1")==0 || names.count(L"image2")==0 ||
               names.count(L"image1\\Empty")==0 ||
               names.count(L"image1\\Nested\\folder.bin")==0 ||
               names.count(L"image2\\SECOND.TXT")==0)return false;
            if(load(root/L"extracted"/L"image1"/L"HELLO.TXT")!="hello-one" ||
               load(root/L"extracted"/L"image1"/L"Nested"/L"folder.bin")!="nested-data"||
               load(root/L"extracted"/L"image2"/L"HELLO.TXT")!="hello-two" ||
               load(root/L"extracted"/L"image2"/L"SECOND.TXT")!="second-image")
                return false;
            return true;
        };
        if(!verify(original))return 9;
        // The .esd association must recognize WIM-compatible containers.
        const auto esd=root/L"test.esd";
        fs::copy_file(original,esd);
        if(!verify(esd))return 10;
        // Malformed files must not be reported as a valid WIM archive.
        const auto bad=root/L"corrupt.wim";
        write(bad,"NOT A WIM");
        std::wstring badpath=bad.wstring();
        OpenW args{};
        args.ArcName=badpath.data();
        HANDLE broken=open(&args);
        if(broken){close(broken);return 11;}
        std::error_code ec;
        fs::remove_all(root,ec);
        std::cout<<"PASS: Windows WIMGAPI multi-index LZX WIM and ESD through WCX x86/x64 API\n";
        return 0;
    }catch(const std::exception& e){
        std::cerr<<"WIM WCX test error: "<<e.what()<<"\n";
        return 12;
    }
}
#endif
