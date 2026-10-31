#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;
using PackFn = int (__stdcall*)(wchar_t*,wchar_t*,wchar_t*,wchar_t*,int);
using DeleteWFn = int (__stdcall*)(wchar_t*,wchar_t*);
using DeleteAFn = int (__stdcall*)(char*,char*);
using CapsFn = int (__stdcall*)();
using SetPackProgressFn = void (__stdcall*)(HANDLE,int (__stdcall*)(wchar_t*,int));
static std::atomic<unsigned> iso_stage_updates{0};
static std::atomic<unsigned> iso_highest_percent{0};
static int __stdcall record_iso_progress(wchar_t*,int size) {
    if(size<0) {
        iso_stage_updates.fetch_add(1,std::memory_order_relaxed);
        const auto percent=static_cast<unsigned>(-size);
        auto previous=iso_highest_percent.load(std::memory_order_relaxed);
        while(percent>previous &&
              !iso_highest_percent.compare_exchange_weak(
                  previous,percent,std::memory_order_relaxed)) {}
    }
    return 1;
}
static int __stdcall cancel_iso_staging(wchar_t* name,int size) {
    (void)name;
    // Fail at a real CopyFileEx clone checkpoint, not only at preflight.
    if(size<=-30 && size>=-75)return 0;
    return 1;
}
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
        const auto setpack=reinterpret_cast<SetPackProgressFn>(
            sym("SetProcessDataProcW"));
        const auto open=reinterpret_cast<OpenFn>(sym("OpenArchiveW"));
        const auto header=reinterpret_cast<HeaderFn>(sym("ReadHeaderExW"));
        const auto process=reinterpret_cast<ProcessFn>(sym("ProcessFileW"));
        const auto close=reinterpret_cast<CloseFn>(sym("CloseArchive"));
        need((caps()&8)!=0,"GetPackerCaps omitted delete capability");

        fs::create_directories(root);
        const auto image=root/L"edited.iso";
        save(image,fixture());
        const auto input=root/L"source";
        // Real TC F5 with the old ISO panel retained. If input and archive
        // entries have equal byte length, replace only the existing extent:
        // no new sectors, directories or volume-descriptor modifications.
        write_source(input/L"HELLO.TXT","NEW!!");
        const auto before_equal=load(image);
        auto fast_arc=image.wstring(),fast_src=input.wstring();
        std::wstring fast_sub;
        wchar_t fast_list[]=L"HELLO.TXT\0\0";
        OpenW fast_panel{};
        fast_panel.ArcName=fast_arc.data();
        const HANDLE fast_handle=open(&fast_panel);
        need(fast_handle && fast_panel.OpenResult==0,
             "ISO fast F5 cannot retain an open TC panel");
        const int fast_result=pack(fast_arc.data(),fast_sub.data(),
                                   fast_src.data(),fast_list,2);
        need(close(fast_handle)==0,"ISO fast F5 CloseArchive failed");
        need(fast_result==0,"real WCX F5 same-size replacement failed");
        const auto after_equal=load(image);
        need(after_equal.size()==before_equal.size() &&
             std::memcmp(after_equal.data()+21*2048,"NEW!!",5)==0 &&
             std::equal(after_equal.begin(),after_equal.begin()+21*2048,
                        before_equal.begin()) &&
             std::equal(after_equal.begin()+21*2048+5,after_equal.end(),
                        before_equal.begin()+21*2048+5),
             "WCX equal-length ISO F5 changed metadata, file layout or size");
        save(image,fixture());
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

        // Real Total Commander F5: source from the opposite panel,
        // target is a nested directory within the ISO, and the common
        // PK_PACK_SAVE_PATHS (bit 2) flag is set. F7/F8 do not test this.
        const auto f5_source=root/L"Incoming ISO files";
        write_source(f5_source/L"COPIED.BIN","native ISO F5 copy bytes");
        write_source(f5_source/L"HELLO.TXT","native ISO F5 overwrite bytes");
        auto f5_dir=f5_source.wstring();
        std::wstring f5_sub=L"DOCS";
        wchar_t f5_list[]=L"COPIED.BIN\0HELLO.TXT\0\0";
        need((caps()&2)!=0,
             "GetPackerCaps omits MODIFY and TC prevents F5 into ISO");
        OpenW retained{};
        retained.ArcName=arc.data();
        const HANDLE original_panel=open(&retained);
        need(original_panel && retained.OpenResult==0,
             "WCX cannot retain an ISO panel while copying via F5");
        iso_stage_updates.store(0);
        iso_highest_percent.store(0);
        setpack(INVALID_HANDLE_VALUE,&record_iso_progress);
        const int f5_status=pack(arc.data(),f5_sub.data(),f5_dir.data(),
                                 f5_list,2);
        setpack(INVALID_HANDLE_VALUE,nullptr);
        need(iso_stage_updates.load()>0 && iso_highest_percent.load()>=90,
             "ISO F5 never published native optical stage progress to TC");
        need(close(original_panel)==0,
             "WCX CloseArchive failed after editing an open ISO panel");
        need(f5_status==0,
             "TC F5 PK_PACK_SAVE_PATHS cannot copy into existing ISO directory");
        names=list_names(image,open,header,process,close);
        need(names.contains(L"DOCS\\COPIED.BIN") &&
             names.contains(L"DOCS\\HELLO.TXT"),
             "WCX ISO F5 entries missing after nested copy");
        // Verify both payloads via WCX extraction, not only the listing.
        const auto verify_data=[&](const wchar_t* archive_file,
                                   const std::string& expected) {
            auto current=image.wstring();
            OpenW options{};options.ArcName=current.data();
            const HANDLE handle=open(&options);
            need(handle && options.OpenResult==0,
                 "WCX failed to reopen ISO after F5");
            bool found=false;
            try {
                for(unsigned i=0;i<500;++i) {
                    HeaderExW entry{};
                    const int code=header(handle,&entry);
                    if(code==10)break;
                    need(code==0,"F5 ISO verification listing failed");
                    if(std::wstring_view(entry.FileName)==archive_file) {
                        const auto target=root/L"extracted";
                        fs::create_directories(target);
                        std::wstring dest=target.wstring();
                        wchar_t output_name[]=L"verified.bin";
                        need(process(handle,2,dest.data(),output_name)==0,
                             "WCX cannot extract F5 copied ISO file");
                        const auto bytes=load(target/L"verified.bin");
                        need(std::string(bytes.begin(),bytes.end())==expected,
                             "extracted ISO F5 content differs from source");
                        found=true;
                        break;
                    }
                    need(process(handle,0,nullptr,nullptr)==0,
                         "WCX skip after F5 failed");
                }
                need(found,"ISO F5 file missing from WCX readback");
            }catch(...) {close(handle);throw;}
            need(close(handle)==0,"WCX CloseArchive after F5 failed");
        };
        verify_data(L"DOCS\\COPIED.BIN","native ISO F5 copy bytes");
        verify_data(L"DOCS\\HELLO.TXT","native ISO F5 overwrite bytes");
        // A real F5 directory selection can contain ONLY the parent in
        // AddList. WCX must include all descendants, including empty
        // directories. The preflight must happen before any ISO mutation.
        const auto folder_source=root/L"Selected folders";
        write_source(folder_source/L"SELECTED"/L"INSIDE.TXT",
                     "ISO F5 selected directory data");
        write_source(folder_source/L"SELECTED"/L"DEEP"/L"MORE.BIN",
                     "ISO F5 nested directory data");
        fs::create_directories(folder_source/L"SELECTED"/L"EMPTY");
        auto folder_dir=folder_source.wstring();
        wchar_t selected_folder[]=L"SELECTED\0\0";
        need(pack(arc.data(),f5_sub.data(),folder_dir.data(),
                  selected_folder,2)==0,
             "ISO F5 selected directory did not pack recursively");
        names=list_names(image,open,header,process,close);
        need(names.contains(L"DOCS\\SELECTED") &&
             names.contains(L"DOCS\\SELECTED\\DEEP") &&
             names.contains(L"DOCS\\SELECTED\\EMPTY") &&
             names.contains(L"DOCS\\SELECTED\\INSIDE.TXT") &&
             names.contains(L"DOCS\\SELECTED\\DEEP\\MORE.BIN"),
             "ISO F5 selected directory lost child files or empty folders");
        verify_data(L"DOCS\\SELECTED\\INSIDE.TXT",
                    "ISO F5 selected directory data");
        verify_data(L"DOCS\\SELECTED\\DEEP\\MORE.BIN",
                    "ISO F5 nested directory data");
        // Some TC versions send both the parent and its children in the
        // same AddList. Deduplicate identical sources and preserve content.
        wchar_t duplicate_tree[]=
            L"SELECTED\\INSIDE.TXT\0SELECTED\0"
            L"SELECTED\\DEEP\\MORE.BIN\0\0";
        need(pack(arc.data(),f5_sub.data(),folder_dir.data(),
                  duplicate_tree,2)==0,
             "ISO F5 duplicate source entries could not be coalesced");
        names=list_names(image,open,header,process,close);
        need(names.contains(L"DOCS\\SELECTED\\DEEP\\MORE.BIN"),
             "ISO F5 duplicate selection lost nested file");
        verify_data(L"DOCS\\SELECTED\\DEEP\\MORE.BIN",
                    "ISO F5 nested directory data");
        // Optional Windows symlink privilege: when creation is permitted,
        // a reparse point inside F5 source must be refused atomically.
        std::error_code link_error;
        fs::create_directory_symlink(
            folder_source/L"SELECTED",folder_source/L"BAD-LINK",link_error);
        if(!link_error) {
            const auto unchanged=load(image);
            wchar_t unsafe_folder[]=L"BAD-LINK\0\0";
            need(pack(arc.data(),f5_sub.data(),folder_dir.data(),
                      unsafe_folder,2)!=0 &&
                 load(image)==unchanged,
                 "ISO F5 followed a reparse point or changed image on refusal");
        }
        // F8 delete of just one file must preserve the second new file.
        wchar_t f8_copied[]=L"DOCS\\COPIED.BIN\0\0";
        need(delW(arc.data(),f8_copied)==0,
             "ISO F8 cannot remove F5 copied file");
        names=list_names(image,open,header,process,close);
        need(!names.contains(L"DOCS\\COPIED.BIN") &&
             names.contains(L"DOCS\\HELLO.TXT"),
             "ISO F8 removed wrong file or left deleted file visible");

        // Cancelling an ISO F5 during the native staged clone must roll
        // back the entire private transaction, preserve the original bytes,
        // and leave no scratch folders. The original user data remains live.
        const auto before_cancel=load(image);
        wchar_t cancel_list[]=L"COPIED.BIN\0\0";
        setpack(INVALID_HANDLE_VALUE,&cancel_iso_staging);
        const int cancelled_iso=pack(arc.data(),f5_sub.data(),f5_dir.data(),
                                     cancel_list,2);
        setpack(INVALID_HANDLE_VALUE,nullptr);
        need(cancelled_iso==21 && load(image)==before_cancel,
             "cancelled ISO F5 changed source image or ignored cancellation");
        for(const auto& entry:fs::directory_iterator(root)) {
            if(entry.path().filename().wstring().starts_with(L".iso-wcx-edit-"))
                need(false,"cancelled ISO F5 left a private edit workspace");
        }
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
