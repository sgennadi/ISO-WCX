// DISM /Get-WimInfo-style metadata; read-only and entirely in-process.
// Uses Windows WIMGAPI and MSXML, which ISO-WCX already depends on.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "iso_wcx/wim_index_report.hpp"
#include "iso_wcx/wim_parts.hpp"
#include <windows.h>
#include <msxml6.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace iso_wcx { namespace {
using CloseFn=BOOL (WINAPI*)(HANDLE);
using CreateFn=HANDLE (WINAPI*)(PCWSTR,DWORD,DWORD,DWORD,DWORD,PDWORD);
using CountFn=DWORD (WINAPI*)(HANDLE);
using LoadFn=HANDLE (WINAPI*)(HANDLE,DWORD);
using GetInfoFn=BOOL (WINAPI*)(HANDLE,void**,DWORD*);
using TempFn=BOOL (WINAPI*)(HANDLE,PCWSTR);
using ReferenceFn=BOOL (WINAPI*)(HANDLE,PCWSTR,DWORD);

void require(bool valid,const char* message) {
    if(!valid)throw std::runtime_error(message);
}
template<class F> F function(HMODULE module,const char* name) {
    const auto p=GetProcAddress(module,name);
    require(p!=nullptr,"required Windows imaging API export missing");
    return reinterpret_cast<F>(p);
}
struct Library {
    HMODULE module=LoadLibraryExW(L"wimgapi.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    Library(){require(module!=nullptr,"Windows WIMGAPI unavailable");}
    ~Library(){if(module)FreeLibrary(module);}
    Library(const Library&)=delete;
};
struct Handle {
    HANDLE handle=nullptr;
    CloseFn close=nullptr;
    Handle(HANDLE h,CloseFn closer):handle(h),close(closer) {
        require(handle && handle!=INVALID_HANDLE_VALUE,"cannot open WIM index");
    }
    ~Handle(){if(handle && handle!=INVALID_HANDLE_VALUE)close(handle);}
    Handle(const Handle&)=delete;
};
template<class T> struct Com {
    T* ptr=nullptr;
    ~Com(){if(ptr)ptr->Release();}
    T** out(){return &ptr;}
    T* operator->(){return ptr;}
};
struct Bstr {
    BSTR value=nullptr;
    explicit Bstr(const wchar_t* text):value(SysAllocString(text)) {
        if(!value)throw std::bad_alloc();
    }
    ~Bstr(){if(value)SysFreeString(value);}
};
struct Apartment {
    HRESULT code=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    Apartment(){require(SUCCEEDED(code)||code==RPC_E_CHANGED_MODE,"COM initialization failed");}
    ~Apartment(){if(SUCCEEDED(code))CoUninitialize();}
};
std::string utf8_safe(const std::wstring& text) {
    const auto limited=text.substr(0,512);
    if(limited.empty())return {};
    const int count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,limited.data(),
         static_cast<int>(limited.size()),nullptr,0,nullptr,nullptr);
    if(count<=0)return "[invalid Unicode]";
    std::string out(static_cast<std::size_t>(count),'\0');
    if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,limited.data(),
         static_cast<int>(limited.size()),out.data(),count,nullptr,nullptr))
        return "[invalid Unicode]";
    for(char& ch:out)
        if(ch=='\r'||ch=='\n'||ch=='\t'||ch=='\0')ch=' ';
    return out;
}
std::wstring xml_value(IXMLDOMDocument2* document,const wchar_t* path) {
    Bstr key(path);
    Com<IXMLDOMNode> node;
    if(FAILED(document->selectSingleNode(key.value,node.out()))||!node.ptr)
        return {};
    BSTR text=nullptr;
    if(FAILED(node->get_text(&text))||!text)return {};
    const std::wstring result(text,SysStringLen(text));
    SysFreeString(text);
    return result;
}
std::wstring xml_field(IXMLDOMDocument2* document,const wchar_t* property) {
    // An image handle commonly yields <IMAGE> but wrapped <WIM><IMAGE>
    // metadata also exists. Avoid external entities and DTDs.
    auto key=std::wstring(L"/*/")+property;
    auto result=xml_value(document,key.c_str());
    if(result.empty()){
        key=std::wstring(L"/*/IMAGE/")+property;
        result=xml_value(document,key.c_str());
    }
    return result;
}
void add(std::string& report,std::string_view label,const std::wstring& value) {
    if(value.empty())return;
    report+="  ";
    report+=label;
    report+=": ";
    report+=utf8_safe(value);
    report+='\n';
}
std::string arch_label(const std::wstring& value) {
    if(value==L"0")return "x86";
    if(value==L"9")return "x64";
    if(value==L"12")return "ARM64";
    if(value==L"5")return "ARM";
    return utf8_safe(value);
}
void append_xml(std::string& report,const wchar_t* text,std::size_t chars) {
    if(!text || !chars){report+="  Metadata: unavailable\n";return;}
    Com<IXMLDOMDocument2> document;
    if(FAILED(CoCreateInstance(__uuidof(DOMDocument60),nullptr,CLSCTX_INPROC_SERVER,
         __uuidof(IXMLDOMDocument2),reinterpret_cast<void**>(document.out()))) ||
         !document.ptr) {
        report+="  Metadata: XML parser unavailable\n";
        return;
    }
    document->put_async(VARIANT_FALSE);
    document->put_validateOnParse(VARIANT_FALSE);
    document->put_resolveExternals(VARIANT_FALSE);
    Bstr setting(L"ProhibitDTD");
    VARIANT flag{};
    flag.vt=VT_BOOL;
    flag.boolVal=VARIANT_TRUE;
    document->setProperty(setting.value,flag);
    BSTR input=SysAllocStringLen(text,static_cast<UINT>(chars));
    if(!input)throw std::bad_alloc();
    VARIANT_BOOL loaded=VARIANT_FALSE;
    const HRESULT result=document->loadXML(input,&loaded);
    SysFreeString(input);
    if(FAILED(result)||loaded!=VARIANT_TRUE){
        report+="  Metadata: invalid XML\n";
        return;
    }
    add(report,"Name",xml_field(document.ptr,L"NAME"));
    add(report,"Description",xml_field(document.ptr,L"DESCRIPTION"));
    add(report,"Edition",xml_field(document.ptr,L"WINDOWS/EDITIONID"));
    const auto architecture=xml_field(document.ptr,L"WINDOWS/ARCH");
    if(!architecture.empty())report+="  Architecture: "+arch_label(architecture)+"\n";
    const auto major=xml_field(document.ptr,L"WINDOWS/VERSION/MAJOR");
    const auto minor=xml_field(document.ptr,L"WINDOWS/VERSION/MINOR");
    const auto build=xml_field(document.ptr,L"WINDOWS/VERSION/BUILD");
    if(!major.empty()&&!minor.empty()&&!build.empty())
        report+="  Windows version: "+utf8_safe(major)+"."+
                utf8_safe(minor)+"."+utf8_safe(build)+"\n";
    add(report,"Flags",xml_field(document.ptr,L"FLAGS"));
    add(report,"Uncompressed bytes",xml_field(document.ptr,L"TOTALBYTES"));
}
void append_index_metadata(std::string& report,HMODULE module,HANDLE image) {
    void* info=nullptr;
    DWORD bytes=0;
    if(!function<GetInfoFn>(module,"WIMGetImageInformation")(image,&info,&bytes)) {
        report+="  Metadata: WIMGetImageInformation failed\n";
        return;
    }
    struct Release {void* data;~Release(){if(data)LocalFree(data);}} release{info};
    if(!info || bytes<2 || bytes>16u*1024u*1024u ||
       bytes%sizeof(wchar_t)!=0) {
        report+="  Metadata: invalid XML length\n";
        return;
    }
    const wchar_t* start=static_cast<const wchar_t*>(info);
    std::size_t chars=bytes/sizeof(wchar_t);
    if(chars && *start==0xfeff){++start;--chars;}
    while(chars && start[chars-1]==L'\0')--chars;
    append_xml(report,start,chars);
}
} // namespace

std::string build_wim_index_summary(const std::filesystem::path& image) {
    const auto parts=resolve_wim_parts(image);
    wim_detail::Header header{};
    require(wim_detail::read_header(parts.front(),header),"cannot read WIM header");
    wim_detail::validate(header);
    const auto boot=wim_detail::u32(header,120);
    Library library;
    const auto create=function<CreateFn>(library.module,"WIMCreateFile");
    const auto close=function<CloseFn>(library.module,"WIMCloseHandle");
    const auto load=function<LoadFn>(library.module,"WIMLoadImage");
    const auto count_images=function<CountFn>(library.module,"WIMGetImageCount");
    const auto set_temp=function<TempFn>(library.module,"WIMSetTemporaryPath");
    DWORD created=0;
    Handle source(create(parts.front().c_str(),GENERIC_READ,OPEN_EXISTING,0,0,&created),close);
    std::array<wchar_t,MAX_PATH+1> temporary{};
    const DWORD n=GetTempPathW(static_cast<DWORD>(temporary.size()),temporary.data());
    require(n>0&&n<temporary.size() && set_temp(source.handle,temporary.data()),
            "cannot set WIM temporary path");
    if(parts.size()>1){
        const auto reference=function<ReferenceFn>(library.module,"WIMSetReferenceFile");
        for(std::size_t i=1;i<parts.size();++i)
            require(reference(source.handle,parts[i].c_str(),0x00010000u),
                    "cannot reference SWM segment");
    }
    const DWORD count=count_images(source.handle);
    require(count>0&&count<=256,"invalid WIM index count");
    Apartment com;
    std::string report="WIM INDEX SUMMARY (WIMGAPI; read-only, not mounted)\n";
    report+="Image indexes: "+std::to_string(count)+"\n";
    report+="WIM parts: "+std::to_string(parts.size())+"\n";
    report+="Boot index: "+(boot?std::to_string(boot):std::string("not set"))+"\n";
    for(DWORD index=1;index<=count;++index){
        Handle selected(load(source.handle,index),close);
        report+="Index "+std::to_string(index);
        if(index==boot)report+=" [BOOT]";
        report+='\n';
        append_index_metadata(report,library.module,selected.handle);
    }
    report+="WIM editor: F5 targets imageN only; no DISM mount required.\n";
    return report;
}
} // namespace iso_wcx
#endif
