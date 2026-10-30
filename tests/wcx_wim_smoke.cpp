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
using CanHandleFn=int(__stdcall*)(wchar_t*);
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
static std::atomic<unsigned> wim_recovery_notes_seen{0};
static fs::path wim_recovery_test_root;
static std::atomic<bool> cancel_fast_mounted_edit{false};
static std::atomic<bool> cancel_fast_after_commit{false};
static std::atomic<bool> cancel_next_wim{false};

// The Preview settings test must not leave any per-user WIM configuration
// changed on its Windows runner, even when a regression fails.
struct ScopedFastMountPreference {
    HKEY key=nullptr;
    DWORD previous_type=0;
    std::vector<BYTE> previous_data;
    bool existed=false;
    ScopedFastMountPreference() {
        const auto opened=RegCreateKeyExW(
            HKEY_CURRENT_USER,L"Software\\ISO-WCX\\WIM",0,nullptr,0,
            KEY_QUERY_VALUE|KEY_SET_VALUE,nullptr,&key,nullptr);
        if(opened!=ERROR_SUCCESS)
            throw std::runtime_error("cannot create isolated WIM test setting");
        DWORD bytes=0;
        const auto found=RegQueryValueExW(
            key,L"FastMountFolders",nullptr,&previous_type,nullptr,&bytes);
        if(found==ERROR_FILE_NOT_FOUND)return;
        if(found!=ERROR_SUCCESS || bytes>4096)
            throw std::runtime_error("cannot preserve existing WIM setting");
        previous_data.resize(bytes);
        if(RegQueryValueExW(key,L"FastMountFolders",nullptr,&previous_type,
           previous_data.data(),&bytes)!=ERROR_SUCCESS)
            throw std::runtime_error("cannot read existing WIM setting");
        existed=true;
    }
    ScopedFastMountPreference(const ScopedFastMountPreference&)=delete;
    ScopedFastMountPreference& operator=(const ScopedFastMountPreference&)=delete;
    void set(DWORD value) {
        if(RegSetValueExW(key,L"FastMountFolders",0,REG_DWORD,
           reinterpret_cast<const BYTE*>(&value),sizeof(value))!=ERROR_SUCCESS)
            throw std::runtime_error("cannot set WIM fast mode test preference");
    }
    ~ScopedFastMountPreference() {
        if(!key)return;
        if(existed)
            (void)RegSetValueExW(key,L"FastMountFolders",0,previous_type,
                 previous_data.data(),static_cast<DWORD>(previous_data.size()));
        else
            (void)RegDeleteValueW(key,L"FastMountFolders");
        RegCloseKey(key);
    }
};

static int __stdcall count_pack_progress(wchar_t* stage,int size) {
    ++pack_progress_calls;
    if(size<0){
        ++wim_percent_calls;
        if(stage && std::wstring_view(stage).starts_with(L"WIM: "))
            ++wim_stage_calls;
        if(stage && std::wstring_view(stage).find(
                L"Create WIM folders without applying image") !=
                std::wstring_view::npos) {
            ++wim_fast_mount_stages;
            // The callback runs AFTER native WIMMountImageHandle, allowing
            // a direct assertion that the recovery note was made durable
            // before the Windows driver began mounting the image.
            if(!wim_recovery_test_root.empty()) {
                try {
                    for(const auto& item:fs::directory_iterator(
                            wim_recovery_test_root)) {
                        if(!item.path().filename().wstring().starts_with(
                                L".iso-wcx-edit-"))continue;
                        const auto note=item.path()/L"WIM-MOUNT-RECOVERY.txt";
                        if(!fs::is_regular_file(note))continue;
                        std::ifstream in(note,std::ios::binary);
                        const std::string contents{
                            std::istreambuf_iterator<char>(in),
                            std::istreambuf_iterator<char>()};
                        if(contents.find("Original WIM: ")!=std::string::npos &&
                           contents.find("Staged WIM: ")!=std::string::npos &&
                           contents.find("Mount directory: ")!=std::string::npos &&
                           contents.find("dism /Get-MountedImageInfo")!=
                               std::string::npos) {
                            ++wim_recovery_notes_seen;
                            break;
                        }
                    }
                }catch(...) {
                    // C callbacks must never let C++ exceptions escape.
                }
            }
        }
        if(cancel_next_wim.load() && stage &&
           std::wstring_view(stage).find(L"Extract WIM image")!=std::wstring_view::npos)
            return 0;
        if(cancel_fast_mounted_edit.load() && stage &&
           std::wstring_view(stage).find(L"Create WIM folders without applying image")!=
               std::wstring_view::npos)
            return 0;
        if(cancel_fast_after_commit.load() && stage &&
           std::wstring_view(stage).find(L"Verify WIM folder changes")!=
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
    const bool fast_mount_test=argc>=3 &&
        std::wstring_view(argv[2])==L"--fast-mount";
    const bool audit_fixture_test=argc==4 &&
        std::wstring_view(argv[2])==L"--audit-fixture";
    // CTest provides the developer tool for both WIM validation paths.
    const bool validate_preflight=(argc==3 && !fast_mount_test) ||
        (argc==4 && fast_mount_test);
    if(argc<2 || argc>4 || (argc==4 && !fast_mount_test &&
                             !audit_fixture_test))return 1;
    const fs::path validator=(validate_preflight || audit_fixture_test) ?
        fs::path(argv[(argc==4)?3:2]) : fs::path();
    try {
        const auto root=fs::temp_directory_path()/
            (L"iso-wcx-wim-smoke-"+std::to_wstring(GetCurrentProcessId())+L"-"+
             std::to_wstring(GetTickCount64()));
        fs::create_directories(root);
        if(fast_mount_test) {
            wim_recovery_test_root=root;
            wim_recovery_notes_seen.store(0);
        }
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

        if(audit_fixture_test) {
            // Independent, READ-ONLY tests on a real system-captured
            // two-index WIM, with no WCX DLL or WIM mount ever invoked.
            // A child process gives a bounded timeout for a wedged WIMGAPI
            // resource read rather than indefinitely blocking CI.
            if(!fs::is_regular_file(validator))return 80;
            const auto original_content=load(original);
            const auto original_time=fs::last_write_time(original);
            // Audit every index and then exercise the selective-index
            // optimization, metadata-only mode, and soft time-budget parser.
            // Each subprocess must finish within the existing watchdog.
            const std::vector<std::wstring> modes={
                L"--audit-only",
                L"--audit-only --full",
                L"--audit-only --stats-only --index 1",
                L"--audit-only --index 2",
                L"--audit-only --full --index 1 --max-seconds 120"
            };
            for(const auto& mode:modes) {
                const auto command=L"\"" + validator.wstring() +
                    L"\" \"" + original.wstring() + L"\" " + mode;
                std::vector<wchar_t> command_line(command.begin(),command.end());
                command_line.push_back(L'\0');
                STARTUPINFOW startup{};
                startup.cb=sizeof(startup);
                PROCESS_INFORMATION process{};
                if(!CreateProcessW(validator.c_str(),command_line.data(),
                                   nullptr,nullptr,FALSE,0,nullptr,
                                   root.c_str(),&startup,&process))
                    return 81;
                CloseHandle(process.hThread);
                const DWORD wait=WaitForSingleObject(process.hProcess,30000);
                if(wait!=WAIT_OBJECT_0) {
                    std::wcerr << L"WIM read-only " << mode
                               << L" audit timed out; inspect native WIMGAPI read"
                               << L" or process teardown\n";
                    TerminateProcess(process.hProcess,82);
                    WaitForSingleObject(process.hProcess,5000);
                    CloseHandle(process.hProcess);
                    return 82;
                }
                DWORD exit_code=0;
                const BOOL got_exit=GetExitCodeProcess(process.hProcess,&exit_code);
                CloseHandle(process.hProcess);
                if(!got_exit || exit_code!=0) {
                    std::wcerr << L"WIM read-only " << mode
                               << L" audit returned " << exit_code << L"\n";
                    return 83;
                }
                if(load(original)!=original_content ||
                   fs::last_write_time(original)!=original_time)
                    return 84; // Original bytes AND NTFS last-write unchanged
                for(const auto& entry:fs::directory_iterator(root))
                    if(entry.path().filename().wstring().starts_with(
                           L".iso-wcx-real-validation-"))
                        return 85; // Audit must never allocate a large clone
            }
            std::error_code cleanup_error;
            fs::remove_all(root,cleanup_error);
            if(cleanup_error)return 86;
            std::cout << "PASS: synthetic two-index WIM metadata, per-index, "
                         "sample/full streaming and soft-budget CLI audit "
                         "without WCX or private clone\n";
            return 0;
        }

        if(validate_preflight) {
            // Run both failure phases against a genuine two-index WIM:
            // (1) invalid image index, before a large clone;
            // (2) a CI-only injected error immediately AFTER full byte-copy
            // and SHA-256 readback, before WIMGAPI/DLL involvement. This
            // isolates pre-WCX cleanup from native WIMGAPI read hangs.
            // Neither may leave a workspace or mutate the original WIM.
            if(!fs::is_regular_file(validator))return 69;
            const auto pristine_bytes=load(original);
            const auto pristine_time=fs::last_write_time(original);
            auto reject_cleanly=[&](const std::wstring& test_argument)->int {
                auto command=L"\""+validator.wstring()+L"\" \""+
                    original.wstring()+L"\" \""+std::wstring(argv[1])+
                    L"\" "+test_argument;
                std::vector<wchar_t> command_line(command.begin(),command.end());
                command_line.push_back(L'\0');
                STARTUPINFOW startup{};
                startup.cb=sizeof(startup);
                PROCESS_INFORMATION process{};
                if(!CreateProcessW(validator.c_str(),command_line.data(),
                                   nullptr,nullptr,FALSE,0,nullptr,
                                   root.c_str(),&startup,&process))
                    return 70;
                CloseHandle(process.hThread);
                const DWORD wait=WaitForSingleObject(process.hProcess,60000);
                if(wait!=WAIT_OBJECT_0) {
                    TerminateProcess(process.hProcess,71);
                    WaitForSingleObject(process.hProcess,5000);
                    CloseHandle(process.hProcess);
                    return 71;
                }
                DWORD exit_code=0;
                const BOOL got_exit=GetExitCodeProcess(process.hProcess,&exit_code);
                CloseHandle(process.hProcess);
                if(!got_exit || exit_code!=2)return 72;
                if(load(original)!=pristine_bytes ||
                   fs::last_write_time(original)!=pristine_time)
                    return 73;
                for(const auto& entry:fs::directory_iterator(root))
                    if(entry.path().filename().wstring().starts_with(
                           L".iso-wcx-real-validation-"))
                        return 74; // Never leave a failed pre-WCX clone.
                return 0;
            };
            if(const int failure=reject_cleanly(L"256"))
                return failure;
            if(const int failure=reject_cleanly(L"--test-fail-after-clone"))
                return failure+5;
        }

        const HMODULE dll=LoadLibraryW(argv[1]);
        if(!dll)return 7;
        const auto open=bind<OpenFn>(dll,"OpenArchiveW");
        const auto read=bind<ReadFn>(dll,"ReadHeaderExW");
        const auto process=bind<ProcessFn>(dll,"ProcessFileW");
        const auto close=bind<CloseFn>(dll,"CloseArchive");
        const auto can_handle=bind<CanHandleFn>(dll,"CanYouHandleThisFileW");
        if(!open||!read||!process||!close||!can_handle)return 8;
        {
            // CanYouHandleThisFile MUST NOT enumerate every image resource
            // before OpenArchive scans them. Validate WIM/ESD by header and
            // reject corrupt inputs without ever launching a WIM browse.
            auto filename=original.wstring();
            if(can_handle(filename.data())!=1)return 75;
            auto fake=(root/L"not-a-wim.wim").wstring();
            write(fake,"NOT A WIM");
            if(can_handle(fake.data())!=0)return 76;
        }
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
        // Total Commander can send an empty folder with the trailing
        // "*.*" wildcard. Previously WCX left the '*' in edit_path()
        // and raised "unsafe archive edit character" instead of deleting.
        wchar_t delete_list[]=L"image1\\NEW_EMPTY_FOLDER\\*.*\0image1\\ADDED.TXT\0\0";
        if(erase(packed.data(),delete_list) ||
           content(L"image1\\ADDED.TXT",data))return 23;
        {
            OpenW args{};args.ArcName=packed.data();
            HANDLE h=open(&args);
            if(!h)return 77;
            bool still_present=false;
            for(;;) {
                HeaderExW item{};
                const int code=read(h,&item);
                if(code==10)break;
                if(code!=0){close(h);return 78;}
                if(std::wstring_view(item.FileName)==L"image1\\NEW_EMPTY_FOLDER")
                    still_present=true;
                if(process(h,0,nullptr,nullptr)!=0){close(h);return 79;}
            }
            close(h);
            if(still_present)return 80;
        }
        // Additional Total Commander recursive-directory spellings may end
        // in a separator and/or contain stacked final \\*.* tokens. Ensure
        // they remove the selected folder, not a wildcard-expanded tree.
        for(const auto& suffix:{L"\\*.*\\",L"\\*\\",L"\\*.*\\*.*"}) {
            fs::create_directories(incoming/L"EMPTY_VARIANT");
            wchar_t make_empty[]=L"EMPTY_VARIANT\0\0";
            std::wstring idx=L"image1";
            if(pack(packed.data(),idx.data(),src.data(),make_empty,0)!=0)
                return 82;
            std::wstring selected=L"image1\\EMPTY_VARIANT";
            selected+=suffix;
            selected.push_back(L'\0');
            selected.push_back(L'\0');
            if(erase(packed.data(),selected.data())!=0)
                return 83;
            OpenW args{};args.ArcName=packed.data();
            HANDLE h=open(&args);
            if(!h)return 84;
            bool found_dir=false;
            for(;;) {
                HeaderExW item{};
                const int rc=read(h,&item);
                if(rc==10)break;
                if(rc){close(h);return 85;}
                if(std::wstring_view(item.FileName)==L"image1\\EMPTY_VARIANT")
                    found_dir=true;
                if(process(h,0,nullptr,nullptr)){close(h);return 86;}
            }
            close(h);
            if(found_dir)return 87;
        }
        // Regression: WIM DeleteFiles used to report success for an absent
        // path because recursive cleanup intentionally accepts a missing
        // target. Never spend a full Apply/Capture claiming to delete nothing
        // and never mutate the original source on such a request.
        wchar_t missing_folder[]=L"image1\\NOT_PRESENT\\*.*\0\0";
        const auto before_missing=load(original);
        if(erase(packed.data(),missing_folder)==0 ||
           load(original)!=before_missing)return 88;
        if(!content(L"image1\\HELLO.TXT",data)||data!="replacement")
            return 89;
        // Unsafe wildcard tokens elsewhere in the path are never globs:
        // they must not change the original WIM on a refused delete.
        wchar_t invalid_wildcard[]=L"image1\\FOO*BAR\0\0";
        const auto before_wildcard=load(original);
        if(erase(packed.data(),invalid_wildcard)==0 ||
           load(original)!=before_wildcard)return 81;
        // A rejected traversal must not change one byte of the original.
        const auto original_bytes=load(original);wchar_t bad_list[]=L"..\\escape\0\0";
        if(pack(packed.data(),sub.data(),src.data(),bad_list,0)==0||load(original)!=original_bytes)return 24;
        if(fast_mount_test) {
            // Exercise the important multi-index case BEFORE deleting image1.
            // Only image2 should gain a new directory; image1 file streams
            // and both index labels must remain intact after native commit.
            fs::create_directories(incoming/L"FAST_SECOND_INDEX");
            wchar_t second_list[]=L"FAST_SECOND_INDEX\0\0";
            std::wstring second_index=L"image2";
            const auto before_second=load(original);
            wim_fast_mount_stages.store(0);
            SetEnvironmentVariableW(L"ISO_WCX_WIM_FAST_MOUNT",L"1");
            set_progress(INVALID_HANDLE_VALUE,&count_pack_progress);
            const int second_result=pack(packed.data(),second_index.data(),
                                         src.data(),second_list,0);
            set_progress(INVALID_HANDLE_VALUE,nullptr);
            SetEnvironmentVariableW(L"ISO_WCX_WIM_FAST_MOUNT",nullptr);
            if(second_result!=0) {
                if(load(original)!=before_second)return 55;
                std::wcerr<<L"Multi-index mount edit failed; preserved "
                          <<root.c_str()<<L"\n";
                return 56;
            }
            if(wim_fast_mount_stages.load()==0)return 57;
            if(!content(L"image1\\HELLO.TXT",data) || data!="replacement" ||
               !content(L"image2\\HELLO.TXT",data) || data!="hello-two")
                return 58;
            std::wstring name=original.wstring();
            OpenW open_new{};
            open_new.ArcName=name.data();
            HANDLE check_archive=open(&open_new);
            if(!check_archive)return 59;
            bool first_index=false,second_index_found=false,new_folder=false;
            for(unsigned n=0;n<100000;++n) {
                HeaderExW entry{};
                const int code=read(check_archive,&entry);
                if(code==10)break;
                if(code!=0){close(check_archive);return 60;}
                const std::wstring_view filename(entry.FileName);
                if(filename==L"image1")first_index=true;
                if(filename==L"image2")second_index_found=true;
                if(filename==L"image2\\FAST_SECOND_INDEX")
                    new_folder=(entry.FileAttr&FILE_ATTRIBUTE_DIRECTORY)!=0;
                if(process(check_archive,0,nullptr,nullptr)!=0) {
                    close(check_archive);return 61;
                }
            }
            close(check_archive);
            if(!first_index || !second_index_found || !new_folder)
                return 62;
        }
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
            if(wim_recovery_notes_seen.load()==0)
                return 68; // Recovery note must exist while WIM is mounted.
            for(const auto& item:fs::directory_iterator(root)) {
                if(item.path().filename().wstring().starts_with(
                       L".iso-wcx-edit-"))
                    return 69; // Successful unmount must remove workspace.
            }
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

            // Cancellation AFTER WIMCommitImageHandle and unmount, but
            // BEFORE EditTransaction::commit replaces the original, must
            // discard the edited staging copy and preserve original bytes.
            fs::create_directories(incoming/L"FAST_ABORT_COMMITTED");
            wchar_t abort_committed_list[]=L"FAST_ABORT_COMMITTED\0\0";
            const auto before_abort_committed=load(original);
            cancel_fast_after_commit.store(true);
            set_progress(INVALID_HANDLE_VALUE,&count_pack_progress);
            SetEnvironmentVariableW(L"ISO_WCX_WIM_FAST_MOUNT",L"1");
            const int late_cancel=pack(packed.data(),index_sub.data(),
                                      src.data(),abort_committed_list,0);
            SetEnvironmentVariableW(L"ISO_WCX_WIM_FAST_MOUNT",nullptr);
            set_progress(INVALID_HANDLE_VALUE,nullptr);
            cancel_fast_after_commit.store(false);
            if(late_cancel!=21 || load(original)!=before_abort_committed)
                return 53;
            for(const auto& item:fs::directory_iterator(root)) {
                const auto name=item.path().filename().wstring();
                if(name.starts_with(L".iso-wcx-edit-"))return 54;
            }

            // Idempotency: making an already existing folder must not mount
            // or rewrite the WIM, and its bytes must remain identical.
            const auto before_noop=load(original);
            wim_fast_mount_stages.store(0);
            set_progress(INVALID_HANDLE_VALUE,&count_pack_progress);
            SetEnvironmentVariableW(L"ISO_WCX_WIM_FAST_MOUNT",L"1");
            const int no_op=pack(packed.data(),index_sub.data(),
                                 src.data(),fast_list,0);
            SetEnvironmentVariableW(L"ISO_WCX_WIM_FAST_MOUNT",nullptr);
            set_progress(INVALID_HANDLE_VALUE,nullptr);
            if(no_op!=0 || load(original)!=before_noop ||
               wim_fast_mount_stages.load()!=0)
                return 52;

            // On case-insensitive NTFS, a different spelling of an existing
            // directory must be rejected before copying a large WIM.
            wchar_t alias_list[]=L"fast_mount_folder\0\0";
            const auto case_before=load(original);
            wim_fast_mount_stages.store(0);
            set_progress(INVALID_HANDLE_VALUE,&count_pack_progress);
            SetEnvironmentVariableW(L"ISO_WCX_WIM_FAST_MOUNT",L"1");
            const int alias_result=pack(packed.data(),index_sub.data(),
                                        src.data(),alias_list,0);
            SetEnvironmentVariableW(L"ISO_WCX_WIM_FAST_MOUNT",nullptr);
            set_progress(INVALID_HANDLE_VALUE,nullptr);
            if(alias_result==0 || load(original)!=case_before ||
               wim_fast_mount_stages.load()!=0)
                return 63;
            for(const auto& item:fs::directory_iterator(root))
                if(item.path().filename().wstring().starts_with(L".iso-wcx-edit-"))
                    return 64;

            // Refuse folder creation UNDER an existing regular file before
            // a staging clone or mount is attempted. This must be a no-write
            // failure even when the caller's source is a valid directory.
            const auto conflict_source=root/L"conflict-src";
            fs::create_directories(conflict_source/L"HELLO.TXT"/L"CHILD");
            wchar_t conflicting_dir[]=L"HELLO.TXT\\CHILD\0\0";
            const auto before_conflict=load(original);
            wim_fast_mount_stages.store(0);
            set_progress(INVALID_HANDLE_VALUE,&count_pack_progress);
            SetEnvironmentVariableW(L"ISO_WCX_WIM_FAST_MOUNT",L"1");
            const int conflict_rc=pack(packed.data(),index_sub.data(),
                                       const_cast<wchar_t*>(conflict_source.c_str()),
                                       conflicting_dir,0);
            SetEnvironmentVariableW(L"ISO_WCX_WIM_FAST_MOUNT",nullptr);
            set_progress(INVALID_HANDLE_VALUE,nullptr);
            if(conflict_rc==0 || load(original)!=before_conflict ||
               wim_fast_mount_stages.load()!=0)
                return 53;
            for(const auto& item:fs::directory_iterator(root))
                if(item.path().filename().wstring().starts_with(L".iso-wcx-edit-"))
                    return 54;
            // Exercise the exact HKCU setting used by ConfigurePacker.
            // No environment variable: the saved option must activate the
            // WIM mount path for a new directory in this image.
            {
                ScopedFastMountPreference setting;
                setting.set(1);
                SetEnvironmentVariableW(L"ISO_WCX_WIM_FAST_MOUNT",nullptr);
                fs::create_directories(incoming/L"GUI_MOUNT_REGISTRY");
                wchar_t gui_entry[]=L"GUI_MOUNT_REGISTRY\0\0";
                const auto before_registry=load(original);
                wim_fast_mount_stages.store(0);
                set_progress(INVALID_HANDLE_VALUE,&count_pack_progress);
                const int gui_rc=pack(packed.data(),index_sub.data(),
                                      src.data(),gui_entry,0);
                set_progress(INVALID_HANDLE_VALUE,nullptr);
                if(gui_rc!=0 || wim_fast_mount_stages.load()==0 ||
                   load(original)==before_registry)
                    return 65;

                // An explicit environment override of 0 must win over
                // the saved HKCU checkbox and force the legacy editor.
                fs::create_directories(incoming/L"GUI_MOUNT_DISABLED");
                wchar_t disabled_entry[]=L"GUI_MOUNT_DISABLED\0\0";
                wim_fast_mount_stages.store(0);
                SetEnvironmentVariableW(L"ISO_WCX_WIM_FAST_MOUNT",L"0");
                set_progress(INVALID_HANDLE_VALUE,&count_pack_progress);
                const int disabled_rc=pack(packed.data(),index_sub.data(),
                                           src.data(),disabled_entry,0);
                set_progress(INVALID_HANDLE_VALUE,nullptr);
                SetEnvironmentVariableW(L"ISO_WCX_WIM_FAST_MOUNT",nullptr);
                if(disabled_rc!=0 || wim_fast_mount_stages.load()!=0)
                    return 66;
            }
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

