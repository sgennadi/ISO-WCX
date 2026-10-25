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
#include <atomic>
#include <thread>
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
using PackFn=int(__stdcall*)(wchar_t*,wchar_t*,wchar_t*,wchar_t*,int);
using DeleteFn=int(__stdcall*)(wchar_t*,wchar_t*);
using OpenFn=HANDLE(__stdcall*)(OpenW*);
using ReadFn=int(__stdcall*)(HANDLE,HeaderExW*);
using ProcessFn=int(__stdcall*)(HANDLE,int,wchar_t*,wchar_t*);
using CloseFn=int(__stdcall*)(HANDLE);
using SetProgressFn=void(__stdcall*)(HANDLE,int(__stdcall*)(wchar_t*,int));
using WimCreateFn=HANDLE(WINAPI*)(PCWSTR,DWORD,DWORD,DWORD,DWORD,PDWORD);
using WimCaptureFn=HANDLE(WINAPI*)(HANDLE,PCWSTR,DWORD);
using WimTempFn=BOOL(WINAPI*)(HANDLE,PCWSTR);
using WimSplitFn=BOOL(WINAPI*)(HANDLE,PCWSTR,PLARGE_INTEGER,DWORD);
using WimCloseFn=BOOL(WINAPI*)(HANDLE);
static std::atomic<unsigned> pack_progress_calls{0};
static int __stdcall count_pack_progress(wchar_t*,int) {
    ++pack_progress_calls;
    return 1;
}

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
        const auto mark=[](const char* stage) {
            std::cerr<<"[WIM-DIAG] "<<stage<<std::endl;
        };
        mark("started");
        const auto root=fs::temp_directory_path()/
            (L"iso-wcx-wim-smoke-"+std::to_wstring(GetCurrentProcessId())+L"-"+
             std::to_wstring(GetTickCount64()));
        fs::create_directories(root);
        const auto first=root/L"first";
        const auto second=root/L"second";
        write(first/L"HELLO.TXT","hello-one");
        write(first/L"EmptyFile.bin","");
        write(first/L"Nested"/L"folder.bin","nested-data");
        fs::create_directories(first/L"Empty");
        write(second/L"HELLO.TXT","hello-two");
        write(second/L"SECOND.TXT","second-image");
        // Incompressible distinct resources force multiple SWM parts.
        std::vector<std::string> payloads;
        std::uint32_t random=0x13579bdf;
        for(unsigned n=0;n<4;++n) {
            std::string data(1024*1024,'\0');
            for(auto& c:data) { random^=random<<13;random^=random>>17;random^=random<<5;c=static_cast<char>(random); }
            write(first/(L"payload"+std::to_wstring(n)+L".bin"),data);
            payloads.push_back(std::move(data));
        }
        HMODULE system=LoadLibraryExW(L"wimgapi.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!system) return 2;
        const auto create=bind<WimCreateFn>(system,"WIMCreateFile");
        const auto set_temp=bind<WimTempFn>(system,"WIMSetTemporaryPath");
        const auto capture=bind<WimCaptureFn>(system,"WIMCaptureImage");
        const auto close_wim=bind<WimCloseFn>(system,"WIMCloseHandle");
        if(!create||!set_temp||!capture||!close_wim)return 3;
        const auto split=bind<WimSplitFn>(system,"WIMSplitFile");
        if(!split)return 13;
        mark("fixtures-written");
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

        mark("wimgapi-capture-complete");
        const HMODULE dll=LoadLibraryW(argv[1]);
        if(!dll)return 7;
        const auto open=bind<OpenFn>(dll,"OpenArchiveW");
        const auto read=bind<ReadFn>(dll,"ReadHeaderExW");
        const auto process=bind<ProcessFn>(dll,"ProcessFileW");
        const auto close=bind<CloseFn>(dll,"CloseArchive");
        if(!open||!read||!process||!close)return 8;
        auto verify=[&](const fs::path& file) {
            fs::remove_all(root/L"extracted");
            std::wstring path=file.wstring();
            OpenW args{};
            args.ArcName=path.data();
            mark("verify-open-call");
            const HANDLE handle=open(&args);
            mark("verify-open-return");
            if(!handle||args.OpenResult!=0) {
                std::cerr<<"WCX cannot open WIM "<<GetLastError()<<"\n";
                return false;
            }
            std::set<std::wstring> names;
            for(unsigned i=0;i<100000;i++) {
                HeaderExW header{};
                if(i<4)mark("verify-read-call");
                const int status=read(handle,&header);
                if(i<4)mark("verify-read-return");
                if(status==10)break;
                if(status!=0){close(handle);return false;}
                const std::wstring name(header.FileName);
                names.insert(name);
                if(name.rfind(L"image1\\payload",0)==0||
                   name==L"image1\\HELLO.TXT"||
                   name==L"image1\\Nested\\folder.bin"||
                   name==L"image2\\HELLO.TXT"||
                   name==L"image2\\SECOND.TXT"||
                   name==L"image1\\EmptyFile.bin") {
                    const fs::path output=root/L"extracted"/fs::path(name);
                    fs::create_directories(output.parent_path());
                    std::wstring target=output.wstring();
                    if(i<12)mark("verify-extract-call");
                    if(process(handle,2,nullptr,target.data())!=0){
                        std::cerr<<"WCX WIM extraction failed for file\n";
                        close(handle);return false;
                    }
                    if(i<12)mark("verify-extract-return");
                }else if(process(handle,0,nullptr,nullptr)!=0){
                    close(handle);return false;
                }
            }
            mark("verify-close-call");
            close(handle);
            mark("verify-close-return");
            if(names.count(L"image1")==0 || names.count(L"image2")==0 ||
               names.count(L"image1\\Empty")==0 ||
               names.count(L"image1\\Nested\\folder.bin")==0 ||
               names.count(L"image1\\EmptyFile.bin")==0 ||
               names.count(L"image2\\SECOND.TXT")==0)return false;
            if(load(root/L"extracted"/L"image1"/L"HELLO.TXT")!="hello-one" ||
               load(root/L"extracted"/L"image1"/L"Nested"/L"folder.bin")!="nested-data"||
               load(root/L"extracted"/L"image1"/L"EmptyFile.bin")!="" ||
               load(root/L"extracted"/L"image2"/L"HELLO.TXT")!="hello-two" ||
               load(root/L"extracted"/L"image2"/L"SECOND.TXT")!="second-image")
                return false;
            for(unsigned n=0;n<payloads.size();++n)
                if(load(root/L"extracted"/L"image1"/(L"payload"+std::to_wstring(n)+L".bin"))!=payloads[n])return false;
            return true;
        };
        mark("native-original-verify-start");
        if(!verify(original))return 9;
        mark("native-original-verify-complete");
        // The .esd association must recognize WIM-compatible containers.
        const auto esd=root/L"test.esd";
        fs::copy_file(original,esd);
        if(!verify(esd))return 10;
        mark("esd-verify-complete");
        // Generate real split resources, then open both first and later parts.
        const auto swm=root/L"split"/L"install.swm";
        fs::create_directories(swm.parent_path());
        mark("swm-split-start");
        archive=create(original.c_str(),GENERIC_READ,OPEN_EXISTING,0,0,&creation);
        if(!archive||archive==INVALID_HANDLE_VALUE)return 14;
        LARGE_INTEGER size{};size.QuadPart=512*1024;
        if(!split(archive,swm.c_str(),&size,0)) {
            std::cerr<<"WIMSplitFile failed: "<<GetLastError()<<"\n";
            close_wim(archive);return 15;
        }
        close_wim(archive);
        const auto later=swm.parent_path()/L"install2.swm";
        if(!fs::exists(later)||!verify(swm)||!verify(later))return 16;
        mark("swm-verify-complete");
        auto reject=[&](const fs::path& p) {
            std::wstring name=p.wstring();OpenW a{};a.ArcName=name.data();
            const auto h=open(&a);if(h){close(h);return false;}return a.OpenResult!=0;
        };
        const auto hidden=root/L"hidden.part";
        fs::rename(later,hidden);
        if(!reject(swm))return 17;
        fs::rename(hidden,later);
        const auto duplicate=swm.parent_path()/L"duplicate.swm";
        fs::copy_file(later,duplicate);
        if(!reject(swm))return 18;
        fs::remove(duplicate);
        // Names do not determine membership or order.
        const auto renamed=swm.parent_path()/L"renamed.SWM";
        fs::rename(later,renamed);
        if(!verify(renamed)||!verify(swm.parent_path()/L"RENAMED.swm"))return 19;
        mark("swm-renames-complete");
        mark("wim-write-start");
        // Transactional WIM changes through the real WCX ABI.
        auto pack=bind<PackFn>(dll,"PackFilesW");auto erase=bind<DeleteFn>(dll,"DeleteFilesW");
        auto set_progress=bind<SetProgressFn>(dll,"SetProcessDataProcW");
        if(!pack||!erase||!set_progress)return 20;
        const auto incoming=root/L"incoming";
        write(incoming/L"HELLO.TXT","replacement");write(incoming/L"ADDED.TXT","new-file");
        std::wstring packed=original.wstring(),src=incoming.wstring(),sub=L"image1";
        wchar_t add_list[]=L"HELLO.TXT\0ADDED.TXT\0\0";
        // WCX callback registration may happen on another thread than packing.
        set_progress(INVALID_HANDLE_VALUE,&count_pack_progress);
        int packed_result=-1;
        std::thread worker([&]{packed_result=pack(packed.data(),sub.data(),src.data(),add_list,0);});
        worker.join();
        set_progress(INVALID_HANDLE_VALUE,nullptr);
        if(packed_result!=0){std::cerr<<"WIM PackFilesW returned "<<packed_result<<"\n";return 21;}
        mark("wim-pack-complete");
        if(!pack_progress_calls.load())return 26;
        auto content=[&](const std::wstring& wanted,std::string& data){
            OpenW a{};a.ArcName=packed.data();HANDLE h=open(&a);if(!h)return false;
            for(;;){HeaderExW head{};const int r=read(h,&head);if(r==10){close(h);return false;}if(r){close(h);return false;}
                if(wanted==head.FileName){auto target=(root/L"readback.bin").wstring();const auto result=process(h,2,nullptr,target.data());close(h);if(result)return false;data=load(target);return true;}
                if(process(h,0,nullptr,nullptr)){close(h);return false;}}
        };
        std::string data;
        if(!content(L"image1\\HELLO.TXT",data)||data!="replacement"||
           !content(L"image1\\ADDED.TXT",data)||data!="new-file"||
           !content(L"image2\\HELLO.TXT",data)||data!="hello-two")return 22;
        mark("wim-readback-complete");
        wchar_t delete_list[]=L"image1\\ADDED.TXT\0\0";
        if(erase(packed.data(),delete_list)||content(L"image1\\ADDED.TXT",data))return 23;
        // A rejected traversal must not change one byte of the original.
        const auto original_bytes=load(original);wchar_t bad_list[]=L"..\\escape\0\0";
        if(pack(packed.data(),sub.data(),src.data(),bad_list,0)==0||load(original)!=original_bytes)return 24;
        wchar_t delete_index[]=L"image1\0\0";
        if(erase(packed.data(),delete_index)||!content(L"image1\\HELLO.TXT",data)||data!="hello-two"||content(L"image2\\HELLO.TXT",data))return 25;
        // Malformed files must not be reported as a valid WIM archive.
        const auto bad=root/L"corrupt.wim";
        write(bad,"NOT A WIM");
        std::wstring badpath=bad.wstring();
        OpenW args{};
        args.ArcName=badpath.data();
        HANDLE broken=open(&args);
        if(broken){close(broken);return 11;}
        mark("wim-actions-complete");
        std::error_code ec;
        fs::remove_all(root,ec);
        mark("cleanup-complete");
        std::cout<<"PASS: Windows WIMGAPI multi-index LZX WIM, ESD and split SWM through WCX x86/x64 API\n";
        return 0;
    }catch(const std::exception& e){
        std::cerr<<"WIM WCX test error: "<<e.what()<<"\n";
        return 12;
    }
}
#endif

