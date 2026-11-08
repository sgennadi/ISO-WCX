// Public Total Commander WCX ABI adapter. Modern parsers live in iso_disc.
#ifdef _WIN32
#include "iso_wcx/disc.hpp"
#include "iso_wcx/archive_edit.hpp"
#include "iso_wcx/wcx_updater.hpp"
#include "iso_wcx/tc_registration_repair.hpp"
#include "iso_wcx/operation_diagnostics.hpp"
#include "iso_wcx/tc_association_flags.hpp"
#include <mutex>
#include <atomic>
#include <type_traits>
#include <cwctype>
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace {
constexpr int E_END_ARCHIVE=10,E_BAD_ARCHIVE=13,E_UNKNOWN_FORMAT=14,E_ECREATE=16,E_EABORTED=21,E_SMALL_BUF=20;
constexpr int PK_SKIP=0,PK_TEST=1,PK_EXTRACT=2;
constexpr int PK_CAPS_BY_CONTENT=64,PK_CAPS_SEARCHTEXT=128;
constexpr int E_NOT_SUPPORTED=24;
constexpr int ATTR_DIR=0x10,ATTR_ARCHIVE=0x20;

struct OpenA {char* ArcName;int OpenMode,OpenResult;char* CmtBuf;int CmtBufSize,CmtSize,CmtState;};
struct OpenW {wchar_t* ArcName;int OpenMode,OpenResult;wchar_t* CmtBuf;int CmtBufSize,CmtSize,CmtState;};
struct HeaderA {char ArcName[260],FileName[260];int Flags,PackSize,UnpSize,HostOS,FileCRC,FileTime,UnpVer,Method,FileAttr;char* CmtBuf;int CmtBufSize,CmtSize,CmtState;};
struct HeaderExA {char ArcName[1024],FileName[1024];int Flags;unsigned int PackSize,PackSizeHigh,UnpSize,UnpSizeHigh;int HostOS,FileCRC,FileTime,UnpVer,Method,FileAttr;char* CmtBuf;int CmtBufSize,CmtSize,CmtState;char Reserved[1024];};
struct HeaderExW {wchar_t ArcName[1024],FileName[1024];int Flags;unsigned int PackSize,PackSizeHigh,UnpSize,UnpSizeHigh;int HostOS,FileCRC,FileTime,UnpVer,Method,FileAttr;char* CmtBuf;int CmtBufSize,CmtSize,CmtState;char Reserved[1024];};
using ProgressA=int (__stdcall*)(char*,int);
using ProgressW=int (__stdcall*)(wchar_t*,int);
// Total Commander may register the packing callback on a different thread
// from the one invoking PackFiles. Snapshot callbacks for each operation.
std::atomic<ProgressA> pack_progress_a{nullptr};
std::atomic<ProgressW> pack_progress_w{nullptr};
std::mutex edit_mutex;
using ChangeA=int (__stdcall*)(char*,int);
using ChangeW=int (__stdcall*)(wchar_t*,int);
struct State {
    explicit State(std::filesystem::path p):archive_path(p),disc(std::move(p)){}
    std::filesystem::path archive_path;
    iso_wcx::Disc disc;
    std::size_t cursor=0;
    ProgressA progress_a=nullptr;
    ProgressW progress_w=nullptr;
    ChangeA change_a=nullptr;
    ChangeW change_w=nullptr;
};

std::wstring from_bytes(const char* s,UINT encoding) {
    if(!s)return {};
    int len=MultiByteToWideChar(encoding,MB_ERR_INVALID_CHARS,s,-1,nullptr,0);
    if(len<=0)throw std::runtime_error("invalid path encoding");
    std::wstring out(static_cast<std::size_t>(len),L'\0');
    if(!MultiByteToWideChar(encoding,MB_ERR_INVALID_CHARS,s,-1,out.data(),len))throw std::runtime_error("path conversion error");
    out.pop_back();return out;
}
std::string to_ansi(const std::wstring& s) {
    int len=WideCharToMultiByte(CP_ACP,0,s.c_str(),-1,nullptr,0,nullptr,nullptr);
    if(len<=0)throw std::runtime_error("path conversion error");
    std::string out(static_cast<std::size_t>(len),'\0');
    if(!WideCharToMultiByte(CP_ACP,0,s.c_str(),-1,out.data(),len,nullptr,nullptr))throw std::runtime_error("path conversion error");
    out.pop_back();return out;
}
std::wstring item_path(const iso_wcx::Item& item) {
    auto wide=from_bytes(item.path.c_str(),CP_UTF8);
    std::replace(wide.begin(),wide.end(),L'/',L'\\');
    return wide;
}
template<typename C, std::size_t N> bool copy_fixed(C (&out)[N],const std::basic_string<C>& in) {
    if(in.size()>=N)return false;
    std::fill_n(out,N,C{});
    std::copy(in.begin(),in.end(),out);
    return true;
}
void fill_common(HeaderExW& h,const iso_wcx::Item& item) {
    h.PackSize=static_cast<unsigned>(item.size);
    h.UnpSize=static_cast<unsigned>(item.size);
    h.PackSizeHigh=static_cast<unsigned>(item.size>>32);
    h.UnpSizeHigh=static_cast<unsigned>(item.size>>32);
    h.FileAttr=static_cast<int>(item.attributes?item.attributes:(item.directory?ATTR_DIR:ATTR_ARCHIVE));
    h.FileTime=static_cast<int>(item.dos_time);
}
int header_w(State* state,HeaderExW* result) {
    if(!state||!result)return E_BAD_ARCHIVE;
    if(state->cursor>=state->disc.items().size())return E_END_ARCHIVE;
    *result={};
    const auto& item=state->disc.items()[state->cursor];
    auto wname=item_path(item);
    if(!copy_fixed(result->FileName,wname))return E_SMALL_BUF;
    fill_common(*result,item);
    return 0;
}
int header_a(State* state,HeaderExA* result) {
    if(!result)return E_BAD_ARCHIVE;
    HeaderExW wide{};
    int rc=header_w(state,&wide);
    if(rc)return rc;
    *result={};
    auto name=to_ansi(wide.FileName);
    if(!copy_fixed(result->FileName,name))return E_SMALL_BUF;
    result->PackSize=wide.PackSize;result->PackSizeHigh=wide.PackSizeHigh;
    result->UnpSize=wide.UnpSize;result->UnpSizeHigh=wide.UnpSizeHigh;
    result->FileAttr=wide.FileAttr;result->FileTime=wide.FileTime;
    return 0;
}
std::filesystem::path destination(const iso_wcx::Item& item,const wchar_t* dest_path,const wchar_t* dest_name) {
    if(dest_name && *dest_name) {
        std::filesystem::path name(dest_name);
        if(name.is_absolute() || !dest_path || !*dest_path)return name;
        return std::filesystem::path(dest_path)/name;
    }
    std::filesystem::path root=dest_path&&*dest_path?std::filesystem::path(dest_path):std::filesystem::current_path();
    return root/std::filesystem::path(item_path(item));
}
int do_process(State* state,int operation,const wchar_t* dest_path,const wchar_t* dest_name) {
    if(!state || state->cursor>=state->disc.items().size())return E_END_ARCHIVE;
    // TC issues PK_SKIP for every enumerated directory entry. Logging each
    // skip would slow down 100k-file install.wim listings and flood the log.
    // CloseArchive records one summary of the enumerated item count instead.
    const auto pos=state->cursor++;
    if(operation==PK_SKIP)return 0;
    const char* action=operation==PK_TEST?"test":
                       operation==PK_EXTRACT?"extract":"other";
    iso_wcx::diagnostics::Operation diagnostic(action,state->archive_path,1);
    const auto finish=[&](int code,const char* reason=""){
        diagnostic.complete(code,reason);
        return code;
    };
    if(operation!=PK_TEST && operation!=PK_EXTRACT)return finish(E_BAD_ARCHIVE,"invalid operation");
    const auto& item=state->disc.items()[pos];
    try {
        diagnostic.stage(L"copy",0);
        std::wstring wide=item_path(item);
        // Total Commander can register both callbacks. Report to Unicode only
        // when available; otherwise every byte is counted twice in its UI.
        auto ansi=(!state->progress_w && state->progress_a) ? to_ansi(wide) : std::string{};
        std::uint64_t prior=0;
        auto callback=[&](std::uint64_t done) {
            auto delta=static_cast<int>(done-prior);
            prior=done;
            if(state->progress_w)return state->progress_w(wide.data(),delta)!=0;
            if(state->progress_a)return state->progress_a(ansi.data(),delta)!=0;
            return true;
        };
        bool test_only=operation==PK_TEST;
        auto output=test_only?std::filesystem::path{}:destination(item,dest_path,dest_name);
        if(!state->disc.copy(pos,output,callback,test_only))return finish(E_EABORTED,"cancelled");
        return finish(0);
    }catch(const std::filesystem::filesystem_error& e){return finish(E_ECREATE,e.what());}
     catch(const std::exception& e){return finish(E_BAD_ARCHIVE,e.what());}
     catch(...){return finish(E_BAD_ARCHIVE,"unknown exception");}
}
}

#define WCX_EXPORT extern "C" __declspec(dllexport)
WCX_EXPORT HANDLE __stdcall OpenArchiveW(OpenW* d) {
    iso_wcx::schedule_update_check();
    if(!d)return nullptr;
    d->OpenResult=E_UNKNOWN_FORMAT;
    if(!d->ArcName)return nullptr;
    try {
        const auto image=std::filesystem::path(d->ArcName);
        iso_wcx::diagnostics::Operation diagnostic("open",image);
        try {
            auto state=std::make_unique<State>(image);
            d->OpenResult=0;
            diagnostic.complete(0);
            return reinterpret_cast<HANDLE>(state.release());
        }catch(const std::exception& e){
            diagnostic.complete(E_UNKNOWN_FORMAT,e.what());return nullptr;
        }catch(...){diagnostic.complete(E_UNKNOWN_FORMAT,"unknown exception");return nullptr;}
    }catch(...){return nullptr;}
}
WCX_EXPORT HANDLE __stdcall OpenArchive(OpenA* d) {
    iso_wcx::schedule_update_check();
    if(!d)return nullptr;
    d->OpenResult=E_UNKNOWN_FORMAT;
    if(!d->ArcName)return nullptr;
    try {
        const auto image=std::filesystem::path(from_bytes(d->ArcName,CP_ACP));
        iso_wcx::diagnostics::Operation diagnostic("open",image);
        try {
            auto state=std::make_unique<State>(image);
            d->OpenResult=0;
            diagnostic.complete(0);
            return reinterpret_cast<HANDLE>(state.release());
        }catch(const std::exception& e) {
            diagnostic.complete(E_UNKNOWN_FORMAT,e.what());return nullptr;
        }catch(...){diagnostic.complete(E_UNKNOWN_FORMAT,"unknown exception");return nullptr;}
    }catch(...){
        iso_wcx::diagnostics::Operation diagnostic("open",{});
        diagnostic.complete(E_UNKNOWN_FORMAT,"path conversion");
        return nullptr;
    }
}
WCX_EXPORT int __stdcall ReadHeaderExW(HANDLE handle,HeaderExW* result) {
    try{return header_w(reinterpret_cast<State*>(handle),result);}catch(...){return E_BAD_ARCHIVE;}
}
WCX_EXPORT int __stdcall ReadHeaderEx(HANDLE handle,HeaderExA* result) {
    try{return header_a(reinterpret_cast<State*>(handle),result);}catch(...){return E_BAD_ARCHIVE;}
}
WCX_EXPORT int __stdcall ReadHeader(HANDLE handle,HeaderA* result) {
    if(!result)return E_BAD_ARCHIVE;
    try {
        HeaderExA ex{};auto rc=header_a(reinterpret_cast<State*>(handle),&ex);if(rc)return rc;
        *result={};
        if(std::strlen(ex.FileName)>=sizeof(result->FileName))return E_SMALL_BUF;
        std::memcpy(result->FileName,ex.FileName,std::strlen(ex.FileName)+1);
        result->PackSize=static_cast<int>(ex.PackSize);result->UnpSize=static_cast<int>(ex.UnpSize);
        result->FileAttr=ex.FileAttr;result->FileTime=ex.FileTime;return 0;
    }catch(...){return E_BAD_ARCHIVE;}
}
WCX_EXPORT int __stdcall ProcessFileW(HANDLE handle,int operation,wchar_t* path,wchar_t* name) {
    return do_process(reinterpret_cast<State*>(handle),operation,path,name);
}
WCX_EXPORT int __stdcall ProcessFile(HANDLE handle,int operation,char* path,char* name) {
    try {
        std::wstring wpath=path?from_bytes(path,CP_ACP):L"";
        std::wstring wname=name?from_bytes(name,CP_ACP):L"";
        return do_process(reinterpret_cast<State*>(handle),operation,
                          path?wpath.c_str():nullptr,name?wname.c_str():nullptr);
    }catch(...){return E_BAD_ARCHIVE;}
}
WCX_EXPORT int __stdcall CloseArchive(HANDLE handle) {
    auto* state=reinterpret_cast<State*>(handle);
    if(state){
        iso_wcx::diagnostics::Operation diagnostic(
            "close",state->archive_path,
            static_cast<unsigned>(std::min<std::size_t>(state->cursor,100000)));
        delete state;
        diagnostic.complete(0);
    }
    return 0;
}
WCX_EXPORT void __stdcall SetProcessDataProcW(HANDLE handle,ProgressW proc) {
    if(handle==INVALID_HANDLE_VALUE||!handle){pack_progress_w.store(proc);return;}
    if(auto* state=reinterpret_cast<State*>(handle))state->progress_w=proc;
}
WCX_EXPORT void __stdcall SetProcessDataProc(HANDLE handle,ProgressA proc) {
    if(handle==INVALID_HANDLE_VALUE||!handle){pack_progress_a.store(proc);return;}
    if(auto* state=reinterpret_cast<State*>(handle))state->progress_a=proc;
}
WCX_EXPORT void __stdcall SetChangeVolProcW(HANDLE handle,ChangeW proc) {
    if(handle==INVALID_HANDLE_VALUE||!handle)return;
    if(auto* state=reinterpret_cast<State*>(handle))state->change_w=proc;
}
WCX_EXPORT void __stdcall SetChangeVolProc(HANDLE handle,ChangeA proc) {
    if(handle==INVALID_HANDLE_VALUE||!handle)return;
    if(auto* state=reinterpret_cast<State*>(handle))state->change_a=proc;
}
namespace {
std::string edit_utf8(const std::wstring& text) {
    if(text.empty())return {};
    int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    if(n<=0)throw iso_wcx::EditUnsupported("invalid Unicode edit path");
    std::string out(static_cast<std::size_t>(n),'\0');
    if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),out.data(),n,nullptr,nullptr))throw iso_wcx::EditUnsupported("invalid Unicode edit path");
    return out;
}
template<class Char> std::vector<std::wstring> edit_list(const Char* list) {
    if(!list)throw iso_wcx::EditUnsupported("missing edit list");
    std::vector<std::wstring> out;std::size_t consumed=0;
    while(*list){std::size_t n=0;while(list[n]){if(++n>2048)throw iso_wcx::EditUnsupported("edit path too long");}
        if(out.size()>=100000 || (consumed+=n+1)>16*1024*1024)throw iso_wcx::EditUnsupported("edit list too large");
        if constexpr(std::is_same_v<Char,wchar_t>)out.emplace_back(list,n);else out.push_back(from_bytes(list,CP_ACP));list+=n+1;
    }return out;
}
// Preserve the underlying ISO edit refusal reason for troubleshooting.
// WCX can only return an integer error code, so TC cannot display what
// the native writer rejected. Never log source file contents or credentials.
void log_edit_failure(const std::wstring& archive,bool remove,const char* reason,int code) noexcept {
    try {
        wchar_t local[32768]{};
        const DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",local,32768);
        if(n==0 || n>=32768)return;
        const auto directory=std::filesystem::path(local)/L"ISO-WCX"/L"Logs";
        std::filesystem::create_directories(directory);
        std::ofstream log(directory/L"edit-errors.log",std::ios::app);
        if(!log)return;
        SYSTEMTIME stamp{};
        GetLocalTime(&stamp);
        const auto suffix=std::filesystem::path(archive).extension().wstring();
        const char* kind=(suffix==L".iso" || suffix==L".ISO" || suffix==L".udf" || suffix==L".UDF")
                             ?"ISO":"IMAGE";
        log << '[' << stamp.wYear << '-' << stamp.wMonth << '-' << stamp.wDay
            << ' ' << stamp.wHour << ':' << stamp.wMinute << ':' << stamp.wSecond
            << "] WCX_EDIT_FAILURE format=" << kind
            << " action=" << (remove?"delete":"pack")
            << " code=" << code
            << " reason=" << (reason?reason:"unknown") << '\n';
    } catch(...) {
        // Diagnostics must never change the WCX operation result.
    }
}
int do_edit(const std::wstring& archive,const std::wstring& subpath,const std::wstring& source,
            const std::vector<std::wstring>& names,bool remove,int flags) {
    unsigned image_index=0;
    if(subpath.size()>5 && subpath.compare(0,5,L"image")==0){
        for(std::size_t at=5;at<subpath.size()&&at<8;++at){
            if(subpath[at]<L'0'||subpath[at]>L'9')break;
            image_index=image_index*10+unsigned(subpath[at]-L'0');
        }
    }
    iso_wcx::diagnostics::Operation diagnostic(
        remove?"delete":"pack",std::filesystem::path(archive),
        static_cast<unsigned>(std::min<std::size_t>(names.size(),100000)),image_index);
    const auto finish=[&](int code,const char* reason="") {
        diagnostic.complete(code,reason);
        return code;
    };
    try {
        // No move packing: source files are never removed by this plugin.
        if(flags&1)return finish(E_NOT_SUPPORTED,"move packing unsupported");
        std::lock_guard lock(edit_mutex);
        std::filesystem::path image(archive);auto ext=image.extension().wstring();std::transform(ext.begin(),ext.end(),ext.begin(),[](wchar_t c){return std::towlower(c);});
        if(ext!=L".iso"&&ext!=L".udf"&&ext!=L".wim"&&
           ext!=L".ima"&&ext!=L".img"&&ext!=L".vfd"&&
           ext!=L".flp"&&ext!=L".bif"&&ext!=L".raw"&&
           ext!=L".vhd"&&ext!=L".vhdx")return finish(E_NOT_SUPPORTED,"unsupported format");
        if(!std::filesystem::exists(image))return finish(E_NOT_SUPPORTED,"archive missing");
        std::vector<iso_wcx::ArchiveChange> changes;
        for(auto name:names){
            if(remove) {
                // TC WCX API uses Folder\\*.* for recursive directory
                // deletion, not only Folder\\*. Strip this transport suffix
                // before validating the canonical archive entry. Never
                // interpret arbitrary wildcard patterns as delete requests.
                if(name.ends_with(L"\\*.*") || name.ends_with(L"/*.*"))
                    name.resize(name.size()-4);
                else if(name.ends_with(L"\\*") || name.ends_with(L"/*"))
                    name.resize(name.size()-2);
            }
            auto relative=iso_wcx::edit_path(edit_utf8(name));
            iso_wcx::ArchiveChange c;c.path=subpath.empty()?relative:iso_wcx::edit_path(iso_wcx::edit_path(edit_utf8(subpath))+"/"+relative);c.remove=remove;
            if(!remove){c.source=std::filesystem::path(source)/std::filesystem::u8path(relative);auto status=std::filesystem::symlink_status(c.source);
                if(!std::filesystem::is_regular_file(status)&&!std::filesystem::is_directory(status))return finish(E_NOT_SUPPORTED,"source missing or unsupported");
                const DWORD attributes=GetFileAttributesW(c.source.c_str());if(attributes==INVALID_FILE_ATTRIBUTES||(attributes&FILE_ATTRIBUTE_REPARSE_POINT))return finish(E_NOT_SUPPORTED,"reparse point or missing source");
                c.directory=std::filesystem::is_directory(status);
            }changes.push_back(std::move(c));
        }
        if(changes.empty())return finish(0);
        if(!remove && changes.size()==1 && changes.front().directory)
            diagnostic.set_action("mkdir");
        auto wide=archive;auto ansi=to_ansi(archive);const auto pw=pack_progress_w.load();const auto pa=pack_progress_a.load();
        const auto progress=[&]{if(pw)return pw(wide.data(),0)!=0;if(pa)return pa(ansi.data(),0)!=0;return true;};
        diagnostic.stage(L"write",0);
        if(ext==L".iso"||ext==L".udf")
             iso_wcx::edit_optical(image,changes,progress);
        else if(ext==L".wim") {
            const auto status=[&](const wchar_t* stage,unsigned percent){
                // The WCX callback uses negative Size for direct 1..100%.
                // Display the active WIM stage instead of the same archive
                // filename for five minutes. Unicode first, never both.
                const int pct=-static_cast<int>(std::clamp(percent,1u,99u));
                diagnostic.stage(stage,percent);
                auto label=std::wstring(L"WIM: ")+stage;
                if(pw)return pw(label.data(),pct)!=0;
                if(pa){auto narrow=to_ansi(label);return pa(narrow.data(),pct)!=0;}
                return true;
            };
            iso_wcx::edit_wim(image,changes,progress,status);
        }
        else iso_wcx::edit_fat(image,changes,progress);
        return finish(0);
    }catch(const iso_wcx::EditCancelled&){return finish(E_EABORTED,"cancelled");}
     catch(const iso_wcx::EditUnsupported& e){log_edit_failure(archive,remove,e.what(),E_NOT_SUPPORTED);return finish(E_NOT_SUPPORTED,e.what());}
     catch(const std::filesystem::filesystem_error& e){log_edit_failure(archive,remove,e.what(),E_ECREATE);return finish(E_ECREATE,e.what());}
     catch(const std::exception& e){log_edit_failure(archive,remove,e.what(),E_BAD_ARCHIVE);return finish(E_BAD_ARCHIVE,e.what());}
     catch(...){log_edit_failure(archive,remove,"unknown native exception",E_BAD_ARCHIVE);return finish(E_BAD_ARCHIVE,"unknown exception");}
}
}
WCX_EXPORT int __stdcall PackFilesW(wchar_t* archive,wchar_t* sub,wchar_t* source,wchar_t* list,int flags) {
    try {
        if(!archive||!source) {
            // If the TC host enters WCX with incomplete arguments, do not
            // silently return 24: this must appear in the local operation log.
            iso_wcx::diagnostics::Operation event(
                "pack",archive?std::filesystem::path(archive):std::filesystem::path{});
            event.complete(E_NOT_SUPPORTED,"missing archive or source");
            return E_NOT_SUPPORTED;
        }
        return do_edit(archive,sub?sub:L"",source,edit_list(list),false,flags);
    }catch(...) {
        // Do not attempt to dereference an untrusted WCX argument while
        // already unwinding from a parsing/encoding exception.
        iso_wcx::diagnostics::Operation event("pack",{});
        event.complete(E_BAD_ARCHIVE,"unparseable WCX pack arguments");
        return E_BAD_ARCHIVE;
    }
}
WCX_EXPORT int __stdcall PackFiles(char* archive,char* sub,char* source,char* list,int flags) {
    try {
        if(!archive||!source) {
            iso_wcx::diagnostics::Operation event("pack",{});
            event.complete(E_NOT_SUPPORTED,"missing ANSI archive or source");
            return E_NOT_SUPPORTED;
        }
        return do_edit(from_bytes(archive,CP_ACP),from_bytes(sub,CP_ACP),
                       from_bytes(source,CP_ACP),edit_list(list),false,flags);
    }catch(...) {
        iso_wcx::diagnostics::Operation event("pack",{});
        event.complete(E_BAD_ARCHIVE,"unparseable ANSI pack arguments");
        return E_BAD_ARCHIVE;
    }
}
WCX_EXPORT int __stdcall DeleteFilesW(wchar_t* archive,wchar_t* list) {
    try{if(!archive)return E_NOT_SUPPORTED;return do_edit(archive,L"",L"",edit_list(list),true,0);}catch(...){return E_BAD_ARCHIVE;}
}
WCX_EXPORT int __stdcall DeleteFiles(char* archive,char* list) {
    try{if(!archive)return E_NOT_SUPPORTED;return do_edit(from_bytes(archive,CP_ACP),L"",L"",edit_list(list),true,0);}catch(...){return E_BAD_ARCHIVE;}
}
WCX_EXPORT void __stdcall ConfigurePacker(HWND parent,HINSTANCE module) {
    try { iso_wcx::configure_updates(parent,module); } catch(...) {}
}
// The Windows rundll32 worker is deliberately exported separately from WCX ABI.
// It performs the approved fix only after all Total Commander windows close.
WCX_EXPORT void __stdcall IsoWcxAssociationFixWorkerW(HWND, HINSTANCE module,
                                                      wchar_t* arguments, int) {
    iso_wcx::run_association_fix_worker(module, arguments?arguments:L"");
}
WCX_EXPORT int __stdcall GetPackerCaps() {
    iso_wcx::schedule_update_check();
    return static_cast<int>(iso_wcx::tc_association::reported_caps); // 159; real TC host F5 verified
}
WCX_EXPORT int __stdcall CanYouHandleThisFileW(wchar_t* path) {
    iso_wcx::schedule_update_check();
    if(!path)return 0;
    try{iso_wcx::Disc test{std::filesystem::path(path)};return 1;}catch(...){return 0;}
}
WCX_EXPORT int __stdcall CanYouHandleThisFile(char* path) {
    iso_wcx::schedule_update_check();
    if(!path)return 0;
    try{iso_wcx::Disc test{std::filesystem::path(from_bytes(path,CP_ACP))};return 1;}catch(...){return 0;}
}
#else
#error "WCX adapter must be compiled for Windows"
#endif

