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
#include <string_view>
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
static std::atomic<unsigned> wim_percent_calls{0};
static std::atomic<unsigned> wim_stage_calls{0};
static std::atomic<unsigned> wim_fast_mount_stages{0};
static std::atomic<bool> cancel_fast_mounted_edit{false};
static std::atomic<bool> cancel_next_wim{false};
static int __stdcall count_pack_progress(wchar_t* stage,int size) {
    ++pack_progress_calls;
    if(size<0){
        ++wim_percent_calls;
        if(stage && std::wstring_view(stage).starts_with(L"WIM: "))
            ++wim_stage_calls;
        if(stage && std::wstring_view(stage).find(
                L"Create WIM folders without applying image") !=
                std::wstring_view::npos)
            ++wim_fast_mount_stages;
        if(cancel_next_wim.load() && stage &&
           std::wstring_view(stage).find(L"Extract WIM image")!=std::wstring_view::npos)
            return 0;
        if(cancel_fast_mounted_edit.load() && stage &&
           std::wstring_view(stage).find(L"Create WIM folders without applying image")!=
               std::wstring_view::npos)
            return 0;
    }
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
    const bool fast_mount_test=argc==3 &&
        std::wstring_view(argv[2])==L"--fast-mount";
    if(argc!=2 && !fast_mount_test) return 1;
    try {
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
            fs::remove_all(root/L"extracted");
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
                if(name.rfind(L"image1\\payload",0)==0||
                   name==L"image1\\HELLO.TXT"||
                   name==L"image1\\Nested\\folder.bin"||
                   name==L"image2\\HELLO.TXT"||
                   name==L"image2\\SECOND.TXT"||
                   name==L"image1\\EmptyFile.bin") {
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
        if(!verify(original))return 9;
        // The .esd association must recognize WIM-compatible containers.
        const auto esd=root/L"test.esd";
        fs::copy_file(original,esd);
        if(!verify(esd))return 10;
        // Generate real split resources, then open both first and later parts.
        const auto swm=root/L"split"/L"install.swm";
        fs::create_directories(swm.parent_path());
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
        if(!pack_progress_calls.load())return 26;
        // Regression: Total Commander creates an *empty folder* through the
        // same PackFilesW API as a file. Verify the index survives correctly.
        fs::create_directories(incoming/L"NEW_EMPTY_FOLDER");
        wchar_t mkdir_list[]=L"NEW_EMPTY_FOLDER\0\0";
        set_progress(INVALID_HANDLE_VALUE,&count_pack_progress);
        const int folder_rc=pack(packed.data(),sub.data(),src.data(),mkdir_list,0);
        set_progress(INVALID_HANDLE_VALUE,nullptr);
        if(folder_rc!=0)return 27;
        if(wim_percent_calls.load()==0 || wim_stage_calls.load()==0)
            return 32; // Progress must be visible even for empty folders.
        // Cancel during a long apply before replacing the original WIM.
        fs::create_directories(incoming/L"CANCEL_FOLDER");
        wchar_t cancel_list[]=L"CANCEL_FOLDER\0\0";
        const auto before_cancel=load(original);
        cancel_next_wim.store(true);
        set_progress(INVALID_HANDLE_VALUE,&count_pack_progress);
        const int cancel_rc=pack(packed.data(),sub.data(),src.data(),cancel_list,0);
        set_progress(INVALID_HANDLE_VALUE,nullptr);
        cancel_next_wim.store(false);
        if(cancel_rc!=21 || load(original)!=before_cancel)
            return 33; // A cancelled edit must leave WIM byte-identical.
        {
            OpenW args{};args.ArcName=packed.data();
            HANDLE h=open(&args);
            if(!h)return 28;
            bool seen_folder=false;
            for(;;){
                HeaderExW item{};
                const int result=read(h,&item);
                if(result==10)break;
                if(result){close(h);return 29;}
                if(std::wstring_view(item.FileName)==L"image1\\NEW_EMPTY_FOLDER")
                    seen_folder=(item.FileAttr&FILE_ATTRIBUTE_DIRECTORY)!=0;
                if(process(h,0,nullptr,nullptr)){close(h);return 30;}
            }
            close(h);
            if(!seen_folder)return 31;
        }
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
        std::error_code ec;
        if(fast_mount_test) {
            // Disposable Windows integration: actually run the optional
            // WIMMountImageHandle -> WIMCommitImageHandle path on a genuine
            // WIM captured earlier by the system WIMGAPI.
            const auto fast_dir=incoming/L"FAST_MOUNT_FOLDER";
            fs::create_directories(fast_dir);
            wchar_t fast_list[]=L"FAST_MOUNT_FOLDER\0\0";
            std::wstring index_sub=L"image1";
            const auto before_fast=load(original);
            wim_fast_mount_stages.store(0);
            SetEnvironmentVariableW(L"ISO_WCX_WIM_FAST_MOUNT",L"1");
            set_progress(INVALID_HANDLE_VALUE,&count_pack_progress);
            const int fast_rc=pack(packed.data(),index_sub.data(),
                                   src.data(),fast_list,0);
            set_progress(INVALID_HANDLE_VALUE,nullptr);
            SetEnvironmentVariableW(L"ISO_WCX_WIM_FAST_MOUNT",nullptr);
            if(fast_rc!=0) {
                // The plugin may retain a failed mounted staging workspace.
                // Do not recursively delete the fixture tree on failure.
                if(load(original)!=before_fast)return 43;
                std::wcerr<<L"WIM mount Preview failed (code "<<fast_rc
                          <<L"). Preserved test root for recovery: "
                          <<root.c_str()<<L"\n";
                return 44;
            }
            if(wim_fast_mount_stages.load()==0)
                return 45; // Must actually reach the mount edit stage.
            std::wstring verify_path=original.wstring();
            OpenW check_open{};
            check_open.ArcName=verify_path.data();
            const HANDLE mounted_check=open(&check_open);
            if(!mounted_check)return 46;
            bool folder_found=false;
            for(unsigned n=0;n<100000;++n) {
                HeaderExW entry{};
                const int rc=read(mounted_check,&entry);
                if(rc==10)break;
                if(rc!=0){close(mounted_check);return 47;}
                if(std::wstring_view(entry.FileName)==
                      L"image1\\FAST_MOUNT_FOLDER")
                    folder_found=(entry.FileAttr&FILE_ATTRIBUTE_DIRECTORY)!=0;
                if(process(mounted_check,0,nullptr,nullptr)!=0){
                    close(mounted_check);return 48;
                }
            }
            close(mounted_check);
            if(!folder_found || !content(L"image1\\HELLO.TXT",data) ||
               data!="hello-two")return 49;

            // Cancellation AFTER WIMMountImageHandle succeeds but BEFORE
            // a folder is created must unmount and leave the source intact.
            fs::create_directories(incoming/L"FAST_CANCEL_MOUNT");
            wchar_t cancel_fast_list[]=L"FAST_CANCEL_MOUNT\0\0";
            const auto before_cancel_fast=load(original);
            cancel_fast_mounted_edit.store(true);
            set_progress(INVALID_HANDLE_VALUE,&count_pack_progress);
            SetEnvironmentVariableW(L"ISO_WCX_WIM_FAST_MOUNT",L"1");
            const int cancelled_fast=pack(packed.data(),index_sub.data(),
                                          src.data(),cancel_fast_list,0);
            SetEnvironmentVariableW(L"ISO_WCX_WIM_FAST_MOUNT",nullptr);
            set_progress(INVALID_HANDLE_VALUE,nullptr);
            cancel_fast_mounted_edit.store(false);
            if(cancelled_fast!=21 || load(original)!=before_cancel_fast)
                return 50;
            for(const auto& item:fs::directory_iterator(root)) {
                const auto name=item.path().filename().wstring();
                if(name.starts_with(L".iso-wcx-edit-"))return 51;
            }

            // Idempotency: making an already existing folder must not mount
            // or rewrite the WIM, and its bytes must remain identical.
            const auto before_noop=load(original);
            wim_fast_mount_stages.store(0);
            SetEnvironmentVariableW(L"ISO_WCX_WIM_FAST_MOUNT",L"1");
            const int no_op=pack(packed.data(),index_sub.data(),
                                 src.data(),fast_list,0);
            SetEnvironmentVariableW(L"ISO_WCX_WIM_FAST_MOUNT",nullptr);
            if(no_op!=0 || load(original)!=before_noop ||
               wim_fast_mount_stages.load()!=0)
                return 52;
        }
        fs::remove_all(root,ec);
        std::cout<<"PASS: Windows WIMGAPI multi-index LZX WIM, ESD and split SWM through WCX x86/x64 API\n";
        return 0;
    }catch(const std::exception& e){
        std::cerr<<"WIM WCX test error: "<<e.what()<<"\n";
        return 12;
    }
}
#endif

