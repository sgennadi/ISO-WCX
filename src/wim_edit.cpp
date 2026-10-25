#ifdef _WIN32
#include "iso_wcx/archive_edit.hpp"
#include "iso_wcx/edit_transaction.hpp"
#include "iso_wcx/wim_archive.hpp"
#include "iso_wcx/wim_parts.hpp"
#include <msxml6.h>
#include <map>
#include <mutex>
#include <set>
#include <atomic>
#include <cstring>
#include <algorithm>
#include <fstream>

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
struct Callback {
    const EditProgress& progress;std::mutex mutex;std::atomic<bool> cancelled{false};
    static DWORD CALLBACK call(DWORD message,WPARAM,LPARAM,void* context) noexcept {
        auto& self=*static_cast<Callback*>(context);
        // PROCESS is the documented cancellation point. Never throw across ABI.
        if(message==0x8000+0x1476+3){try{std::lock_guard lock(self.mutex);if(self.progress&&!self.progress())self.cancelled=true;}catch(...){self.cancelled=true;}if(self.cancelled)return 0xffffffffu;}
        return 0;
    }
    void tick(){if(cancelled)throw EditCancelled();edit_tick(progress);}
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
void no_reparse(const std::filesystem::path& root,const std::filesystem::path& relative){
    auto path=root;for(const auto& part:relative){path/=part;DWORD attr=GetFileAttributesW(path.c_str());if(attr==INVALID_FILE_ATTRIBUTES){auto err=GetLastError();checked(err==ERROR_FILE_NOT_FOUND||err==ERROR_PATH_NOT_FOUND,"cannot inspect WIM edit path");continue;}if(attr&FILE_ATTRIBUTE_REPARSE_POINT)throw EditUnsupported("editing through WIM reparse points is not supported");}
}
void remove_tree(const std::filesystem::path& path){
    DWORD attr=GetFileAttributesW(path.c_str());if(attr==INVALID_FILE_ATTRIBUTES)return;
    if((attr&FILE_ATTRIBUTE_DIRECTORY)&&!(attr&FILE_ATTRIBUTE_REPARSE_POINT))for(const auto& item:std::filesystem::directory_iterator(path))remove_tree(item.path());
    checked(SetFileAttributesW(path.c_str(),attr&~FILE_ATTRIBUTE_READONLY),"clear temporary read-only flag");
    checked((attr&FILE_ATTRIBUTE_DIRECTORY)?RemoveDirectoryW(path.c_str()):DeleteFileW(path.c_str()),"remove WIM entry");
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
    report(L"Prepare NTFS staging",3);
    Privileges privileges;Apartment apartment;EditTransaction tx(path);Library library;auto m=library.h;
    // Native Windows capture/apply preserve security and alternate streams on NTFS.
    wchar_t volume[MAX_PATH]{},filesystem[MAX_PATH]{};checked(GetVolumePathNameW(tx.workspace.c_str(),volume,MAX_PATH),"find WIM workspace volume");checked(GetVolumeInformationW(volume,nullptr,0,nullptr,nullptr,nullptr,filesystem,MAX_PATH),"inspect WIM workspace filesystem");
    if(_wcsicmp(filesystem,L"NTFS")!=0)throw EditUnsupported("WIM editing requires an NTFS workspace volume");
    auto create=api<HANDLE(WINAPI*)(PCWSTR,DWORD,DWORD,DWORD,DWORD,PDWORD)>(m,"WIMCreateFile");auto close=api<Close>(m,"WIMCloseHandle");auto temp=api<BOOL(WINAPI*)(HANDLE,PCWSTR)>(m,"WIMSetTemporaryPath");auto load=api<HANDLE(WINAPI*)(HANDLE,DWORD)>(m,"WIMLoadImage");
    Callback callback{progress,{},false};
    report(L"Open WIM indexes",5);
    {
        Handle source(create(path.c_str(),GENERIC_READ,OPEN_EXISTING,2,0,nullptr),close);
        const DWORD flags=wim_detail::u32(header,16);const DWORD compression=(flags&0x40000)?2u:(flags&0x20000)?1u:0u;
        Handle output(create(tx.output.c_str(),GENERIC_READ|GENERIC_WRITE,CREATE_NEW,2,compression,nullptr),close);
        checked(temp(source.h,tx.workspace.c_str())&&temp(output.h,tx.workspace.c_str()),"set WIM temporary directory");Registered source_callback(m,source.h,callback),output_callback(m,output.h,callback);
        unsigned out_index=0,new_boot=0;
        for(unsigned index=1;index<=images;++index){
            callback.tick();
            report(L"Process WIM index",static_cast<unsigned>(6u+(index-1u)*73u/images));
            if(deleted.count(index))continue;++out_index;if(boot==index)new_boot=out_index;Handle image(load(source.h,index),close);
            if(!edits.count(index)){checked(api<BOOL(WINAPI*)(HANDLE,HANDLE,DWORD)>(m,"WIMExportImage")(image.h,output.h,1),"export unchanged WIM index");continue;}
            const auto expanded=tx.workspace/L"expanded";std::filesystem::create_directory(expanded);
            struct Cleanup {std::filesystem::path path;~Cleanup(){try{remove_tree(path);}catch(...){}}}cleanup{expanded};
            report(L"Extract selected index",static_cast<unsigned>(8u+(index-1u)*73u/images));
            const BOOL applied=api<BOOL(WINAPI*)(HANDLE,PCWSTR,DWORD)>(m,"WIMApplyImage")(image.h,expanded.c_str(),2u|0x100u);callback.tick();checked(applied,"apply WIM index");
            report(L"Apply requested changes",static_cast<unsigned>(35u+(index-1u)*45u/images));
            for(const auto& c:edits[index]){callback.tick();auto relative=relative_path(c.path);no_reparse(expanded,relative);auto target=expanded/relative;
                if(c.remove){remove_tree(target);continue;}
                if(c.directory){std::filesystem::create_directories(target);continue;}
                checked(std::filesystem::is_regular_file(std::filesystem::symlink_status(c.source)),"source must be a regular file");
                std::filesystem::create_directories(target.parent_path());if(std::filesystem::exists(target)){if(std::filesystem::is_directory(target))throw EditUnsupported("cannot replace a directory with a file");remove_tree(target);}
                checked(CopyFileW(c.source.c_str(),target.c_str(),TRUE),"copy replacement into WIM workspace");
            }
            report(L"Rebuild changed WIM index",static_cast<unsigned>(48u+(index-1u)*33u/images));
            HANDLE captured=api<HANDLE(WINAPI*)(HANDLE,PCWSTR,DWORD)>(m,"WIMCaptureImage")(output.h,expanded.c_str(),2u|0x100u);callback.tick();Handle result(captured,close);preserve_metadata(m,image.h,result.h);
        }
        checked(api<BOOL(WINAPI*)(HANDLE,DWORD)>(m,"WIMSetBootImage")(output.h,new_boot),"preserve WIM boot index");
    }
    // Reopen the rebuilt image, validate all index trees, and independently
    // compare newly supplied streams. Do not re-decompress every unchanged
    // file: this made creating even an empty directory take minutes longer.
    report(L"Verify WIM structure",85);
    {
        WimArchive verified(tx.output);
        std::map<std::string,std::size_t> found;
        const auto& items=verified.items();
        for(std::size_t i=0;i<items.size();++i){
            callback.tick();
            if(!found.emplace(items[i].path,i).second)
                throw EditUnsupported("duplicate WIM output path");
        }
        unsigned new_index=0;
        for(unsigned index=1;index<=images;++index){
            if(deleted.count(index))continue;
            ++new_index;
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
    report(L"Commit verified WIM",99);
    tx.commit(progress);
    report(L"WIM edit complete",100);
}
}
#endif
