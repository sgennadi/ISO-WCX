#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// Integration fixture: only Windows system WIMGAPI and Win32 security APIs.
// This developer-only executable is never bundled with the WCX ZIP.
#include <windows.h>
#include <sddl.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs=std::filesystem;
using Create=HANDLE(WINAPI*)(PCWSTR,DWORD,DWORD,DWORD,DWORD,PDWORD);
using SetTemp=BOOL(WINAPI*)(HANDLE,PCWSTR);
using Capture=HANDLE(WINAPI*)(HANDLE,PCWSTR,DWORD);
using Load=HANDLE(WINAPI*)(HANDLE,DWORD);
using Apply=BOOL(WINAPI*)(HANDLE,PCWSTR,DWORD);
using Close=BOOL(WINAPI*)(HANDLE);
using Pack=int(__stdcall*)(wchar_t*,wchar_t*,wchar_t*,wchar_t*,int);
using SetProgress=void(__stdcall*)(HANDLE,int(__stdcall*)(wchar_t*,int));
static std::atomic<unsigned> used_fast_mount{0};

static int __stdcall progress(wchar_t* label,int value) {
    if(value<0 && label &&
       std::wstring_view(label).find(
           L"Create WIM folders without applying image")!=std::wstring_view::npos)
        ++used_fast_mount;
    return 1;
}
static void require(bool valid,const char* msg) {
    if(!valid)throw std::runtime_error(std::string(msg)+
                           " (Win32 "+std::to_string(GetLastError())+")");
}
template<class F>static F bind(HMODULE dll,const char* name) {
    const auto address=GetProcAddress(dll,name);
    require(address!=nullptr,"missing required Win32/WIMGAPI export");
    return reinterpret_cast<F>(address);
}
static void save(const fs::path& file,std::string_view value) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file,std::ios::binary|std::ios::trunc);
    require(bool(out),"cannot create integrity source file or named ADS");
    out.write(value.data(),static_cast<std::streamsize>(value.size()));
    require(bool(out),"cannot flush integrity input bytes");
}
static std::string read(const fs::path& file) {
    std::ifstream in(file,std::ios::binary);
    require(bool(in),"cannot read preserved regular file or named ADS");
    return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>{}};
}
static fs::path ads(const fs::path& file) {
    return fs::path(file.wstring()+L":ISO_WCX_INTEGRITY");
}
static void set_protected_dacl(const fs::path& file) {
    PSECURITY_DESCRIPTOR descriptor=nullptr;
    const auto sddl=L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FR;;;BU)";
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(
               sddl,SDDL_REVISION_1,&descriptor,nullptr)!=FALSE,
            "convert protected DACL fixture");
    const BOOL ok=SetFileSecurityW(file.c_str(),DACL_SECURITY_INFORMATION,
                                   descriptor);
    LocalFree(descriptor);
    require(ok!=FALSE,"assign protected file DACL");
}
static std::wstring dacl(const fs::path& file) {
    DWORD bytes=0;
    SetLastError(ERROR_SUCCESS);
    (void)GetFileSecurityW(file.c_str(),DACL_SECURITY_INFORMATION,
                          nullptr,0,&bytes);
    require(bytes>=sizeof(SECURITY_DESCRIPTOR),"cannot read DACL length");
    std::vector<BYTE> buffer(bytes);
    require(GetFileSecurityW(file.c_str(),DACL_SECURITY_INFORMATION,
                             buffer.data(),bytes,&bytes)!=FALSE,
            "read DACL");
    LPWSTR output=nullptr;
    require(ConvertSecurityDescriptorToStringSecurityDescriptorW(
               buffer.data(),SDDL_REVISION_1,DACL_SECURITY_INFORMATION,
               &output,nullptr)!=FALSE,"convert file DACL to SDDL");
    std::wstring result=output;
    LocalFree(output);
    return result;
}
static std::uint64_t hash(const fs::path& file) {
    std::ifstream input(file,std::ios::binary);
    require(bool(input),"cannot read large WIM integrity file");
    std::array<char,64*1024> bytes{};
    std::uint64_t value=14695981039346656037ull;
    while(input) {
        input.read(bytes.data(),static_cast<std::streamsize>(bytes.size()));
        const auto size=input.gcount();
        for(std::streamsize i=0;i<size;++i) {
            value^=static_cast<unsigned char>(bytes[static_cast<std::size_t>(i)]);
            value*=1099511628211ull;
        }
    }
    require(input.eof() && !input.bad(),"large WIM input could not be hashed");
    return value;
}
static void write_incompressible(const fs::path& file) {
    std::ofstream out(file,std::ios::binary|std::ios::trunc);
    require(bool(out),"cannot create 64-MiB payload");
    std::array<char,64*1024> block{};
    std::uint32_t random=0x71c6d32bu;
    for(unsigned n=0;n<1024;++n) {
        for(auto& item:block) {
            random^=random<<13;
            random^=random>>17;
            random^=random<<5;
            item=static_cast<char>(random);
        }
        out.write(block.data(),static_cast<std::streamsize>(block.size()));
        require(bool(out),"cannot fill 64-MiB payload");
    }
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=2)return 1;
    const auto root=fs::temp_directory_path()/
        (L"iso-wcx-wim-integrity-"+std::to_wstring(GetCurrentProcessId())+L"-"+
         std::to_wstring(GetTickCount64()));
    bool preserve_on_failure=false;
    try {
        fs::create_directories(root);
        const auto source=root/L"source",source2=root/L"source2";
        fs::create_directories(source/L"Protected");
        const auto protected_file=source/L"Protected"/L"restricted.dat";
        const std::string ordinary="Untouched WIM file payload";
        const std::string stream="Named ADS preserved across WIM mounted edits";
        save(protected_file,ordinary);
        save(ads(protected_file),stream);
        set_protected_dacl(protected_file);
        const auto expected_acl=dacl(protected_file);
        require(expected_acl.find(L"D:P")!=std::wstring::npos,
                "protected DACL fixture is not marked protected");
        const auto large=source/L"64MiB-random.bin";
        write_incompressible(large);
        const auto size=fs::file_size(large);
        require(size==64ull*1024ull*1024ull,"large input must be 64 MiB");
        const auto expected_hash=hash(large);
        save(source2/L"second.txt","Second index must be unchanged");
        const auto wim=root/L"integrity.wim";
        const HMODULE dll=LoadLibraryExW(L"wimgapi.dll",nullptr,
                                         LOAD_LIBRARY_SEARCH_SYSTEM32);
        require(dll!=nullptr,"Windows system WIMGAPI unavailable");
        const auto create=bind<Create>(dll,"WIMCreateFile");
        const auto temp=bind<SetTemp>(dll,"WIMSetTemporaryPath");
        const auto capture=bind<Capture>(dll,"WIMCaptureImage");
        const auto load=bind<Load>(dll,"WIMLoadImage");
        const auto apply=bind<Apply>(dll,"WIMApplyImage");
        const auto close=bind<Close>(dll,"WIMCloseHandle");
        DWORD state=0;
        HANDLE container=create(wim.c_str(),GENERIC_READ|GENERIC_WRITE,
                                CREATE_ALWAYS,0,2u,&state);
        require(container && container!=INVALID_HANDLE_VALUE,
                "cannot create two-index integrity WIM");
        require(temp(container,root.c_str())!=FALSE,"set capture scratch");
        for(const auto& input:{source,source2}) {
            HANDLE image=capture(container,input.c_str(),0);
            require(image && image!=INVALID_HANDLE_VALUE,
                    "cannot capture WIM index");
            require(close(image)!=FALSE,"cannot close captured image index");
        }
        require(close(container)!=FALSE,"close captured WIM");

        const auto unpack=[&](const fs::path& target,DWORD index) {
            fs::create_directories(target);
            DWORD opened=0;
            HANDLE archive=create(wim.c_str(),GENERIC_READ,OPEN_EXISTING,
                                  0,0,&opened);
            require(archive && archive!=INVALID_HANDLE_VALUE,
                    "open integrity WIM for independent apply");
            require(temp(archive,root.c_str())!=FALSE,"set WIM apply scratch");
            HANDLE image=load(archive,index);
            require(image && image!=INVALID_HANDLE_VALUE,"load WIM apply index");
            const BOOL ok=apply(image,target.c_str(),0);
            const DWORD error=GetLastError();
            close(image);
            close(archive);
            SetLastError(error);
            require(ok!=FALSE,"independent system WIMApplyImage failed");
        };
        const auto verify=[&](const fs::path& folder) {
            const auto restored=folder/L"Protected"/L"restricted.dat";
            require(read(restored)==ordinary,"regular file content changed");
            require(read(ads(restored))==stream,"NTFS named ADS content changed");
            require(dacl(restored)==expected_acl,
                    "protected NTFS DACL changed across WIM capture or commit");
            const auto restored_large=folder/L"64MiB-random.bin";
            require(fs::file_size(restored_large)==size,"large file length changed");
            require(hash(restored_large)==expected_hash,
                    "64-MiB incompressible WIM file bytes changed");
        };
        unpack(root/L"baseline",1);
        verify(root/L"baseline"); // Baseline proves WIM captured DACL + ADS.

        const HMODULE plugin=LoadLibraryW(argv[1]);
        require(plugin!=nullptr,"cannot load Preview WCX DLL");
        const auto pack=bind<Pack>(plugin,"PackFilesW");
        const auto set_progress=bind<SetProgress>(plugin,"SetProcessDataProcW");
        const auto incoming=root/L"incoming";
        fs::create_directories(incoming/L"FAST_INTEGRITY_FOLDER");
        std::wstring archive_name=wim.wstring(),index_name=L"image1";
        std::wstring source_name=incoming.wstring();
        wchar_t entries[]=L"FAST_INTEGRITY_FOLDER\0\0";
        used_fast_mount.store(0);
        preserve_on_failure=true;
        SetEnvironmentVariableW(L"ISO_WCX_WIM_FAST_MOUNT",L"1");
        set_progress(INVALID_HANDLE_VALUE,&progress);
        const int result=pack(archive_name.data(),index_name.data(),
                              source_name.data(),entries,0);
        set_progress(INVALID_HANDLE_VALUE,nullptr);
        SetEnvironmentVariableW(L"ISO_WCX_WIM_FAST_MOUNT",nullptr);
        require(result==0,"WCX fast WIM folder editing failed");
        require(used_fast_mount.load()>0,"edit bypassed the fast mounted path");
        unpack(root/L"after",1);
        verify(root/L"after");
        require(fs::is_directory(root/L"after"/L"FAST_INTEGRITY_FOLDER"),
                "requested folder is missing after WIM apply");
        unpack(root/L"after2",2);
        require(read(root/L"after2"/L"second.txt")==
                    "Second index must be unchanged",
                "editing first WIM index modified second index contents");
        FreeLibrary(plugin);
        FreeLibrary(dll);
        preserve_on_failure=false;
        std::error_code ec;
        fs::remove_all(root,ec);
        std::cout<<"PASS: 64-MiB LZX WIM fast mount preserved regular files, "
                    "protected DACL, named NTFS ADS and second index\n";
        return 0;
    }catch(const std::exception& e){
        SetEnvironmentVariableW(L"ISO_WCX_WIM_FAST_MOUNT",nullptr);
        std::cerr<<"WIM fast mount ACL/ADS integrity test FAILED: "
                 <<e.what()<<" (fixture "<<root.string()<<")\n";
        if(!preserve_on_failure) {
            std::error_code ec;
            fs::remove_all(root,ec);
        }
        return 2;
    }
}
#endif
