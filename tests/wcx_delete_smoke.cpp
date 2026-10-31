#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;
using PackFn = int (__stdcall*)(wchar_t*,wchar_t*,wchar_t*,wchar_t*,int);
using DeleteWFn = int (__stdcall*)(wchar_t*,wchar_t*);
using DeleteAFn = int (__stdcall*)(char*,char*);
using CapsFn = int (__stdcall*)();
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
using OpenFn = HANDLE (__stdcall*)(OpenW*);
using HeaderFn = int (__stdcall*)(HANDLE,HeaderExW*);
using ProcessFn = int (__stdcall*)(HANDLE,int,wchar_t*,wchar_t*);
using CloseFn = int (__stdcall*)(HANDLE);

static void need(bool ok,const char* why) {
    if(!ok)throw std::runtime_error(why);
}
static void both(Bytes& b,std::size_t at,std::uint32_t number,unsigned bytes=4) {
    for(unsigned i=0;i<bytes;++i) {
        b.at(at+i)=static_cast<std::uint8_t>(number>>(i*8));
        b.at(at+bytes+i)=static_cast<std::uint8_t>(
            number>>(8*(bytes-1-i)));
    }
}
static Bytes record(std::uint32_t lba,std::uint32_t size,
                    std::uint8_t flags,const std::string& name) {
    Bytes r(33+name.size()+(name.size()%2==0?1u:0u),0);
    r[0]=static_cast<std::uint8_t>(r.size());
    both(r,2,lba);both(r,10,size);
    r[25]=flags;
    both(r,28,1,2);
    r[32]=static_cast<std::uint8_t>(name.size());
    std::memcpy(r.data()+33,name.data(),name.size());
    return r;
}
static Bytes fixture() {
    constexpr std::size_t sector=2048;
    Bytes b(36*sector,0);
    const auto p=16*sector;
    b[p]=1;
    std::memcpy(b.data()+p+1,"CD001",5);
    b[p+6]=1;
    both(b,p+80,36);
    both(b,p+120,1,2);
    both(b,p+124,1,2);
    both(b,p+128,2048,2);
    b[p+881]=1;
    const auto root=record(20,2048,2,std::string(1,'\0'));
    need(root.size()==34,"ISO root record is not 34 bytes");
    std::copy(root.begin(),root.end(),b.begin()+p+156);
    b[17*sector]=255;
    std::memcpy(b.data()+17*sector+1,"CD001",5);
    b[17*sector+6]=1;
    std::size_t offset=20*sector;
    for(const auto& r:{root,record(20,2048,2,std::string(1,'\1')),
                                record(21,5,0,"HELLO.TXT;1")}) {
        std::copy(r.begin(),r.end(),b.begin()+offset);
        offset+=r.size();
    }
    std::memcpy(b.data()+21*sector,"world",5);
    return b;
}
static void save(const fs::path& file,const Bytes& data) {
    std::ofstream out(file,std::ios::binary|std::ios::trunc);
    need(bool(out),"cannot create ISO fixture");
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    need(bool(out),"cannot write ISO fixture");
}
static Bytes load(const fs::path& file) {
    std::ifstream in(file,std::ios::binary);
    need(bool(in),"cannot reopen ISO fixture");
    return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>{}};
}
static void write_source(const fs::path& path,const std::string& data) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path,std::ios::binary);
    need(bool(out),"cannot create source input");
    out.write(data.data(),static_cast<std::streamsize>(data.size()));
    need(bool(out),"cannot write source input");
}
static std::set<std::wstring> list_names(const fs::path& image,OpenFn open,
                                        HeaderFn header,ProcessFn process,
                                        CloseFn close) {
    auto path=image.wstring();
    OpenW options{};
    options.ArcName=path.data();
    const HANDLE handle=open(&options);
    need(handle && options.OpenResult==0,"WCX cannot open edited ISO");
    std::set<std::wstring> result;
    try {
        bool reached_end=false;
        for(unsigned i=0;i<500;++i) {
            HeaderExW item{};
            const int code=header(handle,&item);
            if(code==10){reached_end=true;break;}
            need(code==0,"WCX ReadHeaderExW cannot list edited ISO");
            need(result.insert(item.FileName).second,
                 "WCX returned duplicate names after edit");
            need(process(handle,0,nullptr,nullptr)==0,
                 "WCX ProcessFileW PK_SKIP failed");
        }
        need(reached_end,"WCX edited ISO did not reach end of listing");
    }catch(...) {
        close(handle);
        throw;
    }
    close(handle);
    return result;
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=2)return 1;
    const fs::path root=fs::temp_directory_path()/
        (L"iso-wcx-delete-abi-"+std::to_wstring(GetCurrentProcessId()));
    try {
        const HMODULE dll=LoadLibraryW(fs::absolute(argv[1]).c_str());
        need(dll!=nullptr,"cannot load same-architecture WCX module");
        const auto sym=[&](const char* name)->FARPROC {
            const auto result=GetProcAddress(dll,name);
            need(result!=nullptr,"missing WCX ABI export");
            return result;
        };
        const auto pack=reinterpret_cast<PackFn>(sym("PackFilesW"));
        const auto delW=reinterpret_cast<DeleteWFn>(sym("DeleteFilesW"));
        const auto delA=reinterpret_cast<DeleteAFn>(sym("DeleteFiles"));
        const auto caps=reinterpret_cast<CapsFn>(sym("GetPackerCaps"));
        const auto open=reinterpret_cast<OpenFn>(sym("OpenArchiveW"));
        const auto header=reinterpret_cast<HeaderFn>(sym("ReadHeaderExW"));
        const auto process=reinterpret_cast<ProcessFn>(sym("ProcessFileW"));
        const auto close=reinterpret_cast<CloseFn>(sym("CloseArchive"));
        need((caps()&8)!=0,"GetPackerCaps omitted delete capability");

        fs::create_directories(root);
        const auto image=root/L"edited.iso";
        save(image,fixture());
        const auto input=root/L"source";
        write_source(input/L"HELLO.TXT","top-level-file");
        write_source(input/L"DOCS"/L"ONE.TXT","nested-file");
        auto arc=image.wstring(),src=input.wstring();
        std::wstring sub;
        wchar_t add_list[]=L"HELLO.TXT\0DOCS\\ONE.TXT\0\0";
        need(pack(arc.data(),sub.data(),src.data(),add_list,0)==0,
             "PackFilesW cannot add test ISO contents");
        auto names=list_names(image,open,header,process,close);
        need(names.contains(L"HELLO.TXT") &&
             names.contains(L"DOCS") &&
             names.contains(L"DOCS\\ONE.TXT"),
             "WCX failed to list packed test files");

        // Idempotent native PackFilesW mkdir must not rebuild a complete
        // ISO or append redundant directory/path table sectors.
        const auto before_repeat=load(image);
        wchar_t repeat_folder[]=L"DOCS\0\0";
        need(pack(arc.data(),sub.data(),src.data(),repeat_folder,0)==0 &&
             load(image)==before_repeat,
             "PackFilesW remastered ISO for an already existing folder");

        // TC sometimes supplies both a folder and its children in the same
        // NUL-separated DeleteFilesW request. One parent delete must win.
        wchar_t recursive[]=L"DOCS\\ONE.TXT\0DOCS\\*.*\0\0";
        need(delW(arc.data(),recursive)==0,
             "recursive directory deletion returned an error");
        names=list_names(image,open,header,process,close);
        need(names.contains(L"HELLO.TXT") &&
             !names.contains(L"DOCS") &&
             !names.contains(L"DOCS\\ONE.TXT"),
             "DeleteFilesW left removed directory entries behind");

        const auto prior=load(image);
        wchar_t no_names[]=L"\0";
        need(delW(arc.data(),no_names)==22 &&
             load(image)==prior,
             "empty DeleteFilesW incorrectly succeeded or mutated ISO");
        wchar_t missing[]=L"MISSING.TXT\0\0";
        need(delW(arc.data(),missing)==24 &&
             load(image)==prior,
             "missing file silently succeeded or changed source ISO");

        // Repeating the F8 selection after deleting the directory must
        // NOT report a successful ISO rewrite. The terminal wildcard is
        // only a directory selection token, never a filename glob.
        wchar_t stale_folder[]=L"DOCS\\*.*\0\0";
        need(delW(arc.data(),stale_folder)==24 &&
             load(image)==prior,
             "stale ISO directory F8 delete changed the archive or succeeded");
        wchar_t unsafe_glob[]=L"FOO*BAR\\ONE.TXT\0\0";
        need(delW(arc.data(),unsafe_glob)==24 &&
             load(image)==prior,
             "ISO wildcard inside a filename bypassed edit path validation");

        // TC's ANSI ABI is still used by older callers.
        std::string mbcs=image.string();
        char ansi_name[]="HELLO.TXT\0\0";
        need(delA(mbcs.data(),ansi_name)==0,
             "ANSI DeleteFiles could not remove top-level file");
        names=list_names(image,open,header,process,close);
        need(!names.contains(L"HELLO.TXT"),"ANSI DeleteFiles left file visible");

        // If both ISO9660 and UDF namespaces are present, deleting only
        // one view is unsafe. The editor must refuse to touch the file.
        auto bridge=fixture();
        bridge[22*2048+1]='B';
        std::memcpy(bridge.data()+22*2048+1,"BEA01",5);
        save(image,bridge);
        arc=image.wstring();
        wchar_t original_name[]=L"HELLO.TXT\0\0";
        need(delW(arc.data(),original_name)==24 &&
             load(image)==bridge,
             "unsupported ISO/UDF bridge deletion modified the image");

        FreeLibrary(dll);
        fs::remove_all(root);
        std::cout<<"PASS: native WCX x86/x64 pack + Unicode/ANSI delete, "
                    "recursive list, no-op refusal, bridge integrity\n";
        return 0;
    }catch(const std::exception& error) {
        std::cerr<<"FAIL: WCX deletion regression: "<<error.what()
                 <<"\nPrivate fixture: "<<root.string()<<"\n";
        return 2;
    }
}
#endif
