// Public Total Commander WCX ABI adapter. Modern parsers live in iso_disc.
#ifdef _WIN32
#include "iso_wcx/disc.hpp"
#include "iso_wcx/archive_edit.hpp"
#include "iso_wcx/edit_transaction.hpp"
#include "iso_wcx/wcx_updater.hpp"
#include "iso_wcx/wim_parts.hpp"
#include "wcx_wim_open_progress.hpp"
#include <mutex>
#include <atomic>
#include <type_traits>
#include <cwctype>
#include <cwchar>
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <new>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {
constexpr int E_END_ARCHIVE=10,E_BAD_ARCHIVE=13,E_UNKNOWN_FORMAT=14,E_ECREATE=16,E_EABORTED=21,E_SMALL_BUF=20,E_NO_FILES=22;
constexpr int PK_SKIP=0,PK_TEST=1,PK_EXTRACT=2;
constexpr int PK_CAPS_SEARCHTEXT=128;
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
    explicit State(
        std::filesystem::path p,
        std::unique_ptr<iso_wcx::WimOpenProgress> indicator = {})
        :open_progress(std::move(indicator)),
         disc(std::move(p),
              [this](std::uint32_t image,std::size_t entries) {
                  if(open_progress)open_progress->on_scan(image,entries);
              }) {}
    // MUST be constructed before Disc: WIMLoadImage/Find callbacks run
    // inside Disc's constructor. Retain the indicator until ReadHeader
    // completes so TC's 100K+ filename publishing also gets progress.
    std::unique_ptr<iso_wcx::WimOpenProgress> open_progress;
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
    if(state->cursor>=state->disc.items().size()) {
        state->open_progress.reset();
        return E_END_ARCHIVE;
    }
    if(state->open_progress && (state->cursor%512u)==0u) {
        state->open_progress->on_listing(
            state->cursor,state->disc.items().size());
        if(state->open_progress->cancelled())return E_EABORTED;
    }
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
    auto pos=state->cursor++;
    if(operation==PK_SKIP)return 0;
    if(operation!=PK_TEST && operation!=PK_EXTRACT)return E_BAD_ARCHIVE;
    const auto& item=state->disc.items()[pos];
    try {
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
        if(!state->disc.copy(pos,output,callback,test_only))return E_EABORTED;
        return 0;
    }catch(const std::filesystem::filesystem_error&){return E_ECREATE;}
     catch(const std::exception&){return E_BAD_ARCHIVE;}
}
}

namespace {
// The fast CanYouHandleThisFile probe must not load 100K+ WIMGAPI entries.
// Total Commander can call it immediately before OpenArchive, otherwise
// opening a large install.wim needlessly scans every index twice.
// Content probing is intentionally restricted to image extensions.  Never
// attempt the heavyweight Disc parser on ZIP/RAR/7z or arbitrary files.
// PK_CAPS_BY_CONTENT is disabled in GetPackerCaps so TC itself will not
// load ISO-WCX while opening unrelated archive types.
bool probe_image_extension(const std::filesystem::path& file) {
    auto ext=file.extension().wstring();
    std::transform(ext.begin(),ext.end(),ext.begin(),
                   [](wchar_t c){return std::towlower(c);});
    for(const std::wstring_view image : {
        L".iso",L".udf",L".cue",L".bin",L".ccd",L".nrg",
        L".mds",L".mdx",L".mdf",L".cso",L".zso",L".isz",L".daa",
        L".img",L".raw",L".vhd",L".vhdx",L".wim",L".esd",L".swm",
        L".ima",L".vfd",L".flp",L".bif",L".imz",L".chd"}) {
        if(ext==image)return true;
    }
    return false;
}
bool wim_extension(const std::filesystem::path& file) {
    auto ext=file.extension().wstring();
    std::transform(ext.begin(),ext.end(),ext.begin(),
                   [](wchar_t c){return std::towlower(c);});
    return ext==L".wim" || ext==L".esd" || ext==L".swm";
}
bool quick_wim_probe(const std::filesystem::path& path) noexcept {
    try {
        const auto parts=iso_wcx::resolve_wim_parts(path);
        iso_wcx::wim_detail::Header h{};
        if(!iso_wcx::wim_detail::read_header(parts.front(),h))return false;
        iso_wcx::wim_detail::validate(h);
        const auto count=iso_wcx::wim_detail::u32(h,44);
        return count>0 && count<=256;
    }catch(...) {return false;}
}
bool wim_open_dialog_enabled(const std::filesystem::path& file) noexcept {
    try {
        wchar_t host[MAX_PATH]{};
        const DWORD n=GetModuleFileNameW(nullptr,host,MAX_PATH);
        if(!n || n>=MAX_PATH)return false;
        const wchar_t* name=std::wcsrchr(host,L'\\');
        name=name?name+1:host;
        if(_wcsicmp(name,L"TOTALCMD.EXE")!=0 &&
           _wcsicmp(name,L"TOTALCMD64.EXE")!=0)
            return false; // never pop up in a test/CLI service
        wchar_t preference[8]{};
        if(GetEnvironmentVariableW(L"ISO_WCX_WIM_OPEN_PROGRESS",
                                   preference,8)==1 &&
           preference[0]==L'0')
            return false;
        return std::filesystem::file_size(file)>=64ull*1024ull*1024ull;
    }catch(...) {return false;}
}
// F7 pack progress can stay at 0% while WIMGAPI is inside synchronous
// Apply/Capture. The self-owned TaskDialog only runs in a real TC GUI
// process, never when DLL tests, services or CLI tools call PackFiles.
bool wim_edit_dialog_enabled() noexcept {
    wchar_t host[MAX_PATH]{};
    const DWORD n=GetModuleFileNameW(nullptr,host,MAX_PATH);
    if(!n||n>=MAX_PATH)return false;
    const wchar_t* filename=std::wcsrchr(host,L'\\');
    filename=filename?filename+1:host;
    if(_wcsicmp(filename,L"TOTALCMD.EXE")!=0 &&
       _wcsicmp(filename,L"TOTALCMD64.EXE")!=0)return false;
    wchar_t setting[8]{};
    return !(GetEnvironmentVariableW(L"ISO_WCX_WIM_EDIT_PROGRESS",setting,8)==1
             && setting[0]==L'0');
}
std::unique_ptr<State> open_state(const std::filesystem::path& path) {
    if(!wim_extension(path))return std::make_unique<State>(path);
    auto indicator=std::make_unique<iso_wcx::WimOpenProgress>(
        wim_open_dialog_enabled(path));
    iso_wcx::wim_detail::Header header{};
    if(iso_wcx::wim_detail::read_header(path,header))
        indicator->set_image_count(iso_wcx::wim_detail::u32(header,44));
    return std::make_unique<State>(path,std::move(indicator));
}
} // namespace

#define WCX_EXPORT extern "C" __declspec(dllexport)
WCX_EXPORT HANDLE __stdcall OpenArchiveW(OpenW* d) {
    if(!d)return nullptr;
    d->OpenResult=E_UNKNOWN_FORMAT;
    try {
        if(!d->ArcName)return nullptr;
        const std::filesystem::path path(d->ArcName);
        if(!probe_image_extension(path))return nullptr;
        iso_wcx::schedule_update_check();
        auto state=open_state(path);
        d->OpenResult=0;
        return reinterpret_cast<HANDLE>(state.release());
    }catch(const iso_wcx::WimOpenCancelled&){
        d->OpenResult=E_EABORTED;
        return nullptr;
    }catch(...){return nullptr;}
}
WCX_EXPORT HANDLE __stdcall OpenArchive(OpenA* d) {
    if(!d)return nullptr;
    d->OpenResult=E_UNKNOWN_FORMAT;
    try {
        if(!d->ArcName)return nullptr;
        const std::filesystem::path path(from_bytes(d->ArcName,CP_ACP));
        if(!probe_image_extension(path))return nullptr;
        iso_wcx::schedule_update_check();
        auto state=open_state(path);
        d->OpenResult=0;
        return reinterpret_cast<HANDLE>(state.release());
    }catch(const iso_wcx::WimOpenCancelled&){
        d->OpenResult=E_EABORTED;
        return nullptr;
    }catch(...){return nullptr;}
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
    try {
        // The old WIM backup may still be held by the archive reader until
        // TC exits the virtual WIM panel. Release the handle FIRST, then
        // retry cleaning ONLY workspaces created by this plugin process.
        delete reinterpret_cast<State*>(handle);
        iso_wcx::retry_deferred_edit_workspace_cleanup();
        return 0;
    }catch(...) {
        return E_BAD_ARCHIVE;
    }
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
// WCX error codes cannot explain why PackFiles or DeleteFiles failed.
// Display a useful reason only inside interactive TC, never in headless CI.
void report_edit_error(const char* description,bool remove) noexcept {
    try {
        wchar_t host[MAX_PATH]{};
        const DWORD length=GetModuleFileNameW(nullptr,host,MAX_PATH);
        if(!length || length>=MAX_PATH) return;
        const wchar_t* filename=std::wcsrchr(host,L'\\');
        filename=filename ? filename+1 : host;
        if(_wcsicmp(filename,L"TOTALCMD.EXE")!=0 &&
           _wcsicmp(filename,L"TOTALCMD64.EXE")!=0)
            return;
        std::wstring reason;
        try { reason=from_bytes(description,CP_UTF8); }
        catch(...) { reason=L"Unsupported or invalid filesystem layout"; }
        if(reason.size()>800) reason.resize(800);
        std::wstring text=remove
            ? L"The selected entry was NOT deleted.\n\n"
            : L"The requested file or directory was NOT added/replaced.\n\n";
        text+=reason;
        text+=L"\n\nNo successful edit was reported. Unsupported ISO/UDF "
              L"Bridge, Rock Ridge and protected boot layouts need "
              L"a synchronized metadata writer. If a WIM mount failed, "
              L"inspect its recovery workspace before retrying.";
        MessageBoxW(nullptr,text.c_str(),
                    remove?L"ISO-WCX - Delete failed":L"ISO-WCX - Edit failed",
                    MB_OK|MB_ICONWARNING|MB_TASKMODAL);
    }catch(...) {
        OutputDebugStringW(L"ISO-WCX: failed to display archive edit error");
    }
}
int do_edit(const std::wstring& archive,const std::wstring& subpath,const std::wstring& source,
            const std::vector<std::wstring>& names,bool remove,int flags) {
    try {
        // No move packing: source files are never removed by this plugin.
        if(flags&1)return E_NOT_SUPPORTED;
        std::lock_guard lock(edit_mutex);
        std::filesystem::path image(archive);auto ext=image.extension().wstring();std::transform(ext.begin(),ext.end(),ext.begin(),[](wchar_t c){return std::towlower(c);});
        if(ext!=L".iso"&&ext!=L".udf"&&ext!=L".wim"&&
           ext!=L".ima"&&ext!=L".img"&&ext!=L".vfd"&&
           ext!=L".flp"&&ext!=L".bif"&&ext!=L".raw"&&
           ext!=L".vhd"&&ext!=L".vhdx") {
            report_edit_error("Unsupported archive extension for editing",remove);
            return E_NOT_SUPPORTED;
        }
        if(!std::filesystem::exists(image)) {
            report_edit_error("Archive file no longer exists",remove);
            return E_NOT_SUPPORTED;
        }
        std::vector<iso_wcx::ArchiveChange> changes;
        for(auto name:names){
            if(remove) {
                // TC may send "folder\\*.*", "folder\\*", or the same
                // sentinel terminated by a directory separator. Remove
                // terminal directory-selection tokens ONLY: any wildcard in
                // the actual folder/file name is still rejected by edit_path.
                while(!name.empty() &&
                      (name.back()==L'\\' || name.back()==L'/'))
                    name.pop_back();
                while(name.ends_with(L"\\*.*") || name.ends_with(L"/*.*") ||
                      name.ends_with(L"\\*") || name.ends_with(L"/*")) {
                    if(name.ends_with(L"\\*.*") || name.ends_with(L"/*.*"))
                        name.resize(name.size()-4);
                    else
                        name.resize(name.size()-2);
                    while(!name.empty() &&
                          (name.back()==L'\\' || name.back()==L'/'))
                        name.pop_back();
                }
            }
            auto relative=iso_wcx::edit_path(edit_utf8(name));
            iso_wcx::ArchiveChange c;c.path=subpath.empty()?relative:iso_wcx::edit_path(iso_wcx::edit_path(edit_utf8(subpath))+"/"+relative);c.remove=remove;
            if(!remove){c.source=std::filesystem::path(source)/std::filesystem::u8path(relative);auto status=std::filesystem::symlink_status(c.source);
                if(!std::filesystem::is_regular_file(status)&&!std::filesystem::is_directory(status))
                    throw iso_wcx::EditUnsupported("Source must be a regular file or directory");
                const DWORD attributes=GetFileAttributesW(c.source.c_str());
                if(attributes==INVALID_FILE_ATTRIBUTES||(attributes&FILE_ATTRIBUTE_REPARSE_POINT))
                    throw iso_wcx::EditUnsupported("Source is missing or contains a reparse point");
                c.directory=std::filesystem::is_directory(status);
            }changes.push_back(std::move(c));
        }
        if(remove && changes.size()>1) {
            // TC may include both a directory and every one of its children
            // in the DeleteFiles multi-SZ. Deleting the directory first and
            // then processing a child used to turn a valid F8 request into
            // an aborted transaction. Keep the shallowest selected ancestor.
            const auto folded=[](std::string p) {
                for(auto& ch:p)
                    if(ch>='A'&&ch<='Z') ch=char(ch-'A'+'a');
                return p;
            };
            std::stable_sort(changes.begin(),changes.end(),
                [](const auto& a,const auto& b) {
                    return a.path.size()<b.path.size();
                });
            std::unordered_set<std::string> selected;
            std::vector<iso_wcx::ArchiveChange> normalized;
            normalized.reserve(changes.size());
            for(auto& c:changes) {
                const auto key=folded(c.path);
                bool covered=selected.contains(key);
                for(std::size_t slash=key.find('/');!covered &&
                    slash!=std::string::npos;slash=key.find('/',slash+1))
                    if(selected.contains(key.substr(0,slash)))
                        covered=true;
                if(!covered) {
                    selected.insert(key);
                    normalized.push_back(std::move(c));
                }
            }
            changes=std::move(normalized);
        }
        if(changes.empty()) {
            report_edit_error("No entries were passed to the archive editor",remove);
            return E_NO_FILES;
        }
        auto wide=archive;auto ansi=to_ansi(archive);const auto pw=pack_progress_w.load();const auto pa=pack_progress_a.load();
        const auto progress=[&]{if(pw)return pw(wide.data(),0)!=0;if(pa)return pa(ansi.data(),0)!=0;return true;};
        if(ext==L".iso"||ext==L".udf")
             iso_wcx::edit_optical(image,changes,progress);
        else if(ext==L".wim") {
            // TC's plugin progress window can remain at 0% when creating
            // folders (F7). The delayed native UI remains responsive during
            // WIMGAPI calls and preserves real stage/cancel checkpoints.
            // Do not show another dialog for Delete (F8), whose TC
            // progress callback already works on the user's machine.
            iso_wcx::WimOpenProgress indicator(
                !remove && wim_edit_dialog_enabled(),
                iso_wcx::WimOpenProgress::Mode::Editing);
            const auto status=[&](const wchar_t* stage,unsigned percent){
                if(!indicator.on_edit(stage,percent))return false;
                // WCX callback uses negative Size for direct 1..100%.
                // Keep it for standard TC progress and cancellation too.
                const int pct=-static_cast<int>(std::clamp(percent,1u,99u));
                auto label=std::wstring(L"WIM: ")+(stage?stage:L"Working");
                if(pw)return pw(label.data(),pct)!=0;
                if(pa){auto narrow=to_ansi(label);return pa(narrow.data(),pct)!=0;}
                return true;
            };
            iso_wcx::edit_wim(image,changes,progress,status);
        }
        else iso_wcx::edit_fat(image,changes,progress);
        return 0;
    }catch(const iso_wcx::EditCancelled&){return E_EABORTED;}
     catch(const iso_wcx::EditUnsupported& e) {
         report_edit_error(e.what(),remove);
         return E_NOT_SUPPORTED;
     }catch(const std::filesystem::filesystem_error& e) {
         report_edit_error(e.what(),remove);
         return E_ECREATE;
     }catch(const std::exception& e) {
         report_edit_error(e.what(),remove);
         return E_BAD_ARCHIVE;
     }catch(...) {
         report_edit_error("unknown failure",remove);
         return E_BAD_ARCHIVE;
     }
}
}
WCX_EXPORT int __stdcall PackFilesW(wchar_t* archive,wchar_t* sub,wchar_t* source,wchar_t* list,int flags) {
    try {
        if(!archive||!source)return E_NOT_SUPPORTED;
        return do_edit(archive,sub?sub:L"",source,edit_list(list),false,flags);
    }catch(const std::exception& e) {
        report_edit_error(e.what(),false);return E_BAD_ARCHIVE;
    }catch(...) {
        report_edit_error("Unable to parse the Unicode file list",false);
        return E_BAD_ARCHIVE;
    }
}
WCX_EXPORT int __stdcall PackFiles(char* archive,char* sub,char* source,char* list,int flags) {
    try {
        if(!archive||!source)return E_NOT_SUPPORTED;
        return do_edit(from_bytes(archive,CP_ACP),from_bytes(sub,CP_ACP),
                       from_bytes(source,CP_ACP),edit_list(list),false,flags);
    }catch(const std::exception& e) {
        report_edit_error(e.what(),false);return E_BAD_ARCHIVE;
    }catch(...) {
        report_edit_error("Unable to parse the ANSI file list",false);
        return E_BAD_ARCHIVE;
    }
}
WCX_EXPORT int __stdcall DeleteFilesW(wchar_t* archive,wchar_t* list) {
    try {
        if(!archive) return E_NOT_SUPPORTED;
        return do_edit(archive,L"",L"",edit_list(list),true,0);
    }catch(const std::exception& e) {
        report_edit_error(e.what(),true);
        return E_BAD_ARCHIVE;
    }catch(...) {
        report_edit_error("Could not parse Unicode delete list",true);
        return E_BAD_ARCHIVE;
    }
}
WCX_EXPORT int __stdcall DeleteFiles(char* archive,char* list) {
    try {
        if(!archive) return E_NOT_SUPPORTED;
        return do_edit(from_bytes(archive,CP_ACP),L"",L"",edit_list(list),true,0);
    }catch(const std::exception& e) {
        report_edit_error(e.what(),true);
        return E_BAD_ARCHIVE;
    }catch(...) {
        report_edit_error("Could not parse ANSI delete list",true);
        return E_BAD_ARCHIVE;
    }
}
WCX_EXPORT void __stdcall ConfigurePacker(HWND parent,HINSTANCE module) {
    try { iso_wcx::configure_updates(parent,module); } catch(...) {}
}
WCX_EXPORT int __stdcall GetPackerCaps() {
    // TC asks for flags at startup, including when the user only opens ZIPs.
    // Do not start the update checker until an actual image is opened/probed.
    return PK_CAPS_SEARCHTEXT|2|4|8|16; // 158, no global BY_CONTENT ZIP interception
}
WCX_EXPORT int __stdcall CanYouHandleThisFileW(wchar_t* path) {
    if(!path)return 0;
    try {
        const std::filesystem::path source(path);
        if(!probe_image_extension(source))return 0;
        iso_wcx::schedule_update_check();
        if(wim_extension(source))return quick_wim_probe(source)?1:0;
        iso_wcx::Disc test(source);
        return 1;
    }catch(...){return 0;}
}
WCX_EXPORT int __stdcall CanYouHandleThisFile(char* path) {
    if(!path)return 0;
    try {
        const std::filesystem::path source(from_bytes(path,CP_ACP));
        if(!probe_image_extension(source))return 0;
        iso_wcx::schedule_update_check();
        if(wim_extension(source))return quick_wim_probe(source)?1:0;
        iso_wcx::Disc test(source);
        return 1;
    }catch(...){return 0;}
}
#else
#error "WCX adapter must be compiled for Windows"
#endif

