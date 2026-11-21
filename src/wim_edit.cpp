#ifdef _WIN32
#include "iso_wcx/archive_edit.hpp"
#include "iso_wcx/edit_transaction.hpp"
#include "iso_wcx/cleanup_failure.hpp"
#include "iso_wcx/wim_archive.hpp"
#include "iso_wcx/wim_parts.hpp"
#include "iso_wcx/wim_scratch_diagnostics.hpp"
#include "iso_wcx/wim_scratch_delete.hpp"
#include "iso_wcx/wim_scratch_acl.hpp"
#include "iso_wcx/wim_edit_preflight.hpp"
#include "iso_wcx/wim_staging_budget.hpp"
#include <msxml6.h>
#include <aclapi.h>
#include <vector>
#include <map>
#include <mutex>
#include <set>
#include <atomic>
#include <cstring>
#include <algorithm>
#include <array>
#include <fstream>
#include <string>
#include <utility>
#include <cstdint>
#include <exception>

namespace iso_wcx { namespace {
void checked(bool ok,const char* what){if(!ok)throw std::runtime_error(std::string(what)+" (Win32 "+std::to_string(GetLastError())+")");}
template<class T> T api(HMODULE m,const char* name){auto p=GetProcAddress(m,name);checked(p!=nullptr,name);return reinterpret_cast<T>(p);}
struct Library {HMODULE h=LoadLibraryExW(L"wimgapi.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);Library(){checked(h!=nullptr,"WIMGAPI unavailable");}~Library(){FreeLibrary(h);}};
using Close=BOOL(WINAPI*)(HANDLE);
struct Handle {HANDLE h=nullptr;Close close;Handle(HANDLE v,Close c):h(v),close(c){checked(h&&h!=INVALID_HANDLE_VALUE,"WIMGAPI open/load failed");}~Handle(){if(h)close(h);}Handle(const Handle&)=delete;};
// Give this operation its own impersonation token, restoring any caller token.
struct Privileges {
    HANDLE prior=nullptr,token=nullptr;
    Privileges(){
        if(!OpenThreadToken(GetCurrentThread(),TOKEN_QUERY|TOKEN_DUPLICATE|TOKEN_IMPERSONATE,TRUE,&prior))
            checked(GetLastError()==ERROR_NO_TOKEN,"cannot read thread token");
        // Always borrow privileges from the elevated host process. WIMGAPI
        // may temporarily impersonate a local service/account on this
        // thread; that identity can lack backup/restore rights or differ
        // from the domain user who started Total Commander.
        HANDLE source=nullptr;
        if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY|TOKEN_DUPLICATE,&source)){
            if(prior)CloseHandle(prior);
            checked(false,"cannot read WIM host process token");
        }
        const BOOL copied=DuplicateTokenEx(source,
            TOKEN_QUERY|TOKEN_ADJUST_PRIVILEGES|TOKEN_IMPERSONATE,
            nullptr,SecurityImpersonation,TokenImpersonation,&token);
        CloseHandle(source);
        if(!copied){if(prior)CloseHandle(prior);checked(false,"cannot create imaging token");}
        try{for(const wchar_t* name:{L"SeBackupPrivilege",L"SeRestorePrivilege",L"SeSecurityPrivilege"}){
            TOKEN_PRIVILEGES p{};p.PrivilegeCount=1;checked(LookupPrivilegeValueW(nullptr,name,&p.Privileges[0].Luid),"privilege lookup failed");p.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;
            SetLastError(ERROR_SUCCESS);checked(AdjustTokenPrivileges(token,FALSE,&p,0,nullptr,nullptr)&&GetLastError()==ERROR_SUCCESS,"WIM editing requires administrator backup/restore/security privileges");}
            checked(SetThreadToken(nullptr,token),"cannot activate imaging token");
        }catch(...){CloseHandle(token);if(prior)CloseHandle(prior);throw;}
    }
    ~Privileges(){SetThreadToken(nullptr,prior);if(prior)CloseHandle(prior);CloseHandle(token);}
};
// WIMGAPI reports percent during its long synchronous Apply/Capture calls.
// Forward at most five UI notifications per second and preserve cancellation.
// Status is serialized: some WIMGAPI notifications arrive on worker threads.
struct Callback {
    const EditProgress& progress;
    const WimEditStatus& status;
    std::mutex mutex;
    std::atomic<bool> cancelled{false};
    std::wstring stage=L"Processing WIM";
    unsigned phase_start=5, phase_span=0, last_percent=5;
    // WIM_MSG_SETRANGE/SETPOS/STEPIT convey file counts during capture.
    std::uint64_t file_range=0,file_position=0;
    ULONGLONG last_signal=0;

    void notify_locked(unsigned percent,bool force=false) {
        if(cancelled.load())return;
        percent=std::clamp(percent,1u,99u);
        percent=std::max(percent,last_percent);
        const auto now=GetTickCount64();
        // Do not repaint Total Commander for every file notification.
        if(!force && now-last_signal<200u && percent==last_percent)return;
        if(!force && now-last_signal<200u)return;
        last_signal=now;
        last_percent=percent;
        if(status) {
            if(!status(stage.c_str(),percent))cancelled.store(true);
        }else if(progress && !progress()) {
            cancelled.store(true);
        }
    }
    void begin_stage(std::wstring label,unsigned start,unsigned span) {
        std::lock_guard lock(mutex);
        stage=std::move(label);
        phase_start=std::max(last_percent,std::min(start,99u));
        phase_span=std::min(span,99u-phase_start);
        file_range=0;
        file_position=0;
        notify_locked(phase_start,true);
        if(cancelled.load())throw EditCancelled();
    }
    static DWORD CALLBACK call(DWORD message,WPARAM wParam,LPARAM lParam,void* context) noexcept {
        auto& self=*static_cast<Callback*>(context);
        constexpr DWORD base=0x8000u+0x1476u;
        // WIM_MSG_PROGRESS, PROCESS, SETRANGE, SETPOS and STEPIT.
        if(message!=base+2u && message!=base+3u &&
           message!=base+5u && message!=base+6u && message!=base+7u)
            return 0;
        try {
            std::lock_guard lock(self.mutex);
            if(message==base+2u) {
                // Applied-image progress is a direct percentage.
                const auto percent=static_cast<unsigned>(
                    std::min<WPARAM>(wParam,100u));
                self.notify_locked(self.phase_start+
                    (self.phase_span*percent)/100u);
            }else if(message==base+5u) {
                // WIM_MSG_SETRANGE: lParam is the number of files.
                self.file_range=static_cast<std::uint32_t>(lParam);
                self.file_position=0;
            }else if(message==base+6u || message==base+7u) {
                // Capture uses SETPOS + STEPIT rather than PROGRESS.
                if(message==base+6u)
                    self.file_position=static_cast<std::uint32_t>(lParam);
                else if(self.file_position<self.file_range)
                    ++self.file_position;
                if(self.file_range) {
                    const auto done=std::min(self.file_range,self.file_position);
                    self.notify_locked(self.phase_start+
                        static_cast<unsigned>(
                            self.phase_span*done/self.file_range));
                }
            }else{
                // PROCESS is WIMGAPI's documented abort point.
                self.notify_locked(self.last_percent);
            }
        }catch(...){self.cancelled.store(true);}
        if(message==base+3u && self.cancelled.load())return 0xffffffffu;
        return 0;
    }
    void tick() {
        if(cancelled.load())throw EditCancelled();
        edit_tick(progress);
    }
};
using Register=DWORD(WINAPI*)(HANDLE,FARPROC,void*);
using Unregister=BOOL(WINAPI*)(HANDLE,FARPROC);
// Copy-on-edit keeps all untouched WIM indexes byte-for-byte in the
// staged archive. Progress and cancellation remain visible in Total Commander.
struct CopyContext {
    Callback& callback;
    unsigned start, span;
    static DWORD CALLBACK on_progress(LARGE_INTEGER total,LARGE_INTEGER transferred,
                                      LARGE_INTEGER,LARGE_INTEGER,DWORD,DWORD,
                                      HANDLE,HANDLE,LPVOID context) noexcept {
        auto& self=*static_cast<CopyContext*>(context);
        try {
            unsigned progress=0;
            if(total.QuadPart>0) {
                const auto done=std::clamp<LONGLONG>(transferred.QuadPart,0,total.QuadPart);
                progress=static_cast<unsigned>(
                    static_cast<long double>(done)*self.span/total.QuadPart);
            }
            std::lock_guard lock(self.callback.mutex);
            self.callback.notify_locked(self.start+progress);
        } catch(...) {
            self.callback.cancelled.store(true);
        }
        return self.callback.cancelled.load()?PROGRESS_CANCEL:PROGRESS_CONTINUE;
    }
};
struct Registered {HANDLE h;Unregister unregister;Registered(HMODULE m,HANDLE file,Callback& c):h(file),unregister(api<Unregister>(m,"WIMUnregisterMessageCallback")){
    checked(api<Register>(m,"WIMRegisterMessageCallback")(h,reinterpret_cast<FARPROC>(&Callback::call),&c)!=0xffffffffu,"register WIM progress");}
    ~Registered(){unregister(h,reinterpret_cast<FARPROC>(&Callback::call));}
};
template<class T>struct Com {T* p=nullptr;~Com(){if(p)p->Release();}T** out(){return &p;}T* operator->(){return p;}};
struct Bstr {BSTR p=nullptr;Bstr()=default;explicit Bstr(const wchar_t* s):p(SysAllocString(s)){if(!p)throw std::bad_alloc();}~Bstr(){SysFreeString(p);}Bstr(const Bstr&)=delete;};
void hr(HRESULT h){if(FAILED(h))throw std::runtime_error("cannot preserve WIM XML metadata");}
struct Apartment {HRESULT result=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);Apartment(){if(result!=RPC_E_CHANGED_MODE)hr(result);}~Apartment(){if(SUCCEEDED(result))CoUninitialize();}};
using GetInfo=BOOL(WINAPI*)(HANDLE,void**,DWORD*);
void load_xml(HMODULE m,HANDLE image,IXMLDOMDocument2** result){
    Com<IXMLDOMDocument2> doc;hr(CoCreateInstance(__uuidof(DOMDocument60),nullptr,CLSCTX_INPROC_SERVER,__uuidof(IXMLDOMDocument2),reinterpret_cast<void**>(doc.out())));
    hr(doc->put_async(VARIANT_FALSE));hr(doc->put_validateOnParse(VARIANT_FALSE));hr(doc->put_resolveExternals(VARIANT_FALSE));
    Bstr property(L"ProhibitDTD");VARIANT value{};value.vt=VT_BOOL;value.boolVal=VARIANT_TRUE;hr(doc->setProperty(property.p,value));
    void* info=nullptr;DWORD bytes=0;checked(api<GetInfo>(m,"WIMGetImageInformation")(image,&info,&bytes),"read WIM XML");
    struct Free {void* p;~Free(){LocalFree(p);}} free{info};
    checked(bytes%2==0&&bytes<16*1024*1024,"invalid WIM XML size");
    auto* begin=static_cast<const wchar_t*>(info);std::size_t chars=bytes/2;if(chars&&begin[0]==0xfeff){++begin;--chars;}while(chars&&begin[chars-1]==0)--chars;
    Bstr xml;xml.p=SysAllocStringLen(begin,static_cast<UINT>(chars));if(!xml.p)throw std::bad_alloc();VARIANT_BOOL ok=VARIANT_FALSE;hr(doc->loadXML(xml.p,&ok));checked(ok==VARIANT_TRUE,"invalid WIM XML");
    *result=doc.p;doc.p=nullptr;
}
void preserve_metadata(HMODULE m,HANDLE old_image,HANDLE new_image){
    Com<IXMLDOMDocument2> old_doc,new_doc;load_xml(m,old_image,old_doc.out());load_xml(m,new_image,new_doc.out());
    Com<IXMLDOMElement> old_root,new_root;hr(old_doc->get_documentElement(old_root.out()));hr(new_doc->get_documentElement(new_root.out()));
    Com<IXMLDOMNodeList> children;hr(old_root->get_childNodes(children.out()));long count=0;hr(children->get_length(&count));
    for(long i=0;i<count;++i){Com<IXMLDOMNode> child;hr(children->get_item(i,child.out()));Bstr name;hr(child->get_nodeName(&name.p));const std::wstring key=name.p;
        if(key==L"DIRCOUNT"||key==L"FILECOUNT"||key==L"TOTALBYTES"||key==L"HARDLINKBYTES"||key==L"LASTMODIFICATIONTIME")continue;
        DOMNodeType type{};hr(child->get_nodeType(&type));if(type!=NODE_ELEMENT)continue;
        // Replace old descriptive/custom fields, keeping freshly captured statistics.
        Com<IXMLDOMNode> existing;hr(new_root->selectSingleNode(name.p,existing.out()));if(existing.p){Com<IXMLDOMNode> removed;hr(new_root->removeChild(existing.p,removed.out()));}
        Com<IXMLDOMNode> copy,appended;hr(child->cloneNode(VARIANT_TRUE,copy.out()));hr(new_root->appendChild(copy.p,appended.out()));
    }
    Bstr text;hr(new_doc->get_xml(&text.p));std::wstring xml=L"\xfeff";xml.append(text.p,SysStringLen(text.p));
    checked(api<BOOL(WINAPI*)(HANDLE,void*,DWORD)>(m,"WIMSetImageInformation")(new_image,xml.data(),static_cast<DWORD>((xml.size()+1)*sizeof(wchar_t))),"write WIM XML");
}
std::filesystem::path relative_path(const std::string& text){return std::filesystem::u8path(edit_path(text));}
void no_reparse(const std::filesystem::path& root,const std::filesystem::path& relative){
    auto path=root;for(const auto& part:relative){path/=part;DWORD attr=GetFileAttributesW(path.c_str());if(attr==INVALID_FILE_ATTRIBUTES){auto err=GetLastError();checked(err==ERROR_FILE_NOT_FOUND||err==ERROR_PATH_NOT_FOUND,"cannot inspect WIM edit path");continue;}if(attr&FILE_ATTRIBUTE_REPARSE_POINT)throw EditUnsupported("editing through WIM reparse points is not supported");}
}
// The current WIMGAPI thread may impersonate a local account even when
// Total Commander was started by an elevated domain account. Anchor private
// workspace ownership and cleanup access to the stable host process SID.
struct ProcessUserSid {
    std::vector<BYTE> bytes;
    ProcessUserSid() {
        HANDLE raw=nullptr;
        checked(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw),
                "open WIM host user token");
        struct Token {HANDLE h;~Token(){CloseHandle(h);}} token{raw};
        DWORD size=0;
        GetTokenInformation(token.h,TokenUser,nullptr,0,&size);
        checked(size>=sizeof(TOKEN_USER) && size<65536u &&
                GetLastError()==ERROR_INSUFFICIENT_BUFFER,
                "size WIM host user SID");
        bytes.resize(size);
        checked(GetTokenInformation(token.h,TokenUser,bytes.data(),size,&size),
                "read WIM host user SID");
    }
    PSID sid() const {
        return reinterpret_cast<const TOKEN_USER*>(bytes.data())->User.Sid;
    }
};

struct WimScratchAcl {
    std::array<BYTE,SECURITY_MAX_SID_SIZE> system_sid{},admin_sid{};
    PACL acl=nullptr;
    WimScratchAcl(PSID process_user,DWORD inheritance) {
        DWORD system_bytes=static_cast<DWORD>(system_sid.size());
        DWORD admin_bytes=static_cast<DWORD>(admin_sid.size());
        checked(CreateWellKnownSid(WinLocalSystemSid,nullptr,
                                   system_sid.data(),&system_bytes),
                "create WIM SYSTEM SID");
        checked(CreateWellKnownSid(WinBuiltinAdministratorsSid,nullptr,
                                   admin_sid.data(),&admin_bytes),
                "create WIM Administrators SID");
        EXPLICIT_ACCESSW entries[3]{};
        const PSID identities[3]={process_user,system_sid.data(),admin_sid.data()};
        for(unsigned i=0;i<3;++i) {
            auto& entry=entries[i];
            entry.grfAccessPermissions=FILE_ALL_ACCESS;
            entry.grfAccessMode=GRANT_ACCESS;
            entry.grfInheritance=inheritance;
            entry.Trustee.TrusteeForm=TRUSTEE_IS_SID;
            entry.Trustee.TrusteeType=i==2?TRUSTEE_IS_GROUP:TRUSTEE_IS_USER;
            entry.Trustee.ptstrName=reinterpret_cast<LPWSTR>(identities[i]);
        }
        const DWORD result=SetEntriesInAclW(3,entries,nullptr,&acl);
        if(result!=ERROR_SUCCESS)
            throw std::runtime_error("create WIM workspace security ACL (Win32 "+
                                     std::to_string(result)+")");
    }
    WimScratchAcl(const WimScratchAcl&)=delete;
    ~WimScratchAcl(){if(acl)LocalFree(acl);}
};

// Create a private sibling workspace atomically with the desired DACL.
// A second SetSecurityInfo/WRITE_DAC call can fail with Win32 5 on elevated
// domain machines even when the caller has permission to create the folder.
// No owner reassignment, global takeown, or modification of parent/image.
bool create_wim_workspace(const std::filesystem::path& workspace) {
    ProcessUserSid user;
    WimScratchAcl acl(user.sid(),SUB_CONTAINERS_AND_OBJECTS_INHERIT);
    SECURITY_DESCRIPTOR security{};
    checked(InitializeSecurityDescriptor(&security,
                                         SECURITY_DESCRIPTOR_REVISION),
            "initialize WIM workspace descriptor");
    checked(SetSecurityDescriptorDacl(&security,TRUE,acl.acl,FALSE),
            "attach WIM workspace DACL");
    checked(SetSecurityDescriptorControl(
                &security,SE_DACL_PROTECTED,SE_DACL_PROTECTED),
            "protect WIM workspace DACL inheritance");
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength=sizeof(attributes);
    attributes.lpSecurityDescriptor=&security;
    attributes.bInheritHandle=FALSE;
    if(CreateDirectoryW(workspace.c_str(),&attributes))return true;
    const DWORD error=GetLastError();
    if(error==ERROR_ALREADY_EXISTS || error==ERROR_FILE_EXISTS)return false;
    SetLastError(error);
    checked(false,"create WIM workspace with explicit ACL");
    return false;
}

// Restore access ONLY for private WIM apply/capture scratch entries.
// An applied Windows image may contain TrustedInstaller-protected ACLs.
// Grant cleanup to the elevated host process user, SYSTEM and Administrators,
// without following a reparse point or changing the original WIM.
void allow_workspace_cleanup(const std::filesystem::path& path){
    // WIMGAPI can end impersonation or disable privileges while applying
    // an image. Rebuild a scoped privileged identity even if a thread
    // token is still present, and restore the caller identity on exit.
    Privileges cleanup_identity;
    ProcessUserSid user;
    WimScratchAcl acl(user.sid(),NO_INHERITANCE);
    HANDLE raw_file=CreateFileW(path.c_str(),
        WRITE_DAC|WRITE_OWNER,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,
        OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    checked(raw_file!=INVALID_HANDLE_VALUE,"open WIM scratch entry for ACL repair");
    struct File {HANDLE h;~File(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);}}
        file{raw_file};
    // An ACL/owner update on a hard-linked inode affects every link.
    // Delete-by-privilege is tried first; fail closed rather than
    // changing security metadata for multiply linked scratch entries.
    BY_HANDLE_FILE_INFORMATION original{};
    checked(GetFileInformationByHandle(file.h,&original),
            "inspect WIM scratch links before ACL repair");
    if(original.nNumberOfLinks!=1)
        throw EditUnsupported("WIM scratch ACL repair refused on a hard-linked entry");
    // An opened reparse point may point outside the scratch tree. We do not
    // follow it, and refuse a security edit if the link could not be deleted
    // with the previous privilege-only unlink attempt.
    if(original.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)
        throw EditUnsupported("WIM scratch ACL repair refused on a reparse point");

    const auto set_dacl_on=[&](HANDLE handle,bool protect) -> DWORD {
        return SetSecurityInfo(handle,SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION|
                (protect?PROTECTED_DACL_SECURITY_INFORMATION:0u),
            nullptr,nullptr,acl.acl,nullptr);
    };
    const auto set_owner=[&]() -> DWORD {
        // Apply ownership to the singly linked disposable scratch inode
        // only, never the original WIM or any other directory.
        return SetSecurityInfo(file.h,SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION,user.sid(),nullptr,nullptr,nullptr);
    };
    unsigned max_allowed_opened=0;
    unsigned max_allowed_identity_rejected=0;
    const auto checked_fresh_dacl=[&](DWORD access,bool protect) -> DWORD {
        // MAXIMUM_ALLOWED is a documented special case for SetSecurityInfo:
        // the call does NOT propagate inheritable ACEs into child files or
        // directories, which may be TrustedInstaller-owned and protected.
        // Never use this for the original WIM, parent directory, junctions,
        // hardlinks, or another operation's temporary workspace.
        HANDLE handle=CreateFileW(path.c_str(),access,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,
            OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|
                FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(handle==INVALID_HANDLE_VALUE)return GetLastError();
        File refreshed{handle};
        if(access==MAXIMUM_ALLOWED)++max_allowed_opened;
        BY_HANDLE_FILE_INFORMATION current{};
        if(!GetFileInformationByHandle(refreshed.h,&current))
            return GetLastError();
        if(current.nNumberOfLinks!=1 ||
           (current.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) ||
           current.dwVolumeSerialNumber!=original.dwVolumeSerialNumber ||
           current.nFileIndexHigh!=original.nFileIndexHigh ||
           current.nFileIndexLow!=original.nFileIndexLow) {
            if(access==MAXIMUM_ALLOWED)++max_allowed_identity_rejected;
            return ERROR_INVALID_DATA;
        }
        return set_dacl_on(refreshed.h,protect);
    };
    const auto outcome=wim_scratch::recover_private_scratch_acl(
        [&](bool protect){return set_dacl_on(file.h,protect);},
        set_owner,
        [&](bool protect) {
            return checked_fresh_dacl(WRITE_DAC|WRITE_OWNER,protect);
        },
        [&](bool protect) {
            // MUST request exactly MAXIMUM_ALLOWED; OR-ing WRITE_DAC or
            // other access bits defeats the documented no-propagation rule.
            return checked_fresh_dacl(MAXIMUM_ALLOWED,protect);
        });
    if(!outcome.ok()) {
        const bool owner_failed=
            outcome.owner_change!=wim_scratch::AclRecovery::unused &&
            outcome.owner_change!=ERROR_SUCCESS;
        throw std::runtime_error(std::string(
            owner_failed?"restore WIM scratch entry ownership":
                         "restore WIM scratch cleanup access")+
            outcome.counts()+
            " (max_allowed_handles_opened="+std::to_string(max_allowed_opened)+
            ", max_allowed_identity_rejected="+
                std::to_string(max_allowed_identity_rejected)+
            ") (Win32 "+std::to_string(outcome.final_error)+")");
    }
}
// An applied Windows image may contain protected folders, reparse entries
// and many thousands of files. Never delete children while a Win32 directory
// enumeration handle is still live: its handles can retain delete-pending
// entries and lead to ERROR_DIR_NOT_EMPTY (145) on RemoveDirectoryW.
void remove_tree(std::filesystem::path path,
                 const EditProgress& progress = {},
                 bool posix_delete = false) {
    // Literal paths are required ONLY for disposal of our private scratch,
    // never for a user's selected WIM file/directory removal or replacement.
    // A WIM applied on NTFS can contain names that the DOS Win32 parser
    // trims (trailing dots/spaces) or paths beyond MAX_PATH. Enumeration
    // sees those names, while normal GetFileAttributesW says FILE_NOT_FOUND.
    // Once in cleanup mode, retain the literal prefix in recursive paths.
    if(posix_delete)
        path=wim_scratch::literal_private_ntfs_path(path);
    constexpr unsigned max_cleanup_passes=10;
    const auto retry_wait=[](unsigned pass) {
        // Only 145/303 are retried; bounded waiting gives WIMGAPI/AV time
        // to close delete-pending scratch handles, without indefinite F5.
        Sleep(std::min(800u,100u*(pass+1u)));
    };
    // DeleteFileW success can mean delete-on-last-close, not namespace
    // removal. Windows may report ACCESS_DENIED when querying that same
    // delete-pending file, so remember the state across bounded passes.
    bool pending_legacy_delete=false;
    for(unsigned pass=0;pass<max_cleanup_passes;++pass) {
        const DWORD attr=GetFileAttributesW(path.c_str());
        if(attr==INVALID_FILE_ATTRIBUTES){
            const DWORD error=GetLastError();
            if(error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND)
                return;
            if((error==ERROR_DELETE_PENDING ||
                (error==ERROR_ACCESS_DENIED && pending_legacy_delete))) {
                if(pass+1u<max_cleanup_passes) {
                    edit_tick(progress);
                    retry_wait(pass);
                    continue;
                }
                SetLastError(ERROR_DELETE_PENDING);
                checked(false,
                    "WIM scratch deletion remained pending after bounded cleanup");
            }
            SetLastError(error);
            checked(false,"inspect WIM scratch entry before cleanup");
        }
        const bool directory=(attr&FILE_ATTRIBUTE_DIRECTORY)!=0;
        const bool recurse=directory && !(attr&FILE_ATTRIBUTE_REPARSE_POINT);
        if(recurse) {
            // Snapshot the directory first and close its iterator BEFORE
            // recursively deleting anything. This fixes a live Win32
            // FindFirstFile handle interacting with pending child deletions.
            std::vector<std::filesystem::path> children;
            std::error_code ec;
            {
                auto it=std::filesystem::directory_iterator(path,ec);
                if(ec==std::errc::permission_denied) {
                    allow_workspace_cleanup(path);
                    ec.clear();
                    it=std::filesystem::directory_iterator(path,ec);
                }
                if(!ec) {
                    const auto end=std::filesystem::directory_iterator{};
                    while(it!=end) {
                        children.push_back(it->path());
                        it.increment(ec);
                        if(ec)break;
                    }
                }
            } // All directory-iterator Win32 handles are closed here.
            if(ec) {
                if(ec.value()==ERROR_DELETE_PENDING &&
                   pass+1u<max_cleanup_passes) {
                    edit_tick(progress);
                    retry_wait(pass);
                    continue;
                }
                throw std::filesystem::filesystem_error(
                    "enumerate WIM scratch before delete",path,ec);
            }
            for(std::size_t i=0;i<children.size();++i) {
                if((i&127u)==0u)edit_tick(progress);
                remove_tree(children[i],progress,posix_delete);
            }
        }
        // Prefer temporary SeRestorePrivilege for deletion before modifying
        // NTFS security descriptors inherited from the WIM source.
        const auto retry_privileged=[&](auto&& operation,
                                        const char* failure) {
            DWORD error=ERROR_SUCCESS;
            {
                Privileges identity;
                if(operation())return true;
                error=GetLastError();
            }
            if(error!=ERROR_ACCESS_DENIED){
                SetLastError(error);
                checked(false,failure);
            }
            return false;
        };
        const auto clear_readonly=[&](){
            return !(attr&FILE_ATTRIBUTE_READONLY) ||
                   SetFileAttributesW(path.c_str(),
                                      attr&~FILE_ATTRIBUTE_READONLY);
        };
        if(!clear_readonly()){
            const DWORD error=GetLastError();
            if(error!=ERROR_ACCESS_DENIED){
                SetLastError(error);
                checked(false,"clear WIM scratch read-only flag");
            }
            if(!retry_privileged(clear_readonly,
                   "clear WIM scratch attributes with restore privilege")){
                allow_workspace_cleanup(path);
                checked(clear_readonly(),
                        "clear WIM scratch read-only flag after ACL repair");
            }
        }
        const auto unlink=[&]() -> DWORD {
            if(posix_delete) {
                // NTFS can retain deleted file or *empty directory* names
                // while a WIMGAPI/AV reader still holds a delete-sharing
                // handle. RemoveDirectoryW then reports Win32 145 for
                // an apparently empty parent. POSIX disposition removes the
                // name immediately if allowed, never following a reparse
                // point. Nonempty directories still fail closed.
                //
                // Invoked ONLY for entries in this transaction's private
                // post-Apply/Capture scratch workspace, never source WIMs.
                if(wim_scratch::try_posix_unlink_private_entry(path,directory))
                    return ERROR_SUCCESS;
            }
            if(directory) {
                if(RemoveDirectoryW(path.c_str()))return ERROR_SUCCESS;
                return GetLastError();
            }
            if(!DeleteFileW(path.c_str()))return GetLastError();
            if(!posix_delete)return ERROR_SUCCESS;
            // Legacy DeleteFileW can succeed merely by marking a name
            // DELETE_PENDING; the parent still reports Win32 145. Never
            // mistake that state for a completed private scratch cleanup.
            const DWORD remaining=GetFileAttributesW(path.c_str());
            if(remaining!=INVALID_FILE_ATTRIBUTES)
                return ERROR_DELETE_PENDING;
            const DWORD outcome=GetLastError();
            if(outcome==ERROR_FILE_NOT_FOUND ||
               outcome==ERROR_PATH_NOT_FOUND)
                return ERROR_SUCCESS;
            // Microsoft's DeleteFileW contract permits ACCESS_DENIED on a
            // later open of an already delete-pending name. We already
            // deleted successfully, so this is not evidence of a bad DACL.
            if(outcome==ERROR_ACCESS_DENIED ||
               outcome==ERROR_DELETE_PENDING)
                return ERROR_DELETE_PENDING;
            return outcome;
        };
        DWORD error=unlink();
        if(error==ERROR_ACCESS_DENIED) {
            // Under applied-image ACLs the first attempt may fail with 5.
            // Privileged unlink may instead observe a newly created file
            // (145) or an outstanding delete (303). Preserve that actual
            // result for the common retry loop instead of throwing inside
            // the privilege fallback and abandoning the verified WIM.
            {
                Privileges identity;
                error=unlink();
            }
            if(error==ERROR_ACCESS_DENIED) {
                // Modify security only for this private, singly-linked
                // scratch inode. The original WIM is never touched here.
                allow_workspace_cleanup(path);
                error=unlink();
            }
        }
        if(error==ERROR_SUCCESS || error==ERROR_FILE_NOT_FOUND ||
           error==ERROR_PATH_NOT_FOUND)
            return;
        if((recurse && error==ERROR_DIR_NOT_EMPTY) ||
           error==ERROR_DELETE_PENDING) {
            if(error==ERROR_DELETE_PENDING && !directory)
                pending_legacy_delete=true;
            if(pass+1u<max_cleanup_passes) {
                edit_tick(progress);
                retry_wait(pass);
                // Directory handles are closed; re-enumerate children that
                // appeared during any of the ACL/privilege deletion attempts.
                continue;
            }
            if(!recurse) {
                // A file can remain DELETE_PENDING while another process
                // retains its handle. Do not enumerate a regular file as
                // a directory, and do not claim a successful F5.
                SetLastError(error);
                checked(false,
                    "WIM scratch deletion remained pending after bounded cleanup");
            }
            // Only aggregate metadata and numeric Win32 categories leave
            // this private scratch tree; no filenames, paths or contents.
            // A DELETE-open probe distinguishes lingering handles, DACLs
            // and delete-pending names without modifying any survivor.
            const auto state=wim_scratch::probe_directory(path);
            // WIMGAPI and antivirus may drop delete-sharing handles while
            // the final snapshot is being taken. If ALL observed names
            // vanished during that snapshot, close its enumeration handle
            // and give the *same private directory* a bounded final retry.
            // Never infer success from vanished counts or Win32 145: only
            // a successful NTFS unlink / confirmed absent path may return.
            const bool vanished_only=state.scan_error==0 &&
                state.survivors>0 && state.survivors<32u &&
                state.survivors==state.vanished;
            if(wim_scratch::retry_vanished_private_directory(path,state))
                return;
            const auto reason=std::string(
                "WIM scratch deletion remained pending after bounded cleanup")+
                state.counts()+
                (vanished_only?" (vanished_retry_exhausted=1)":"");
            SetLastError(error);
            checked(false,reason.c_str());
        }
        SetLastError(error);
        checked(false,"remove WIM scratch entry");
    }
}
// WIMGAPI may create extra temporary files directly in its configured
// workspace, not just in the expanded image tree. Remove all of these
// AFTER WIMGAPI handles and readback handles are closed, BEFORE commit.
// The only item allowed to survive into the atomic rename is the verified WIM.
void remove_wim_residue(const std::filesystem::path& scratch_root,
                        const std::filesystem::path& output,
                        const EditProgress& progress) {
    // Enumerate via the same literal namespace used for deletion, or a
    // normalized path can list names that cannot be opened for deletion.
    const auto root=wim_scratch::literal_private_ntfs_path(scratch_root);
    // WIMGAPI may release temporary handles shortly after WIMCloseHandle.
    // Retry the whole sweep rather than failing on one late entry. The
    // verified staged WIM must be the ONLY remaining file before commit.
    constexpr unsigned max_residue_passes=6;
    std::exception_ptr last_failure;
    for(unsigned pass=0;pass<max_residue_passes;++pass) {
        edit_tick(progress);
        const DWORD root_attr=GetFileAttributesW(root.c_str());
        if(root_attr==INVALID_FILE_ATTRIBUTES ||
           !(root_attr&FILE_ATTRIBUTE_DIRECTORY) ||
           (root_attr&FILE_ATTRIBUTE_REPARSE_POINT))
            throw EditUnsupported("WIM edit workspace was replaced or removed");
        try {
            std::vector<std::filesystem::path> residue;
            bool found_output=false;
            // Enumerate first, close all iterator handles, then delete.
            for(const auto& entry:std::filesystem::directory_iterator(root)) {
                edit_tick(progress);
                if(entry.path().filename()==output.filename()) {
                    const DWORD attr=GetFileAttributesW(entry.path().c_str());
                    if(attr==INVALID_FILE_ATTRIBUTES ||
                       (attr&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))
                        throw EditUnsupported("WIM staging output is not a regular file");
                    found_output=true;
                }else residue.push_back(entry.path());
            }
            if(!found_output)
                throw EditUnsupported("verified WIM staging output disappeared");
            for(const auto& entry:residue) {
                edit_tick(progress);
                remove_tree(entry,progress,true);
            }
            bool clean=true;
            for(const auto& entry:std::filesystem::directory_iterator(root)) {
                edit_tick(progress);
                if(entry.path().filename()!=output.filename())clean=false;
            }
            if(clean)return;
            throw std::runtime_error(
                "WIM temporary files still remain before commit");
        }catch(const EditCancelled&) {
            throw;
        }catch(const EditUnsupported&) {
            // Structural safety failures must not be retried or hidden.
            throw;
        }catch(...) {
            last_failure=std::current_exception();
        }
        if(pass+1u<max_residue_passes) {
            edit_tick(progress);
            Sleep(std::min(1500u,200u*(pass+1u)));
        }
    }
    if(last_failure)std::rethrow_exception(last_failure);
    throw std::runtime_error("WIM transaction residue cleanup exhausted");
}
// Rollback must use the same ACL-aware deletion as successful WIM cleanup.
// Ordinary remove_all can leave TrustedInstaller-protected expanded files
// behind when cancellation or an Apply/Capture error unwinds the operation.
struct WimWorkspaceCleanup {
    std::filesystem::path folder;
    ~WimWorkspaceCleanup() noexcept {
        // WIMGAPI or filesystem filters may still be releasing files
        // during exception unwinding. Retain the impersonated imaging
        // token and retry ACL-aware cleanup of this one private workspace.
        // Never delete unrelated/older .iso-wcx-edit-* directories.
        for(unsigned attempt=0;attempt<4;++attempt) {
            try {remove_tree(folder,{},true);return;}catch(...) { cleanup_diagnostics::note(cleanup_diagnostics::Stage::wim_workspace,std::current_exception(),attempt+1); }
            if(attempt<3)Sleep(150u*(attempt+1u));
        }
    }
};
}
void edit_wim(const std::filesystem::path& path,const std::vector<ArchiveChange>& changes,
              const EditProgress& progress,const WimEditStatus& status){
    const auto report=[&](const wchar_t* label,unsigned percent){
        if(status && !status(label,percent))throw EditCancelled();
        edit_tick(progress);
    };
    report(L"Validate WIM",1);
    edit_tick(progress);wim_detail::Header header;if(!wim_detail::read_header(path,header))throw EditUnsupported("invalid WIM header");wim_detail::validate(header);
    if(header.total!=1 || (wim_detail::u32(header,16)&(0x4u|0x8u|0x10u|0x20u|0x80000u)))throw EditUnsupported("split, solid/ESD, delta and read-only WIM editing is not supported");
    const DWORD images=wim_detail::u32(header,44),boot=wim_detail::u32(header,120);
    if(!images||images>256||boot>images)throw EditUnsupported("invalid WIM image count");
    std::map<unsigned,std::vector<ArchiveChange>> edits;std::set<unsigned> deleted;
    for(auto c:changes){auto p=edit_path(c.path);auto slash=p.find('/');auto top=p.substr(0,slash);if(!top.starts_with("image")||top.size()<6)throw EditUnsupported("select an imageN folder inside the WIM");
        auto digits=top.substr(5);if(digits.empty()||digits.size()>3||digits[0]=='0'||digits.find_first_not_of("0123456789")!=std::string::npos)throw EditUnsupported("invalid WIM index");unsigned index=static_cast<unsigned>(std::stoul(digits));if(!index||index>images)throw EditUnsupported("creating WIM indexes is not supported yet");
        if(slash==std::string::npos){if(!c.remove)throw EditUnsupported("cannot replace an image index");deleted.insert(index);}else{c.path=p.substr(slash+1);edits[index].push_back(std::move(c));}}
    // Validate the entire WCX batch before creating the expensive NTFS
    // workspace. In particular, deleting imageN while also editing one of
    // its children must never report success for the ignored changes.
    for(const auto& [index,entries]:edits) {
        if(deleted.contains(index))
            throw EditUnsupported(
                "WIM batch cannot delete and modify the same image index");
        // Windows' NTFS path comparisons use Unicode case folding. A
        // bytewise ASCII transform misses e.g. Latin/Cyrillic case aliases
        // and can let two WCX source names overwrite the same staged target.
        const auto native=[](const std::string& utf8) {
            auto path=relative_path(utf8).wstring();
            for(auto& c:path)if(c==L'/')c=L'\\';
            return path;
        };
        const auto compare=[](const std::wstring& a,
                              const std::wstring& b) {
            const auto rc=CompareStringOrdinal(
                a.c_str(),static_cast<int>(a.size()),
                b.c_str(),static_cast<int>(b.size()),TRUE);
            if(!rc)throw EditUnsupported("invalid Unicode WIM batch path");
            return rc;
        };
        struct WinLess {
            bool operator()(const std::wstring& a,const std::wstring& b)
                const {
                const auto result=CompareStringOrdinal(
                    a.c_str(),static_cast<int>(a.size()),
                    b.c_str(),static_cast<int>(b.size()),TRUE);
                if(!result)
                    throw EditUnsupported("cannot compare Unicode WIM paths");
                return result==CSTR_LESS_THAN;
            }
        };
        std::set<std::wstring,WinLess> seen;
        for(const auto& entry:entries)
            if(!seen.insert(native(entry.path)).second)
                throw EditUnsupported("WIM batch contains duplicate target paths");
        for(const auto& removal:entries) {
            if(!removal.remove)continue;
            auto parent=native(removal.path);
            parent+=L'\\';
            for(const auto& added:entries) {
                if(added.remove)continue;
                const auto child=native(added.path);
                if(child.size()>=parent.size() &&
                   compare(child.substr(0,parent.size()),parent)==CSTR_EQUAL)
                    throw EditUnsupported(
                        "WIM batch cannot create inside a removed directory");
            }
        }
    }
    if(deleted.size()==images)throw EditUnsupported("cannot delete the last WIM image");
    // Existing WIMGAPI exports all untouched indexes into a new WIM.
    // When only the final index changes, it can instead be replaced inside
    // an isolated copy, keeping all preceding index records in place.
    // WIMDeleteImage cannot be used for one-image WIMs or non-final indexes:
    // the former is rejected by Windows; the latter would renumber indexes.
    const bool replace_last_only=images>1 && deleted.empty() &&
                                 edits.size()==1 && edits.contains(images);
    // For any single-index edit, the native export/capture path already
    // validates unchanged indexes; avoid a second full directory walk.
    const unsigned verify_only_index=deleted.empty() && edits.size()==1
                                         ?edits.begin()->first:0u;
    report(L"Prepare NTFS staging",3);
    // A WCX host/antivirus can keep the source open without FILE_SHARE_DELETE.
    // The final MoveFileExW replacement would then fail only AFTER the costly
    // Apply/Capture. Detect an unequivocal sharing conflict before staging.
    // Other DELETE access failures are intentionally non-fatal here: ACLs on
    // the parent directory can still permit a final atomic replacement.
    {
        HANDLE probe=CreateFileW(path.c_str(),DELETE,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(probe==INVALID_HANDLE_VALUE) {
            const DWORD failure=GetLastError();
            if(failure==ERROR_SHARING_VIOLATION ||
               failure==ERROR_LOCK_VIOLATION)
                throw EditUnsupported(
                    "WIM archive is open without delete sharing (Win32 "+
                    std::to_string(failure)+")");
        } else CloseHandle(probe);
    }
    Library library;auto m=library.h;
    report(L"Check WIM mount state and edit lock",4);
    // Machine-local lease prevents overlapping WCX transactions from two
    // Total Commander processes, including x86/x64 and image hardlinks.
    wim_preflight::ImageLease lease(path);
    // No DISM mount/unmount is invoked: registered mounts remain untouched.
    wim_preflight::reject_registered_mounts(m,path);

    // WIMGAPI will materialize an expanded NTFS tree and a staged WIM.
    // Refuse clearly insufficient space BEFORE creating any private
    // workspace. This lower bound is deliberately not a prediction of
    // the final uncompressed Apply size.
    report(L"Check WIM staging capacity",4);
    // GetVolumePathNameW requires a fully qualified input; callers of the
    // WCX interface may pass relative archive paths.
    const auto staging_source=std::filesystem::absolute(path);
    wchar_t scratch_volume[MAX_PATH]{};
    checked(GetVolumePathNameW(staging_source.c_str(),scratch_volume,MAX_PATH),
            "locate WIM staging volume");
    ULARGE_INTEGER available{};
    checked(GetDiskFreeSpaceExW(scratch_volume,&available,nullptr,nullptr),
            "query WIM staging capacity");
    const auto capacity=wim_detail::wim_staging_budget(
        std::filesystem::file_size(path),available.QuadPart);
    if(capacity.arithmetic_overflow)
        throw EditUnsupported("WIM staging capacity arithmetic overflow");
    if(!capacity.enough)
        throw EditUnsupported(
            "WIM staging capacity insufficient (available_mib="+
            std::to_string(wim_detail::wim_mib_ceil(capacity.available_bytes))+
            ", minimum_required_mib="+
            std::to_string(wim_detail::wim_mib_ceil(capacity.required_bytes))+
            "; expanded image requires additional free space)");
    Privileges privileges;Apartment apartment;
    // Create the unique sibling directory with the ACL already attached.
    // Atomic creation works for domain-admin TC without WRITE_DAC on an
    // existing directory and avoids a security-transition window.
    EditTransaction tx(path,&create_wim_workspace);
    WimWorkspaceCleanup scratch{tx.workspace};
    // Native Windows capture/apply preserve security and alternate streams on NTFS.
    wchar_t volume[MAX_PATH]{},filesystem[MAX_PATH]{};checked(GetVolumePathNameW(tx.workspace.c_str(),volume,MAX_PATH),"find WIM workspace volume");checked(GetVolumeInformationW(volume,nullptr,0,nullptr,nullptr,nullptr,filesystem,MAX_PATH),"inspect WIM workspace filesystem");
    if(_wcsicmp(filesystem,L"NTFS")!=0)throw EditUnsupported("WIM editing requires an NTFS workspace volume");
    auto create=api<HANDLE(WINAPI*)(PCWSTR,DWORD,DWORD,DWORD,DWORD,PDWORD)>(m,"WIMCreateFile");auto close=api<Close>(m,"WIMCloseHandle");auto temp=api<BOOL(WINAPI*)(HANDLE,PCWSTR)>(m,"WIMSetTemporaryPath");auto load=api<HANDLE(WINAPI*)(HANDLE,DWORD)>(m,"WIMLoadImage");
    Callback callback{progress,status};
    report(L"Open WIM indexes",5);
    {
        Handle source(create(path.c_str(),GENERIC_READ,OPEN_EXISTING,2,0,nullptr),close);
        const DWORD flags=wim_detail::u32(header,16);const DWORD compression=(flags&0x40000)?2u:(flags&0x20000)?1u:0u;
        if(replace_last_only){
            // Copy, never mutate, the original. A failure or cancellation
            // destroys only the private workspace in EditTransaction.
            callback.begin_stage(L"Copy WIM (preserve unchanged indexes)",6,17);
            CopyContext copy{callback,6,17};
            const BOOL copied=CopyFileExW(path.c_str(),tx.output.c_str(),
                                          &CopyContext::on_progress,&copy,nullptr,0);
            if(!copied){
                if(callback.cancelled.load())throw EditCancelled();
                checked(false,"copy WIM before selected-index edit");
            }
        }
        Handle output(create(tx.output.c_str(),GENERIC_READ|GENERIC_WRITE,
                             replace_last_only?OPEN_EXISTING:CREATE_NEW,2,
                             replace_last_only?0u:compression,nullptr),close);
        checked(temp(source.h,tx.workspace.c_str())&&temp(output.h,tx.workspace.c_str()),"set WIM temporary directory");Registered source_callback(m,source.h,callback),output_callback(m,output.h,callback);
        if(replace_last_only){
            callback.begin_stage(L"Remove old final WIM index",23,0);
            // No output image handles are open. This is safe only for the
            // last index in a multi-image, unsplit WIM.
            checked(api<BOOL(WINAPI*)(HANDLE,DWORD)>(m,"WIMDeleteImage")(output.h,images),
                    "remove old final WIM index from staged copy");
        }
        unsigned out_index=replace_last_only?images-1u:0u;
        unsigned new_boot=replace_last_only?boot:0u;
        for(unsigned index=1;index<=images;++index){
            callback.tick();
            if(replace_last_only && index!=images)continue;
            const unsigned begin=replace_last_only?25u:
                6u+static_cast<unsigned>(75ull*(index-1u)/images);
            const unsigned end=replace_last_only?81u:
                6u+static_cast<unsigned>(75ull*index/images);
            const std::wstring number=std::to_wstring(index)+L"/"+std::to_wstring(images);
            if(deleted.count(index)){
                callback.begin_stage(L"Skipping removed WIM image "+number,begin,0);
                continue;
            }
            ++out_index;if(boot==index)new_boot=out_index;
            if(!edits.count(index)){
                Handle image(load(source.h,index),close);
                callback.begin_stage(L"Export unchanged WIM image "+number,begin,end-begin);
                checked(api<BOOL(WINAPI*)(HANDLE,HANDLE,DWORD)>(m,"WIMExportImage")(image.h,output.h,1),"export unchanged WIM index");
                callback.tick();
                continue;
            }
            // Use a unique Apply directory for each edited image. A folder
            // awaiting WIMGAPI handle release cannot contaminate the next.
            const auto expanded=tx.workspace/
                (edits.size()==1 ? std::wstring(L"expanded") :
                 std::wstring(L"expanded-image")+std::to_wstring(index));
            checked(std::filesystem::create_directory(expanded),
                    "create fresh WIM staging directory");
            struct Cleanup {
                std::filesystem::path path;
                bool complete=false;
                ~Cleanup() noexcept {
                    if(!complete)try{remove_tree(path,{},true);}catch(...) { cleanup_diagnostics::note(cleanup_diagnostics::Stage::wim_expanded,std::current_exception()); }
                }
            } cleanup{expanded};
            // Loaded/captured WIMGAPI handles must close BEFORE scratch deletion.
            {
            Handle image(load(source.h,index),close);
            const unsigned apply_end=begin+(end-begin)*55u/100u;
            callback.begin_stage(L"Extract WIM image "+number,begin,apply_end-begin);
            const BOOL applied=api<BOOL(WINAPI*)(HANDLE,PCWSTR,DWORD)>(m,"WIMApplyImage")(image.h,expanded.c_str(),2u|0x100u);
            callback.tick();checked(applied,"apply WIM index");
            // A multi-GB Apply may take many minutes. Do not recapture over
            // a source WIM whose file ID or NTFS ChangeTime has changed.
            tx.assert_source_unchanged();
            callback.begin_stage(L"Edit WIM image "+number,apply_end,0);
            for(const auto& c:edits[index]){
                callback.tick();
                auto relative=relative_path(c.path);no_reparse(expanded,relative);auto target=expanded/relative;
                if(c.remove){remove_tree(target,progress);continue;}
                if(c.directory){std::filesystem::create_directories(target);continue;}
                checked(std::filesystem::is_regular_file(std::filesystem::symlink_status(c.source)),"source must be a regular file");
                std::filesystem::create_directories(target.parent_path());
                if(std::filesystem::exists(target)){
                    if(std::filesystem::is_directory(target))throw EditUnsupported("cannot replace a directory with a file");
                    remove_tree(target,progress);
                }
                // CopyFileW can block for a long time on large F5 sources and
                // offers no cancellation point. Reuse the WCX-aware callback:
                // this edit phase has zero percentage span, so progress stays
                // monotonic while Total Commander can still cancel promptly.
                callback.begin_stage(L"Copy F5 source into WIM workspace "+number,
                                     apply_end,0);
                CopyContext source_copy{callback,apply_end,0};
                const BOOL copied_source=CopyFileExW(
                    c.source.c_str(),target.c_str(),
                    &CopyContext::on_progress,&source_copy,nullptr,
                    COPY_FILE_FAIL_IF_EXISTS);
                if(!copied_source) {
                    if(callback.cancelled.load())throw EditCancelled();
                    checked(false,"copy replacement into WIM workspace");
                }
                callback.tick();
            }
            callback.begin_stage(L"Rebuild WIM image "+number,apply_end,end-apply_end);
            HANDLE captured=api<HANDLE(WINAPI*)(HANDLE,PCWSTR,DWORD)>(m,"WIMCaptureImage")(output.h,expanded.c_str(),2u|0x100u);
            callback.tick();Handle result(captured,close);preserve_metadata(m,image.h,result.h);
            }
            callback.begin_stage(L"Cleanup WIM temporary files "+number,end,0);
            // A WIMGAPI worker or AV filter may still hold a scratch file.
            // If early cleanup fails, retry after ALL source/output WIMGAPI
            // handles close. Final residue cleanup is mandatory before commit.
            try {
                remove_tree(expanded,progress,true);
            }catch(const EditCancelled&) {
                throw;
            }catch(const std::exception&) {
                cleanup_diagnostics::note(cleanup_diagnostics::Stage::wim_expanded,std::current_exception());
                // Private folder remains under the guarded transaction root.
                // Never commit while any of its contents survive.
            }
            cleanup.complete=true;
        }
        checked(api<DWORD(WINAPI*)(HANDLE)>(m,"WIMGetImageCount")(output.h)==out_index,
                "staged WIM index count changed unexpectedly");
        checked(api<BOOL(WINAPI*)(HANDLE,DWORD)>(m,"WIMSetBootImage")(output.h,new_boot),"preserve WIM boot index");
    }
    // Reopen the rebuilt image, validate all index trees, and independently
    // compare newly supplied streams. Do not re-decompress every unchanged
    // file: this made creating even an empty directory take minutes longer.
    report(L"Verify WIM structure",85);
    {
        // For a single-index edit, enumerate only the changed directory
        // tree. Image count and export status have already been checked.
        WimArchive verified(tx.output,verify_only_index);
        std::map<std::string,std::size_t> found;
        const auto& items=verified.items();
        for(std::size_t i=0;i<items.size();++i){
            // Metadata may contain hundreds of thousands of entries. Avoid
            // a UI callback for each one; checking cancellation every 1024
            // entries is responsive without monopolizing Total Commander.
            if((i&1023u)==0u)callback.tick();
            if(!found.emplace(items[i].path,i).second)
                throw EditUnsupported("duplicate WIM output path");
        }
        unsigned new_index=0;
        for(unsigned index=1;index<=images;++index){
            if(deleted.count(index))continue;
            ++new_index;
            if(verify_only_index && new_index!=verify_only_index)continue;
            const auto prefix="image"+std::to_string(new_index);
            if(!found.contains(prefix))throw EditUnsupported("missing output WIM image index");
            if(!edits.contains(index))continue;
            for(const auto& change:edits.at(index)){
                callback.tick();
                const auto key=prefix+"/"+change.path;
                const auto entry=found.find(key);
                if(change.remove){
                    if(entry!=found.end())throw EditUnsupported("removed WIM entry still exists");
                    continue;
                }
                if(entry==found.end())throw EditUnsupported("new WIM entry is missing");
                const auto& item=items[entry->second];
                if(item.directory!=change.directory)
                    throw EditUnsupported("WIM edited entry has wrong type");
                if(change.directory)continue;
                const auto length=std::filesystem::file_size(change.source);
                if(item.size!=length)throw EditUnsupported("WIM edited file size differs");
                std::ifstream input(change.source,std::ios::binary);
                if(!input)throw EditUnsupported("cannot verify WIM added file source");
                bool mismatch=false;
                std::array<char,64*1024> buffer{};
                const bool read=verified.read(entry->second,[&](const std::uint8_t* data,std::size_t bytes){
                    callback.tick();
                    std::size_t offset=0;
                    while(offset<bytes){
                        const auto n=std::min(buffer.size(),bytes-offset);
                        input.read(buffer.data(),static_cast<std::streamsize>(n));
                        if(input.gcount()!=static_cast<std::streamsize>(n) ||
                           std::memcmp(buffer.data(),data+offset,n)!=0){
                            mismatch=true;return false;
                        }
                        offset+=n;
                    }
                    return true;
                });
                if(!read || mismatch || input.peek()!=std::char_traits<char>::eof())
                    throw EditUnsupported("WIM edited file readback mismatch");
            }
        }
    }
    report(L"Cleanup WIM transaction scratch",98);
    remove_wim_residue(tx.workspace,tx.output,progress);
    // DISM may have registered a mount while Apply/Capture was running.
    // Refuse to replace the source rather than touching an active mount.
    wim_preflight::reject_registered_mounts(m,path);
    report(L"Commit verified WIM",99);
    tx.commit(progress);
    // No cancellation or throwing callbacks after the atomic commit.
}
}
#endif
