// ISO-WCX one-click installer. No runtime scripts, elevated permissions or DLL dependencies.
// Packed Win32/x64 WCX images are embedded as native RCDATA resources in CI.
// Only the WCX matching the TARGET Total Commander is deployed.
// Total Commander must be closed before modifying its configuration.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace fs=std::filesystem;
struct Options {
    fs::path tc, ini, plugin;
    int arch=0;
    bool dry=false,quiet=false;
};
struct Host {fs::path folder;int arch=0;};
struct Ini {
    fs::path path;
    std::string original;
    std::wstring text, eol;
    enum class Encoding { ansi, utf8, utf16le, utf16be } encoding=Encoding::ansi;
};
struct Change {Ini doc;std::wstring updated;fs::path backup;bool committed=false;};
struct Payload {const unsigned char* ptr=nullptr;std::size_t size=0;std::vector<unsigned char> owned;};
std::wstring lower(std::wstring x) {
    std::transform(x.begin(),x.end(),x.begin(),[](wchar_t c){return std::towlower(c);});
    return x;
}
std::wstring trim(std::wstring x) {
    const auto b=x.find_first_not_of(L" \t\r\n\"");
    if(b==std::wstring::npos)return {};
    const auto e=x.find_last_not_of(L" \t\r\n\"");
    return x.substr(b,e-b+1);
}
bool ends(const std::wstring& x,const wchar_t* suffix) {
    const std::wstring s(suffix);
    return x.size()>=s.size() && x.compare(x.size()-s.size(),s.size(),s)==0;
}
std::wstring env(const wchar_t* name) {
    DWORD n=GetEnvironmentVariableW(name,nullptr,0);
    if(!n || n>32768)return {};
    std::wstring x(n,L'\0');
    const DWORD got=GetEnvironmentVariableW(name,x.data(),n);
    if(!got || got>=n)return {};
    x.resize(got);return x;
}
std::wstring expand(const std::wstring& value) {
    DWORD size=ExpandEnvironmentStringsW(value.c_str(),nullptr,0);
    if(!size||size>32768)throw std::runtime_error("Invalid environment path");
    std::wstring x(size,L'\0');
    DWORD result=ExpandEnvironmentStringsW(value.c_str(),x.data(),size);
    if(!result || result>size)throw std::runtime_error("Cannot expand path");
    x.resize(result-1);
    return x;
}
bool file(const fs::path& p) {
    std::error_code ec;
    auto info=fs::symlink_status(p,ec);
    return !ec && fs::is_regular_file(info) && !fs::is_symlink(info);
}
void assert_plain_file(const fs::path& p) {
    if(!file(p))throw std::runtime_error("Missing regular file or disallowed symlink");
    auto attributes=GetFileAttributesW(p.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("Refusing reparse-point file");
}
std::string read_file(const fs::path& p) {
    assert_plain_file(p);
    if(fs::file_size(p)>16ull*1024*1024)
        throw std::runtime_error("INI exceeds 16 MiB limit");
    std::ifstream input(p,std::ios::binary);
    if(!input)throw std::runtime_error("Cannot read INI");
    return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}
void write_bytes(const fs::path& path,const void* data,std::size_t size) {
    std::ofstream stream(path,std::ios::binary|std::ios::trunc);
    if(!stream)throw std::runtime_error("Cannot stage file");
    stream.write(static_cast<const char*>(data),static_cast<std::streamsize>(size));
    stream.flush();
    if(!stream)throw std::runtime_error("File stage write failed");
}
std::wstring decode_multi(const char* s,int len,UINT cp,DWORD flags) {
    if(len==0)return {};
    int count=MultiByteToWideChar(cp,flags,s,len,nullptr,0);
    if(count<=0)throw std::runtime_error("INI text decoding failed");
    std::wstring out(static_cast<std::size_t>(count),L'\0');
    if(!MultiByteToWideChar(cp,flags,s,len,out.data(),count))
        throw std::runtime_error("INI text decoding failed");
    return out;
}
std::string encode_multi(const std::wstring& s,UINT cp,DWORD flags) {
    if(s.empty())return {};
    BOOL lossy=FALSE;
    BOOL* used=cp==CP_UTF8?nullptr:&lossy;
    int n=WideCharToMultiByte(cp,flags,s.data(),static_cast<int>(s.size()),
                             nullptr,0,nullptr,used);
    if(n<=0||lossy)throw std::runtime_error("INI path is not representable in its original codepage");
    std::string result(static_cast<std::size_t>(n),'\0');
    lossy=FALSE;
    if(!WideCharToMultiByte(cp,flags,s.data(),static_cast<int>(s.size()),
                           result.data(),n,nullptr,used) || lossy)
        throw std::runtime_error("INI encoding would lose characters");
    return result;
}
Ini load_ini(const fs::path& path) {
    Ini d;d.path=fs::absolute(path).lexically_normal();d.original=read_file(d.path);
    const auto& b=d.original;
    if(b.size()>=3 && static_cast<unsigned char>(b[0])==0xef &&
       static_cast<unsigned char>(b[1])==0xbb &&
       static_cast<unsigned char>(b[2])==0xbf) {
        d.encoding=Ini::Encoding::utf8;
        d.text=decode_multi(b.data()+3,static_cast<int>(b.size()-3),CP_UTF8,MB_ERR_INVALID_CHARS);
    } else if(b.size()>=2 && static_cast<unsigned char>(b[0])==0xff &&
              static_cast<unsigned char>(b[1])==0xfe) {
        d.encoding=Ini::Encoding::utf16le;
        if((b.size()-2)%2)throw std::runtime_error("Invalid UTF-16LE length");
        for(std::size_t i=2;i<b.size();i+=2)
            d.text.push_back(static_cast<wchar_t>(static_cast<unsigned char>(b[i])|
                       (static_cast<unsigned char>(b[i+1])<<8)));
    } else if(b.size()>=2 && static_cast<unsigned char>(b[0])==0xfe &&
              static_cast<unsigned char>(b[1])==0xff) {
        d.encoding=Ini::Encoding::utf16be;
        if((b.size()-2)%2)throw std::runtime_error("Invalid UTF-16BE length");
        for(std::size_t i=2;i<b.size();i+=2)
            d.text.push_back(static_cast<wchar_t>((static_cast<unsigned char>(b[i])<<8)|
                        static_cast<unsigned char>(b[i+1])));
    } else {
        d.text=decode_multi(b.data(),static_cast<int>(b.size()),CP_ACP,0);
    }
    if(d.text.find(L'\0')!=std::wstring::npos)
        throw std::runtime_error("Unexpected NUL inside INI");
    d.eol=d.text.find(L"\r\n")!=std::wstring::npos?L"\r\n":L"\n";
    return d;
}
std::string encode_ini(const Ini& doc,const std::wstring& text) {
    std::string bytes;
    switch(doc.encoding) {
    case Ini::Encoding::utf8:
        bytes.assign("\xef\xbb\xbf",3);
        bytes+=encode_multi(text,CP_UTF8,WC_ERR_INVALID_CHARS);
        break;
    case Ini::Encoding::utf16le:
    case Ini::Encoding::utf16be: {
        const bool little=doc.encoding==Ini::Encoding::utf16le;
        bytes.push_back(static_cast<char>(little?0xff:0xfe));
        bytes.push_back(static_cast<char>(little?0xfe:0xff));
        for(wchar_t w:text) {
            bytes.push_back(static_cast<char>(little?(w&0xff):(w>>8)));
            bytes.push_back(static_cast<char>(little?(w>>8):(w&0xff)));
        }
        break;
    }
    default: bytes=encode_multi(text,CP_ACP,0);break;
    }
    return bytes;
}
using Row=std::pair<std::wstring,std::wstring>;
struct Section {std::map<std::wstring,std::wstring> values;bool present=false;};
Section section(const std::wstring& text,const std::wstring& expected) {
    Section res;bool active=false;
    std::size_t pos=0;
    while(pos<text.size()) {
        auto end=text.find(L'\n',pos);
        if(end==std::wstring::npos)end=text.size();
        std::wstring line=text.substr(pos,end-pos);
        if(!line.empty()&&line.back()==L'\r')line.pop_back();
        auto stripped=trim(line);
        if(stripped.size()>=2&&stripped.front()==L'['&&stripped.back()==L']') {
            active=lower(trim(stripped.substr(1,stripped.size()-2)))==lower(expected);
            if(active&&res.present)throw std::runtime_error("Duplicate INI section");
            if(active)res.present=true;
        } else if(active) {
            const auto equal=line.find(L'=');
            if(equal!=std::wstring::npos) {
                const auto key=lower(trim(line.substr(0,equal)));
                if(!key.empty()&&key[0]!=L';'&&key[0]!=L'#') {
                    if(res.values.count(key))throw std::runtime_error("Duplicate INI setting");
                    res.values[key]=trim(line.substr(equal+1));
                }
            }
        }
        pos=end==text.size()?end:end+1;
    }
    return res;
}
std::wstring lookup(const Ini& ini,const wchar_t* section_name,const wchar_t* key) {
    auto v=section(ini.text,section_name);
    auto it=v.values.find(lower(key));
    return it==v.values.end()?L"":it->second;
}
fs::path redirect(const Ini& root,const wchar_t* section_name) {
    auto value=lookup(root,section_name,L"RedirectSection");
    if(value.empty()||value==L"0")return root.path;
    if(value==L"1") {
        value=lookup(root,L"Configuration",L"AlternateUserIni");
        if(value.empty())throw std::runtime_error("RedirectSection=1 without AlternateUserIni");
    }
    value=expand(value);
    fs::path p(value);
    if(p.is_relative())p=root.path.parent_path()/p;
    p=p.lexically_normal();
    assert_plain_file(p);
    return p;
}
bool same(const fs::path& a,const fs::path& b) {
    return lower(fs::absolute(a).lexically_normal().wstring())==
           lower(fs::absolute(b).lexically_normal().wstring());
}
std::wstring update_packer(const Ini& in,const fs::path& target,unsigned& changed,unsigned& skipped) {
    constexpr std::array<const wchar_t*,23> formats{
        L"iso",L"udf",L"cue",L"ccd",L"nrg",L"mds",L"mdx",L"cso",
        L"zso",L"isz",L"daa",L"img",L"raw",L"vhd",L"vhdx",L"wim",
        L"esd",L"swm",L"ima",L"vfd",L"flp",L"bif",L"imz"};
    const auto pack=section(in.text,L"PackerPlugins");
    const auto new_value=L"158,"+target.wstring();
    std::map<std::wstring,std::wstring> replacements;
    std::vector<std::wstring> additions;
    for(const wchar_t* ext:formats) {
        auto it=pack.values.find(ext);
        if(it==pack.values.end()) {
            additions.push_back(std::wstring(ext)+L"="+new_value);++changed;
            continue;
        }
        const auto raw=it->second;
        const auto comma=raw.find(L',');
        const auto filename=comma==std::wstring::npos?L"":trim(raw.substr(comma+1));
        auto basename=lower(fs::path(filename).filename().wstring());
        // Only migrate our own current or old ISO-WCX handler.
        // Never take ownership of an unrelated third-party association.
        const auto normalized=lower(filename);
        bool ours=(basename==L"iso-wcx.wcx"||basename==L"iso-wcx.wcx64") &&
                  (normalized.find(L"iso-wcx")!=std::wstring::npos);
        // The old ISO.WCX handler can be migrated only if its file is gone.
        if(std::wstring(ext)==L"iso"&&basename==L"iso.wcx"&&!file(filename))
            ours=true;
        if(!ours){++skipped;continue;}
        if(raw!=new_value) {replacements[ext]=new_value;++changed;}
    }
    if(!changed)return in.text;
    std::wstring result;result.reserve(in.text.size()+additions.size()*128);
    std::size_t start=0;bool active=false,found=false;
    while(start<in.text.size()) {
        auto end=in.text.find(L'\n',start);
        if(end==std::wstring::npos)end=in.text.size();
        auto line=in.text.substr(start,end-start);
        std::wstring core=line;
        if(!core.empty()&&core.back()==L'\r')core.pop_back();
        auto t=trim(core);
        if(t.size()>=2&&t.front()==L'['&&t.back()==L']') {
            active=lower(trim(t.substr(1,t.size()-2)))==L"packerplugins";
            if(active) {
                found=true;
                result+=line;
                if(end<in.text.size())result+=L'\n';
                else result+=in.eol;
                for(const auto& entry:additions)result+=entry+in.eol;
                start=end==in.text.size()?end:end+1;
                continue;
            }
        } else if(active) {
            auto n=core.find(L'=');
            if(n!=std::wstring::npos) {
                auto k=lower(trim(core.substr(0,n)));
                if(auto it=replacements.find(k);it!=replacements.end()) {
                    core=core.substr(0,n+1)+it->second;
                    if(ends(line,L"\r"))core+=L'\r';
                    line=core;
                }
            }
        }
        result+=line;
        if(end<in.text.size())result+=L'\n';
        start=end==in.text.size()?end:end+1;
    }
    if(!found) {
        if(!result.empty()&&result.back()!=L'\n')result+=in.eol;
        result+=L"[PackerPlugins]"+in.eol;
        for(const auto& entry:additions)result+=entry+in.eol;
    }
    return result;
}
std::wstring remove_checksum(const Ini& in,const std::wstring& text) {
    std::wstring output;output.reserve(text.size());
    bool active=false;
    std::size_t start=0;
    while(start<text.size()) {
        auto end=text.find(L'\n',start);
        if(end==std::wstring::npos)end=text.size();
        auto line=text.substr(start,end-start);
        auto stripped=trim(line);
        if(stripped.size()>=2&&stripped.front()==L'['&&stripped.back()==L']')
            active=lower(trim(stripped.substr(1,stripped.size()-2)))==L"packerplugins64";
        bool skip=false;
        if(active) {
            auto equal=line.find(L'=');
            if(equal!=std::wstring::npos && lower(trim(line.substr(0,equal)))==L"$checksum$")
                skip=true;
        }
        if(!skip) {
            output+=line;
            if(end<text.size())output+=L'\n';
        }
        start=end==text.size()?end:end+1;
    }
    (void)in;return output;
}
fs::path unique_path(const fs::path& path,const wchar_t* marker) {
    SYSTEMTIME tm{};GetLocalTime(&tm);
    wchar_t time[64]{};
    swprintf_s(time,L"%04u%02u%02u-%02u%02u%02u-%lu",tm.wYear,tm.wMonth,
               tm.wDay,tm.wHour,tm.wMinute,tm.wSecond,GetCurrentProcessId());
    for(int i=0;i<100;++i) {
        fs::path result(path.wstring()+marker+time+L"-"+std::to_wstring(i));
        if(!fs::exists(result))return result;
    }
    throw std::runtime_error("Cannot create unique transaction file");
}
std::wstring reg(HKEY root,const wchar_t* value) {
    DWORD size=0;
    const wchar_t* key=L"Software\\Ghisler\\Total Commander";
    if(RegGetValueW(root,key,value,RRF_RT_REG_SZ|RRF_RT_REG_EXPAND_SZ,nullptr,nullptr,&size)!=ERROR_SUCCESS||
       size<2||size>32768*sizeof(wchar_t))return {};
    std::wstring out(size/sizeof(wchar_t),L'\0');
    if(RegGetValueW(root,key,value,RRF_RT_REG_SZ|RRF_RT_REG_EXPAND_SZ,
                    nullptr,out.data(),&size)!=ERROR_SUCCESS)return {};
    out.resize(std::wcslen(out.c_str()));
    return expand(out);
}
std::optional<Host> running_parent_tc() {
    DWORD pid=GetCurrentProcessId();
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snapshot==INVALID_HANDLE_VALUE)return {};
    DWORD parent=0;
    PROCESSENTRY32W process{};process.dwSize=sizeof(process);
    if(Process32FirstW(snapshot,&process))do {
        if(process.th32ProcessID==pid){parent=process.th32ParentProcessID;break;}
    }while(Process32NextW(snapshot,&process));
    CloseHandle(snapshot);
    if(!parent)return {};
    HANDLE h=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,parent);
    if(!h)return {};
    std::wstring path(32768,L'\0');DWORD length=static_cast<DWORD>(path.size());
    BOOL ok=QueryFullProcessImageNameW(h,0,path.data(),&length);
    CloseHandle(h);
    if(!ok)return {};
    path.resize(length);
    auto name=lower(fs::path(path).filename().wstring());
    if(name==L"totalcmd64.exe")return Host{fs::path(path).parent_path(),64};
    if(name==L"totalcmd.exe")return Host{fs::path(path).parent_path(),32};
    return {};
}
bool tc_process_running() {
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snapshot==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot enumerate running TC processes");
    PROCESSENTRY32W process{};process.dwSize=sizeof(process);
    bool found=false;
    if(Process32FirstW(snapshot,&process))do {
        auto name=lower(process.szExeFile);
        if(name==L"totalcmd.exe"||name==L"totalcmd64.exe") {found=true;break;}
    }while(Process32NextW(snapshot,&process));
    CloseHandle(snapshot);return found;
}
Host detect_host(const Options& opts,HWND parent) {
    auto started=running_parent_tc();
    fs::path folder=opts.tc;
    if(folder.empty()&&started)folder=started->folder;
    if(folder.empty()) {
        auto value=env(L"COMMANDER_PATH");
        if(!value.empty())folder=value;
    }
    if(folder.empty()) {
        auto value=reg(HKEY_CURRENT_USER,L"InstallDir");
        if(value.empty())value=reg(HKEY_LOCAL_MACHINE,L"InstallDir");
        if(!value.empty())folder=value;
    }
    if(folder.empty())throw std::runtime_error("Total Commander directory not detected. Supply --tc-dir.");
    folder=fs::absolute(folder).lexically_normal();
    const bool x86=file(folder/L"TOTALCMD.EXE");
    const bool x64=file(folder/L"TOTALCMD64.EXE");
    if(!x86&&!x64)throw std::runtime_error("Target is not a Total Commander installation");
    int arch=opts.arch;
    if(!arch&&started&&same(started->folder,folder))
        arch=started->arch;
    if(!arch) {
        if(x64&&!x86)arch=64;
        else if(x86&&!x64)arch=32;
    }
    if(!arch) {
        if(opts.quiet)throw std::runtime_error("Ambiguous TC architecture: specify --arch 32 or --arch 64");
        int answer=MessageBoxW(parent,
            L"Both 32-bit and 64-bit Total Commander are present.\n\n"
            L"YES = install for 64-bit TC\nNO = install for 32-bit TC\nCANCEL = do nothing",
            L"ISO-WCX: Select Total Commander",MB_YESNOCANCEL|MB_ICONQUESTION);
        if(answer==IDCANCEL)throw std::runtime_error("Cancelled before installation");
        arch=answer==IDYES?64:32;
    }
    if((arch==32&&!x86)||(arch==64&&!x64))
        throw std::runtime_error("Chosen TC architecture not installed in target folder");
    return {folder,arch};
}
fs::path find_ini(const Options& options,const Host& host) {
    if(!options.ini.empty())return fs::absolute(options.ini);
    auto from_env=env(L"COMMANDER_INI");
    if(!from_env.empty()&&file(from_env))return fs::absolute(expand(from_env));
    auto in_program=host.folder/L"wincmd.ini";
    if(file(in_program)) {
        auto doc=load_ini(in_program);
        if(lookup(doc,L"Configuration",L"UseIniInProgramDir")==L"7")return in_program;
    }
    auto from_reg=reg(HKEY_CURRENT_USER,L"IniFileName");
    if(!from_reg.empty()&&file(from_reg))return fs::absolute(from_reg);
    auto app=env(L"APPDATA");
    if(!app.empty()) {
        auto path=fs::path(app)/L"Ghisler"/L"wincmd.ini";
        if(file(path))return path;
    }
    throw std::runtime_error("Cannot find active wincmd.ini. Specify --ini.");
}
bool valid_pe(const unsigned char* buf,std::size_t length,unsigned machine) {
    if(!buf||length<128||buf[0]!='M'||buf[1]!='Z')return false;
    const auto pos=static_cast<std::size_t>(buf[60])|
                  (static_cast<std::size_t>(buf[61])<<8)|
                  (static_cast<std::size_t>(buf[62])<<16)|
                  (static_cast<std::size_t>(buf[63])<<24);
    if(pos>length||length-pos<6)return false;
    return buf[pos]=='P'&&buf[pos+1]=='E'&&buf[pos+2]==0&&buf[pos+3]==0&&
          ((static_cast<unsigned>(buf[pos+4]))|(static_cast<unsigned>(buf[pos+5])<<8))==machine;
}
Payload plugin_payload(const fs::path& installer,int arch) {
    Payload out;
    HRSRC resource=FindResourceW(nullptr,MAKEINTRESOURCEW(arch==64?102:101),RT_RCDATA);
    if(resource) {
        HGLOBAL obj=LoadResource(nullptr,resource);
        if(!obj)throw std::runtime_error("Embedded WCX resource failed to load");
        out.ptr=static_cast<const unsigned char*>(LockResource(obj));
        out.size=SizeofResource(nullptr,resource);
    } else {
        // Fallback for source-tree unit tests, NOT a runtime dependency
        // of the packaged self-contained installer.
        const auto filename=arch==64?L"ISO-WCX.wcx64":L"ISO-WCX.wcx";
        auto sidecar=installer.parent_path()/filename;
        assert_plain_file(sidecar);
        std::ifstream input(sidecar,std::ios::binary);
        out.owned={std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
        out.ptr=out.owned.data();out.size=out.owned.size();
    }
    if(!valid_pe(out.ptr,out.size,arch==64?0x8664:0x014c))
        throw std::runtime_error("Embedded WCX has invalid PE architecture");
    return out;
}
void atomically_replace(const fs::path& stage,const fs::path& dest) {
    if(file(dest)) {
        if(!ReplaceFileW(dest.c_str(),stage.c_str(),nullptr,REPLACEFILE_IGNORE_MERGE_ERRORS,
                         nullptr,nullptr))
            throw std::runtime_error("Could not atomically replace installed file");
    } else if(!MoveFileExW(stage.c_str(),dest.c_str(),MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("Could not move staged file");
    }
}
std::wstring install(const Options& o,HWND hwnd) {
    const auto host=detect_host(o,hwnd);
    auto ini_path=find_ini(o,host);
    auto root=load_ini(ini_path);
    auto assoc_path=redirect(root,L"PackerPlugins");
    auto cache_path=redirect(root,L"PackerPlugins64");
    auto assoc=same(root.path,assoc_path)?root:load_ini(assoc_path);
    auto cache=same(assoc.path,cache_path)?assoc:(same(root.path,cache_path)?root:load_ini(cache_path));
    auto plugin_dir=o.plugin.empty()
        ? host.folder/L"plugins"/L"wcx"/L"ISO-WCX"
        : fs::absolute(o.plugin);
    auto target=plugin_dir/(host.arch==64?L"ISO-WCX.wcx64":L"ISO-WCX.wcx");
    fs::path self;
    {
        std::wstring buffer(32768,L'\0');
        DWORD n=GetModuleFileNameW(nullptr,buffer.data(),static_cast<DWORD>(buffer.size()));
        if(!n||n>=buffer.size())throw std::runtime_error("Cannot locate installer");
        buffer.resize(n);self=buffer;
    }
    auto payload=plugin_payload(self,host.arch);
    unsigned changed=0,skipped=0;
    auto updated_assoc=update_packer(assoc,target,changed,skipped);
    auto updated_cache=same(cache.path,assoc.path)
        ? remove_checksum(cache,updated_assoc)
        : remove_checksum(cache,cache.text);
    if(same(cache.path,assoc.path))updated_assoc=updated_cache;

    std::vector<Change> changes;
    if(updated_assoc!=assoc.text)changes.push_back({assoc,updated_assoc,{ },false});
    if(!same(cache.path,assoc.path)&&updated_cache!=cache.text)
        changes.push_back({cache,updated_cache,{},false});

    std::wstring report=L"Total Commander: "+host.folder.wstring()+
        L"\nArchitecture: "+std::to_wstring(host.arch)+L"-bit"+
        L"\nSelected module: "+target.filename().wstring()+
        L"\nImage registrations: "+std::to_wstring(changed)+
        L"\nPreserved third-party associations: "+std::to_wstring(skipped)+
        L"\nINI: "+root.path.wstring();

    if(o.dry)return L"DRY RUN (no files changed)\n"+report;
    if(tc_process_running()) {
        if(o.quiet)throw std::runtime_error("Close Total Commander before installing");
        for(;;) {
            const int answer=MessageBoxW(hwnd,
                L"Close ALL Total Commander windows before completing installation.\n"
                L"Then select Retry. Cancel will leave everything unchanged.",
                L"ISO-WCX: Close Total Commander",
                MB_RETRYCANCEL|MB_ICONINFORMATION);
            if(answer!=IDRETRY)throw std::runtime_error("Cancelled before installation");
            if(!tc_process_running())break;
        }
    }
    // Existing files must be normal, not symlink/reparse targets.
    if(fs::exists(target))assert_plain_file(target);
    for(const auto& c:changes) {
        if(read_file(c.doc.path)!=c.doc.original)
            throw std::runtime_error("INI changed during preparation");
    }
    fs::create_directories(plugin_dir);
    fs::path dll_stage=unique_path(target,L".stage.");
    fs::path dll_backup;
    bool binary_committed=false;
    std::vector<fs::path> ini_stages;
    try {
        write_bytes(dll_stage,payload.ptr,payload.size);
        for(auto& c:changes) {
            auto stage=unique_path(c.doc.path,L".stage.");
            auto bytes=encode_ini(c.doc,c.updated);
            write_bytes(stage,bytes.data(),bytes.size());
            ini_stages.push_back(stage);
        }
        if(file(target)) {
            dll_backup=unique_path(target,L".isowcx-backup.");
            fs::copy_file(target,dll_backup,fs::copy_options::none);
        }
        atomically_replace(dll_stage,target);
        binary_committed=true;
        for(std::size_t i=0;i<changes.size();++i) {
            auto& c=changes[i];
            if(read_file(c.doc.path)!=c.doc.original)
                throw std::runtime_error("INI changed before commit");
            c.backup=unique_path(c.doc.path,L".isowcx-backup.");
            fs::copy_file(c.doc.path,c.backup,fs::copy_options::none);
            atomically_replace(ini_stages[i],c.doc.path);
            c.committed=true;
        }
    } catch(...) {
        for(auto it=changes.rbegin();it!=changes.rend();++it) {
            if(it->committed&&!it->backup.empty()) {
                std::error_code ec;fs::copy_file(it->backup,it->doc.path,
                                    fs::copy_options::overwrite_existing,ec);
            }
        }
        if(binary_committed) {
            std::error_code ec;
            if(!dll_backup.empty())fs::copy_file(dll_backup,target,
                                fs::copy_options::overwrite_existing,ec);
            else fs::remove(target,ec);
        }
        std::error_code ec;fs::remove(dll_stage,ec);
        for(const auto& s:ini_stages)fs::remove(s,ec);
        throw;
    }
    report+=L"\nComplete: only the selected WCX was installed.\nZIP/RAR/7z, buttons and user commands were preserved.";
    return report;
}
Options parse(int count,wchar_t** argv) {
    Options out;
    for(int i=1;i<count;++i) {
        auto arg=lower(argv[i]);
        auto next=[&]()->fs::path{
            if(i+1>=count)throw std::runtime_error("Missing option value");
            return argv[++i];
        };
        if(arg==L"--tc-dir")out.tc=next();
        else if(arg==L"--ini")out.ini=next();
        else if(arg==L"--plugin-dir")out.plugin=next();
        else if(arg==L"--arch") {
            auto value=next().wstring();
            if(value==L"32")out.arch=32;
            else if(value==L"64")out.arch=64;
            else throw std::runtime_error("--arch must be 32 or 64");
        } else if(arg==L"--dry-run")out.dry=true;
        else if(arg==L"--quiet")out.quiet=true;
        else throw std::runtime_error("Unknown command-line option");
    }
    return out;
}
void cli_error(const std::string& text) {
    HANDLE err=GetStdHandle(STD_ERROR_HANDLE);
    if(!err||err==INVALID_HANDLE_VALUE)return;
    DWORD written=0;
    WriteFile(err,text.data(),static_cast<DWORD>(text.size()),&written,nullptr);
}
} // namespace

int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    int argc=0;wchar_t** argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(!argv)return 2;
    bool quiet=false;
    try {
        Options opts=parse(argc,argv);quiet=opts.quiet;
        LocalFree(argv);
        auto summary=install(opts,nullptr);
        if(!quiet)MessageBoxW(nullptr,summary.c_str(),L"ISO-WCX installer",MB_OK|MB_ICONINFORMATION);
        return 0;
    } catch(const std::exception& e) {
        LocalFree(argv);
        cli_error(std::string("ISO-WCX installation failed: ")+e.what()+"\r\n");
        if(!quiet) {
            std::wstring msg=L"ISO-WCX installation failed; no user plugin associations were intentionally replaced.\n"
                             L"See the backup files if interrupted during commit.\n\n";
            auto reason=decode_multi(e.what(),static_cast<int>(std::strlen(e.what())),CP_ACP,0);
            msg+=reason;
            MessageBoxW(nullptr,msg.c_str(),L"ISO-WCX installer",MB_OK|MB_ICONERROR);
        }
        return 2;
    }
}
