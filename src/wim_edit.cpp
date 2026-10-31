#ifdef _WIN32
#include "iso_wcx/archive_edit.hpp"
#include "iso_wcx/edit_transaction.hpp"
#include "iso_wcx/wim_archive.hpp"
#include "iso_wcx/wim_parts.hpp"
#include "iso_wcx/wim_mount_fast_plan.hpp"
#include "iso_wcx/wim_mount_recovery.hpp"
#include "iso_wcx/wim_mount_state.hpp"
#include <msxml6.h>
#include <map>
#include <mutex>
#include <set>
#include <atomic>
#include <cstring>
#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>

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
        HANDLE source=prior;
        if(!source)checked(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY|TOKEN_DUPLICATE,&source),"cannot read process token");
        const BOOL copied=DuplicateTokenEx(source,TOKEN_QUERY|TOKEN_ADJUST_PRIVILEGES|TOKEN_IMPERSONATE,nullptr,SecurityImpersonation,TokenImpersonation,&token);
        if(!prior)CloseHandle(source);
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
// Reject reparse ancestors even when deleting a selected symlink/junction.
// A reparse LEAF is safe to unlink via remove_expanded_entry(), which never
// descends into its target. Folder creation and replacement stay strict.
void no_reparse(const std::filesystem::path& root,
                const std::filesystem::path& relative,
                bool allow_leaf_reparse=false) {
    auto path=root;
    for(auto it=relative.begin();it!=relative.end();++it) {
        path/=*it;
        const DWORD attr=GetFileAttributesW(path.c_str());
        if(attr==INVALID_FILE_ATTRIBUTES) {
            const DWORD error=GetLastError();
            checked(error==ERROR_FILE_NOT_FOUND ||
                    error==ERROR_PATH_NOT_FOUND,"cannot inspect WIM edit path");
            continue;
        }
        if((attr&FILE_ATTRIBUTE_REPARSE_POINT) &&
           !(allow_leaf_reparse && std::next(it)==relative.end()))
            throw EditUnsupported("editing through WIM reparse points is not supported");
    }
}
// WIMApplyImage may restore Windows system-folder DACLs that deny ordinary
// recursive deletion. Delete selected entries only through the same scoped,
// reparse-safe NTFS cleanup engine used by private WIM workspaces.
// The WIMGAPI Apply/Capture percentages are checkpoints, not a claim that
// recursive deletion has a known total. Report an exact visited-entry count
// and honor cancellation at the start and every 128 deleted-tree entries.
struct WimDeleteStatus {
    Callback& wim;
    const WimEditStatus& status;
    unsigned checkpoint;
    static bool poll(void* opaque,std::uint64_t visited) noexcept {
        auto& state=*static_cast<WimDeleteStatus*>(opaque);
        try {
            state.wim.tick();
            if(!state.status)return true;
            const auto label=L"Delete from WIM: visited "+
                std::to_wstring(visited)+L" entries";
            return state.status(label.c_str(),state.checkpoint);
        }catch(...) {
            return false;
        }
    }
};
void remove_staged_wim_entry(const std::filesystem::path& workspace,
                             const std::filesystem::path& expanded,
                             const std::filesystem::path& target,
                             Callback& callback,
                             const WimEditStatus& status,
                             unsigned checkpoint) {
    WimDeleteStatus state{callback,status,checkpoint};
    workspace_cleanup::DeletePulse pulse{
        &WimDeleteStatus::poll,&state};
    const auto result=workspace_cleanup::remove_expanded_entry(
        workspace,expanded,target,&pulse);
    if(result.clean)return;
    if(result.error==ERROR_CANCELLED)throw EditCancelled();
    const auto detail="WIM staged delete failed (Win32 "+
        std::to_string(result.error)+")";
    if(result.unsafe)
        throw EditUnsupported(detail+
            ": unsafe path, active mount or reparse ancestor");
    throw std::runtime_error(detail);
}

// The fast folder-only path is deliberately opt-in during initial field
// validation. No untested driver mount is attempted in existing Stable builds.
// WIM_GENERIC_MOUNT == GENERIC_EXECUTE; this uses the Windows system WIMGAPI,
// never DISM.EXE, wimlib-imagex, a runtime DLL or a separate worker EXE.
bool fast_mount_requested() {
    // A process-level override is especially useful for CI and immediate
    // emergency opt-out: 0 always disables; 1 enables regardless of GUI.
    wchar_t setting[16]{};
    SetLastError(ERROR_SUCCESS);
    const DWORD n=GetEnvironmentVariableW(
        L"ISO_WCX_WIM_FAST_MOUNT",setting,16);
    if(n!=0 || GetLastError()!=ERROR_ENVVAR_NOT_FOUND)
        return n==1 && setting[0]==L'1';
    // User preference is stored independently from the updater settings.
    // Unset or malformed values fail closed to the old Apply/Capture path.
    constexpr wchar_t registry_key[]=L"Software\\ISO-WCX\\WIM";
    DWORD enabled=0,size=sizeof(enabled);
    return RegGetValueW(HKEY_CURRENT_USER,registry_key,
              L"FastMountFolders",RRF_RT_REG_DWORD,nullptr,&enabled,
              &size)==ERROR_SUCCESS && enabled==1;
}
std::vector<wim_detail::WimTreeItem> tree_snapshot(const WimArchive& archive) {
    std::vector<wim_detail::WimTreeItem> result;
    result.reserve(archive.items().size());
    for(const auto& item:archive.items())
        result.push_back({item.path,item.size,item.attributes,item.dos_time,
                          item.directory});
    return result;
}
struct WimStageCopy {
    const EditProgress& progress;
    const WimEditStatus& status;
    ULONGLONG last_signal=0;
    bool cancelled=false;
};
DWORD CALLBACK wim_stage_copy_callback(
    LARGE_INTEGER total,LARGE_INTEGER transferred,LARGE_INTEGER,
    LARGE_INTEGER,DWORD,DWORD,HANDLE,HANDLE,LPVOID opaque) noexcept {
    auto& state=*static_cast<WimStageCopy*>(opaque);
    try {
        const auto now=GetTickCount64();
        if(now-state.last_signal>=200u || transferred.QuadPart==total.QuadPart) {
            state.last_signal=now;
            const auto bytes=total.QuadPart>0 ?
                std::clamp<LONGLONG>(transferred.QuadPart,0,total.QuadPart) : 0;
            const unsigned percent=total.QuadPart>0 ?
                6u+static_cast<unsigned>(20ull*static_cast<std::uint64_t>(bytes)/
                                          static_cast<std::uint64_t>(total.QuadPart)) : 6u;
            if((state.status && !state.status(L"Clone WIM (fast folder edit)",percent)) ||
               (state.progress && !state.progress())) {
                state.cancelled=true;
                return PROGRESS_CANCEL;
            }
        }
        return PROGRESS_CONTINUE;
    }catch(...) {
        state.cancelled=true;
        return PROGRESS_CANCEL;
    }
}
void fast_mount_folder_edit(
        const std::filesystem::path& path,
        const wim_detail::MountFolderPlan& plan,
        const EditProgress& progress,const WimEditStatus& status) {
    auto report=[&](const wchar_t* label,unsigned percent) {
        if(status && !status(label,percent))throw EditCancelled();
        edit_tick(progress);
    };
    report(L"Validate WIM for fast folder edit",2);
    std::vector<wim_detail::WimTreeItem> original;
    {
        // Only the selected WIM index can gain folders. Enumerating every
        // unrelated Windows edition here doubled the cost of both the
        // preflight and the independent post-commit parity check.
        const WimArchive existing(path,plan.index);
        original=tree_snapshot(existing);
    }
    // Skip an expensive transaction entirely if each folder already exists.
    // WIM can encode two names which Windows treats as the same path,
    // and NTFS is usually case-insensitive. Reject ambiguous aliases before
    // cloning any multi-gigabyte WIM or making a mount point.
    std::unordered_map<std::string,bool> known;
    std::unordered_map<std::string,std::string> case_aliases;
    known.reserve(original.size());
    case_aliases.reserve(original.size());
    for(const auto& item:original) {
        if(!known.emplace(item.path,item.directory).second ||
           !case_aliases.emplace(
               wim_detail::windows_case_key(item.path),item.path).second)
            throw EditUnsupported("ambiguous or case-colliding WIM paths");
    }
    const std::string prefix="image"+std::to_string(plan.index)+"/";
    bool all_exist=true;
    for(const auto& relative:plan.paths) {
        std::size_t start=0;
        while(start<relative.size()) {
            const auto slash=relative.find('/',start);
            const auto end=slash==std::string::npos?relative.size():slash;
            const auto key=prefix+relative.substr(0,end);
            const auto alias=case_aliases.find(wim_detail::windows_case_key(key));
            if(alias!=case_aliases.end() && alias->second!=key)
                throw EditUnsupported(
                    "WIM folder spelling differs from an existing NTFS name");
            const auto hit=known.find(key);
            if(hit!=known.end() && !hit->second)
                throw EditUnsupported("cannot create WIM folder beneath an existing file");
            if(slash==std::string::npos) {
                if(hit==known.end())all_exist=false;
                break;
            }
            start=slash+1;
        }
    }
    if(all_exist) {
        report(L"WIM folders already exist",99);
        return;
    }

    // Keep a recoverable sibling clone. Even a failed mount must never
    // touch the original WIM or delete a still-mounted Windows directory.
    Privileges privileges;
    // Fail before the potentially multi-gigabyte stage clone if the host
    // does not export its native Windows WIM mount/commit entry points.
    Library library;
    HMODULE dll=library.h;
    using Mount=BOOL(WINAPI*)(HANDLE,PCWSTR,DWORD);
    using Commit=BOOL(WINAPI*)(HANDLE,DWORD,PHANDLE);
    using Unmount=BOOL(WINAPI*)(HANDLE,DWORD);
    auto mount=api<Mount>(dll,"WIMMountImageHandle");
    auto commit=api<Commit>(dll,"WIMCommitImageHandle");
    auto unmount=api<Unmount>(dll,"WIMUnmountImageHandle");
    auto create=api<HANDLE(WINAPI*)(PCWSTR,DWORD,DWORD,DWORD,DWORD,PDWORD)>(
        dll,"WIMCreateFile");
    auto close=api<Close>(dll,"WIMCloseHandle");
    auto temp=api<BOOL(WINAPI*)(HANDLE,PCWSTR)>(dll,"WIMSetTemporaryPath");
    auto load=api<HANDLE(WINAPI*)(HANDLE,DWORD)>(dll,"WIMLoadImage");
    EditTransaction tx(path);
    const auto folder=tx.workspace/L"mounted";
    const auto scratch=tx.workspace/L"scratch";
    const auto mount_name=folder.wstring();
    if(mount_name.size()>=MAX_PATH || scratch.wstring().size()>=MAX_PATH)
        throw EditUnsupported("WIM fast mount path exceeds Windows MAX_PATH");
    wchar_t volume[MAX_PATH]{},filesystem[MAX_PATH]{};
    checked(GetVolumePathNameW(tx.workspace.c_str(),volume,MAX_PATH),
            "inspect mount workspace volume");
    checked(GetVolumeInformationW(volume,nullptr,0,nullptr,nullptr,nullptr,
                                  filesystem,MAX_PATH),
            "inspect mount workspace filesystem");
    if(_wcsicmp(filesystem,L"NTFS")!=0)
        throw EditUnsupported("WIM mount fast path requires an NTFS volume");
    // CopyFileEx would otherwise fill the drive and fail after many minutes.
    // Verify quota-aware caller-available space on the actual sibling mount
    // volume, with a conservative allowance for mount scratch and metadata.
    ULARGE_INTEGER available{};
    checked(GetDiskFreeSpaceExW(tx.workspace.c_str(),&available,
                                nullptr,nullptr),
            "inspect NTFS free space for fast WIM staging");
    const auto source_size=std::filesystem::file_size(path);
    if(!wim_detail::enough_staging_space(
          static_cast<std::uint64_t>(source_size),available.QuadPart))
        throw EditUnsupported(
            "insufficient NTFS free space for WIM clone and mount scratch");
    std::filesystem::create_directory(folder);
    std::filesystem::create_directory(scratch);
    report(L"Stage WIM before mounting",6);
    WimStageCopy copy{progress,status};
    BOOL aborted=FALSE;
    const BOOL cloned=CopyFileExW(path.c_str(),tx.output.c_str(),
                                 &wim_stage_copy_callback,&copy,&aborted,
                                 COPY_FILE_FAIL_IF_EXISTS);
    if(!cloned) {
        if(copy.cancelled || GetLastError()==ERROR_REQUEST_ABORTED)
            throw EditCancelled();
        checked(false,"clone WIM into private edit workspace");
    }
    checked(std::filesystem::file_size(tx.output)==std::filesystem::file_size(path),
            "WIM staged clone changed length");
    // Fail before invoking the privileged mount driver if another process
    // modified/replaced the source while we copied the multi-gigabyte WIM.
    // The commit repeats this check after unmount and verification.
    tx.assert_source_unchanged();
    wim_detail::Header stage_header{};
    if(!wim_detail::read_header(tx.output,stage_header))
        throw EditUnsupported("cloned WIM header is invalid");
    wim_detail::validate(stage_header);
    report(L"Mount selected WIM image",28);
    // The mount driver can outlive the process. Persist this private,
    // operation-specific recovery note before registering the mount.
    {
        const auto note=wim_detail::make_mount_recovery_note(
            std::filesystem::absolute(path),tx.output,folder,plan.index);
        const auto marker=tx.workspace/L"WIM-MOUNT-RECOVERY.txt";
        HANDLE handle=CreateFileW(marker.c_str(),GENERIC_WRITE,0,nullptr,
                                  CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        checked(handle!=INVALID_HANDLE_VALUE,"create WIM recovery note");
        DWORD written=0;
        const BOOL wrote=WriteFile(handle,note.data(),
                                  static_cast<DWORD>(note.size()),&written,nullptr);
        const BOOL durable=wrote && written==note.size() && FlushFileBuffers(handle);
        CloseHandle(handle);
        checked(durable,"flush WIM recovery note");
    }

    // WIMGAPI's mount driver may not be installed on every Windows host.
    // This Preview mode refuses and preserves the original if unavailable.
    {
        // Windows WIMGAPI mount access is the GENERIC_EXECUTE bit. Keep
        // READ|WRITE as well so WIMCommitImageHandle can update the staged WIM.
        Handle container(create(tx.output.c_str(),
                          GENERIC_READ|GENERIC_WRITE|GENERIC_EXECUTE,
                          OPEN_EXISTING,0,0,nullptr),close);
        checked(temp(container.h,scratch.c_str()),
                "configure WIM fast mount scratch directory");
        Handle image(load(container.h,plan.index),close);
        Callback callback{progress,status};
        Registered registration(dll,container.h,callback);
        bool mounted=false;
        bool unmount_attempted=false;
        try {
            // FAST flag supports lazy Windows WIM mounts. No fallback after
            // invoking mount: failed mounts must be diagnosed/cleaned explicitly.
            constexpr DWORD WIM_FLAG_MOUNT_FAST=0x00000400u;
            constexpr DWORD WIM_FLAG_NO_RP_FIX=0x00000100u;
            if(!mount(image.h,folder.c_str(),
                      WIM_FLAG_MOUNT_FAST|WIM_FLAG_NO_RP_FIX)) {
                const DWORD error=GetLastError();
                // Failed WIMMountImageHandle does not imply a live mount.
                // Previously we ALWAYS preserved a multi-GB clone, even
                // when the Windows driver registered nothing (DISM empty).
                // Only discard our private stage if the native WIMGAPI
                // registry AND reparse-point check conclusively say safe.
                if(wim_detail::inspect_private_mount(
                        dll,folder,tx.workspace)!=
                   wim_detail::PrivateMountState::Unmounted)
                    tx.preserve();
                throw std::runtime_error("WIM fast mount failed (Win32 "+
                    std::to_string(error)+"); workspace: "+
                    tx.workspace.string());
            }
            mounted=true;
            report(L"Create WIM folders without applying image",39);
            for(const auto& relative_name:plan.paths) {
                const auto relative=relative_path(relative_name);
                no_reparse(folder,relative);
                const auto destination=folder/relative;
                std::filesystem::create_directories(destination);
                no_reparse(folder,relative);
                const auto attrs=GetFileAttributesW(destination.c_str());
                checked(attrs!=INVALID_FILE_ATTRIBUTES &&
                        (attrs&FILE_ATTRIBUTE_DIRECTORY)!=0 &&
                        (attrs&FILE_ATTRIBUTE_REPARSE_POINT)==0,
                        "verify new WIM directory attributes");
                callback.tick();
            }
            report(L"Commit fast WIM metadata",60);
            // 0 updates the selected index in place, never adds a new index.
            // Do not request VERIFY here; a full stream re-read would defeat
            // folder-only performance. Independently verify the resulting tree.
            checked(commit(image.h,0,nullptr),"commit mounted WIM metadata");
            report(L"Unmount fast WIM edit",83);
            // WIMGAPI may have partially released the mount on failure.
            // Never invoke Unmount twice on a possibly stale image handle.
            unmount_attempted=true;
            if(!unmount(image.h,0)) {
                const DWORD error=GetLastError();
                if(wim_detail::inspect_private_mount(
                       dll,folder,tx.workspace)!=
                   wim_detail::PrivateMountState::Unmounted)
                    tx.preserve();
                throw std::runtime_error("WIM fast unmount failed (Win32 "+
                    std::to_string(error)+"); recover workspace if mounted: "+
                    tx.workspace.string());
            }
            mounted=false;
        }catch(...) {
            if(mounted && !unmount_attempted) {
                unmount_attempted=true;
                if(!unmount(image.h,0)) {
                    if(wim_detail::inspect_private_mount(
                           dll,folder,tx.workspace)!=
                       wim_detail::PrivateMountState::Unmounted)
                        tx.preserve();
                    throw std::runtime_error(
                        "WIM mounted workspace could not be discarded; "
                        "do NOT delete or rename its image before recovery: "+
                        tx.workspace.string());
                }
            }
            // A successful unmount can still leave a native registration or
            // reparse point; never let EditTransaction recursively delete it.
            if(wim_detail::inspect_private_mount(dll,folder,tx.workspace)!=
               wim_detail::PrivateMountState::Unmounted) {
                tx.preserve();
                throw std::runtime_error(
                    "WIM mount remains registered after cancel; preserve "+
                    tx.workspace.string());
            }
            throw;
        }
    }
    // WIMGAPI handles are closed here. Recheck its OWN native mount registry:
    // DISM may return an empty list even for orphaned WIMGAPI mount points.
    // Only remove an EMPTY mount directory; never recursively unlink a
    // mounted Windows tree or invoke global WIMDeleteImageMounts.
    if(wim_detail::inspect_private_mount(dll,folder,tx.workspace)!=
       wim_detail::PrivateMountState::Unmounted) {
        tx.preserve();
        throw std::runtime_error(
            "WIM mount cleanup incomplete: preserved "+
            tx.workspace.string());
    }
    {
        std::error_code ec;
        const bool removed=std::filesystem::remove(folder,ec);
        if(!removed || ec) {
            tx.preserve();
            throw std::runtime_error(
                "WIM mount directory is not empty after unmount: preserve "+
                tx.workspace.string());
        }
    }
    // The mount and all WIMGAPI handles must be closed before independent
    // metadata enumeration and before attempting to replace the source WIM.
    report(L"Verify WIM folder changes",87);
    wim_detail::Header updated_header{};
    if(!wim_detail::read_header(tx.output,updated_header))
        throw EditUnsupported("fast WIM output has no valid header");
    wim_detail::validate(updated_header);
    if(wim_detail::u32(updated_header,44)!=
           wim_detail::u32(stage_header,44) ||
       wim_detail::u32(updated_header,120)!=
           wim_detail::u32(stage_header,120))
        throw EditUnsupported("fast WIM unexpectedly changed image count or boot index");
    {
        // Other indexes are preserved by the native in-place WIM commit.
        // The header check above ensures image count and boot index remain
        // intact; compare every entry and attribute in the modified index.
        const WimArchive updated(tx.output,plan.index);
        const auto newer=tree_snapshot(updated);
        std::string failure;
        if(!wim_detail::verify_mounted_folder_tree(original,newer,plan,failure))
            throw EditUnsupported("fast WIM change verification failed: "+failure);
    }
    report(L"Commit verified fast WIM",99);
    // Commit validates the original source and cancellation BEFORE releasing
    // the host's WIM reader handles. Failed or cancelled edits keep them live.
    tx.commit(progress,[&]{ WimArchive::release_open_readers(path); });
}
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
    if(deleted.size()==images)throw EditUnsupported("cannot delete the last WIM image");
    // A mixed request must not silently discard edits to an index that is
    // removed in the same transaction. Fail before cloning/applying the WIM.
    for(const unsigned index:deleted)
        if(edits.contains(index))
            throw EditUnsupported("cannot edit a WIM image index scheduled for deletion");

    // Keep a live Total Commander WIM panel readable during preflight and
    // expensive Apply/Capture. Release readers only after cancellation checks
    // at the final atomic replacement boundary in EditTransaction::commit.

    // F8 often selects a stale entry after an earlier edit, or Total Commander
    // passes a folder wildcard that names an absent directory. Check the
    // SOURCE metadata BEFORE WIMApplyImage/WIMCaptureImage or creating a
    // transaction. A nonexistent item must not launch a multi-gigabyte
    // Apply/Capture merely to return an error after several minutes.
    //
    // Only DELETE needs this source scan. File additions and replacements
    // retain their previous fast path; a valid deletion is still verified
    // independently after capture. WIMGAPI may block inside WIMLoadImage,
    // so report the metadata stage without claiming a fabricated percentage.
    for(const auto& [index, changes_for_index]:edits) {
        std::unordered_set<std::string> pending;
        const auto prefix="image"+std::to_string(index)+"/";
        for(const auto& change:changes_for_index)
            if(change.remove)pending.insert(prefix+change.path);
        if(pending.empty())continue;

        const auto message=L"Check WIM delete targets (image "+
                           std::to_wstring(index)+L")";
        report(message.c_str(),2);
        ULONGLONG last_update=0;
        WimArchive source_listing(path,index,
            [&](std::uint32_t,std::size_t entries) {
                edit_tick(progress);
                if(!status)return;
                const ULONGLONG now=GetTickCount64();
                if(last_update!=0 && now-last_update<250u)return;
                last_update=now;
                const auto label=L"Check WIM delete targets: image "+
                                 std::to_wstring(index)+L", "+
                                 std::to_wstring(entries)+L" paths";
                if(!status(label.c_str(),2))throw EditCancelled();
            });
        for(const auto& item:source_listing.items()) {
            if(pending.erase(item.path) && pending.empty())break;
        }
        if(!pending.empty())
            throw EditUnsupported(
                "WIM entry to delete was not found in source metadata");
    }
    // Strict experimental opt-in until real machine WIM mount and crash
    // recovery have been validated. Normal users retain Apply/Capture.
    if(fast_mount_requested()) {
        if(const auto plan=wim_detail::plan_mounted_folders(edits,deleted)) {
            fast_mount_folder_edit(path,*plan,progress,status);
            return;
        }
    }
    report(L"Prepare NTFS staging",3);
    Privileges privileges;Apartment apartment;EditTransaction tx(path);Library library;auto m=library.h;
    // Native Windows capture/apply preserve security and alternate streams on NTFS.
    wchar_t volume[MAX_PATH]{},filesystem[MAX_PATH]{};checked(GetVolumePathNameW(tx.workspace.c_str(),volume,MAX_PATH),"find WIM workspace volume");checked(GetVolumeInformationW(volume,nullptr,0,nullptr,nullptr,nullptr,filesystem,MAX_PATH),"inspect WIM workspace filesystem");
    if(_wcsicmp(filesystem,L"NTFS")!=0)throw EditUnsupported("WIM editing requires an NTFS workspace volume");
    auto create=api<HANDLE(WINAPI*)(PCWSTR,DWORD,DWORD,DWORD,DWORD,PDWORD)>(m,"WIMCreateFile");auto close=api<Close>(m,"WIMCloseHandle");auto temp=api<BOOL(WINAPI*)(HANDLE,PCWSTR)>(m,"WIMSetTemporaryPath");auto load=api<HANDLE(WINAPI*)(HANDLE,DWORD)>(m,"WIMLoadImage");
    Callback callback{progress,status};
    report(L"Open WIM indexes",5);
    {
        Handle source(create(path.c_str(),GENERIC_READ,OPEN_EXISTING,2,0,nullptr),close);
        const DWORD flags=wim_detail::u32(header,16);const DWORD compression=(flags&0x40000)?2u:(flags&0x20000)?1u:0u;
        Handle output(create(tx.output.c_str(),GENERIC_READ|GENERIC_WRITE,CREATE_NEW,2,compression,nullptr),close);
        checked(temp(source.h,tx.workspace.c_str())&&temp(output.h,tx.workspace.c_str()),"set WIM temporary directory");Registered source_callback(m,source.h,callback),output_callback(m,output.h,callback);
        unsigned out_index=0,new_boot=0;
        for(unsigned index=1;index<=images;++index){
            callback.tick();
            const unsigned begin=6u+static_cast<unsigned>(75ull*(index-1u)/images);
            const unsigned end=6u+static_cast<unsigned>(75ull*index/images);
            const std::wstring number=std::to_wstring(index)+L"/"+std::to_wstring(images);
            if(deleted.count(index)){
                callback.begin_stage(L"Skipping removed WIM image "+number,begin,0);
                continue;
            }
            ++out_index;if(boot==index)new_boot=out_index;
            Handle image(load(source.h,index),close);
            if(!edits.count(index)){
                callback.begin_stage(L"Export unchanged WIM image "+number,begin,end-begin);
                checked(api<BOOL(WINAPI*)(HANDLE,HANDLE,DWORD)>(m,"WIMExportImage")(image.h,output.h,1),"export unchanged WIM index");
                callback.tick();
                continue;
            }
            // Each edited index gets its OWN apply tree. If Windows keeps
            // one tree locked, later indexes must never see stale content.
            const auto expanded=tx.workspace/
                (edits.size()==1 ? L"expanded" :
                 (L"expanded-image-"+std::to_wstring(index)));
            std::filesystem::create_directory(expanded);
            struct Cleanup {
                std::filesystem::path root,path;
                const WimEditStatus& status;
                unsigned percent;
                ~Cleanup() noexcept {
                    // WIMGAPI Capture has returned; all image data and ACLs
                    // are already recorded in the output WIM. The temporary
                    // Windows tree can now be removed using SeRestorePrivilege
                    // without changing the captured image's NTFS security.
                    try {
                        if(status) (void)status(L"Cleanup expanded WIM staging",
                                                percent);
                        const auto result=
                            workspace_cleanup::cleanup_expanded(root,path);
                        if(!result.clean) {
                            OutputDebugStringW((
                                L"ISO-WCX: deferred WIM expanded cleanup: "+
                                path.wstring()+L" blocked="+
                                result.blocked.wstring()+L" error="+
                                std::to_wstring(result.error)+L"\n").c_str());
                        }
                    }catch(...) {}
                    // On an incomplete cleanup EditTransaction will retry
                    // the entire private workspace after WIMGAPI handles
                    // are released (and again on WCX CloseArchive).
                }
            }cleanup{tx.workspace,expanded,status,
                      begin+(end-begin)*94u/100u};
            const unsigned apply_end=begin+(end-begin)*55u/100u;
            callback.begin_stage(L"Extract WIM image "+number,begin,apply_end-begin);
            const BOOL applied=api<BOOL(WINAPI*)(HANDLE,PCWSTR,DWORD)>(m,"WIMApplyImage")(image.h,expanded.c_str(),2u|0x100u);
            callback.tick();checked(applied,"apply WIM index");
            callback.begin_stage(L"Edit WIM image "+number,apply_end,0);
            for(const auto& c:edits[index]){
                callback.tick();
                auto relative=relative_path(c.path);
                no_reparse(expanded,relative,c.remove);
                auto target=expanded/relative;
                if(c.remove) {
                    // This private NTFS removal is intentionally recursive for selected
                    // cleanup, but a user F8/Delete request must NOT succeed
                    // if the selected WIM entry was already absent. Otherwise
                    // we recapture a large install.wim and report a fake
                    // successful deletion of nothing.
                    const DWORD attr=GetFileAttributesW(target.c_str());
                    if(attr==INVALID_FILE_ATTRIBUTES) {
                        const DWORD error=GetLastError();
                        if(error==ERROR_FILE_NOT_FOUND ||
                           error==ERROR_PATH_NOT_FOUND)
                            throw EditUnsupported("WIM entry to delete was not found");
                    }
                    remove_staged_wim_entry(tx.workspace,expanded,target,
                                            callback,status,apply_end);
                    continue;
                }
                if(c.directory){std::filesystem::create_directories(target);continue;}
                checked(std::filesystem::is_regular_file(std::filesystem::symlink_status(c.source)),"source must be a regular file");
                std::filesystem::create_directories(target.parent_path());
                if(std::filesystem::exists(target)){
                    if(std::filesystem::is_directory(target))throw EditUnsupported("cannot replace a directory with a file");
                    remove_staged_wim_entry(tx.workspace,expanded,target,
                                            callback,status,apply_end);
                }
                // WIMApplyImage restores Windows' protected NTFS DACLs.
                // A normal F5 CopyFileW may fail with ACCESS_DENIED even
                // when F7/F8 work. Only the private expanded parent may
                // receive a temporary create-file ACE; restore its original
                // DACL before WIMCaptureImage records security descriptors.
                const auto copied=workspace_cleanup::copy_into_expanded(
                    tx.workspace,expanded,c.source,target);
                if(!copied.clean) {
                    const auto detail="WIM F5 file copy failed (Win32 "+
                        std::to_string(copied.error)+
                        ") while writing "+c.path;
                    if(copied.unsafe)
                        throw EditUnsupported(detail+
                            ": unsafe path, mount, or parent NTFS ACL could not be restored");
                    throw std::runtime_error(detail);
                }
            }
            callback.begin_stage(L"Rebuild WIM image "+number,apply_end,end-apply_end);
            HANDLE captured=api<HANDLE(WINAPI*)(HANDLE,PCWSTR,DWORD)>(m,"WIMCaptureImage")(output.h,expanded.c_str(),2u|0x100u);
            callback.tick();Handle result(captured,close);preserve_metadata(m,image.h,result.h);
        }
        checked(api<BOOL(WINAPI*)(HANDLE,DWORD)>(m,"WIMSetBootImage")(output.h,new_boot),"preserve WIM boot index");
    }
    // WIMExportImage already validated every unchanged index. The legacy
    // verifier re-enumerated ALL trees after Apply/Capture (including hundreds
    // of thousands of files in unrelated Windows editions) even though it
    // only checked requested changes. Verify the header's index/boot mapping
    // once, and enumerate/read back only indexes that were actually changed.
    // This preserves full edited-file content comparison and cancellation.
    report(L"Verify WIM structure",85);
    wim_detail::Header result_header{};
    if(!wim_detail::read_header(tx.output,result_header))
        throw EditUnsupported("rebuilt WIM header is missing");
    wim_detail::validate(result_header);
    const unsigned expected_images=images-static_cast<unsigned>(deleted.size());
    unsigned expected_boot=0;
    if(boot!=0 && !deleted.contains(boot)) {
        expected_boot=boot;
        for(const auto removed_index:deleted)
            if(removed_index<boot) --expected_boot;
    }
    if(wim_detail::u32(result_header,44)!=expected_images ||
       wim_detail::u32(result_header,120)!=expected_boot)
        throw EditUnsupported("rebuilt WIM image/boot index mapping changed");

    unsigned output_index=0;
    for(unsigned index=1;index<=images;++index) {
        if(deleted.contains(index))continue;
        ++output_index;
        const auto at=edits.find(index);
        if(at==edits.end())continue;
        // WIMGAPI can enumerate one chosen index without loading other
        // editions. The original full-image scan was proportional to the
        // total combined file count, not the number of requested changes.
        WimArchive verified(tx.output,output_index);
        std::unordered_map<std::string,std::size_t> found;
        const auto& items=verified.items();
        found.reserve(items.size());
        for(std::size_t i=0;i<items.size();++i) {
            if((i&1023u)==0u)callback.tick();
            if(!found.emplace(items[i].path,i).second)
                throw EditUnsupported("duplicate WIM output path");
        }
        const auto prefix="image"+std::to_string(output_index);
        if(!found.contains(prefix))
            throw EditUnsupported("missing rebuilt WIM image index");
        for(const auto& change:at->second) {
            callback.tick();
            const auto key=prefix+"/"+change.path;
            const auto entry=found.find(key);
            if(change.remove) {
                if(entry!=found.end())
                    throw EditUnsupported("removed WIM entry still exists");
                continue;
            }
            if(entry==found.end())
                throw EditUnsupported("new WIM entry is missing");
            const auto& item=items[entry->second];
            if(item.directory!=change.directory)
                throw EditUnsupported("WIM edited entry has wrong type");
            if(change.directory)continue;
            const auto length=std::filesystem::file_size(change.source);
            if(item.size!=length)
                throw EditUnsupported("WIM edited file size differs");
            std::ifstream input(change.source,std::ios::binary);
            if(!input)
                throw EditUnsupported("cannot verify WIM added file source");
            bool mismatch=false;
            std::array<char,64*1024> buffer{};
            const bool read=verified.read(entry->second,
                [&](const std::uint8_t* data,std::size_t bytes) {
                    callback.tick();
                    std::size_t offset=0;
                    while(offset<bytes) {
                        const auto n=std::min(buffer.size(),bytes-offset);
                        input.read(buffer.data(),static_cast<std::streamsize>(n));
                        if(input.gcount()!=static_cast<std::streamsize>(n) ||
                           std::memcmp(buffer.data(),data+offset,n)!=0) {
                            mismatch=true;
                            return false;
                        }
                        offset+=n;
                    }
                    return true;
                });
            if(!read || mismatch ||
               input.peek()!=std::char_traits<char>::eof())
                throw EditUnsupported("WIM edited file readback mismatch");
        }
    }
    report(L"Commit verified WIM",99);
    tx.commit(progress,[&]{ WimArchive::release_open_readers(path); });
    // No cancellation or throwing callbacks after the atomic commit.
}
}
#endif
