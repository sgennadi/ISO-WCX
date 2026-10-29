// ISO-WCX standalone, single-file Windows installer for Total Commander.
// The installer embeds both native WCX modules and deploys only the module
// matching the selected Total Commander executable, never the OS bitness.
// No PowerShell, CMD helper, unpacker or extra plugin DLL is required.
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
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
namespace fs = std::filesystem;

namespace {
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct Options {
    fs::path tc, ini, destination;
    int arch = 0;
    bool quiet = false;
    bool dry_run = false;
};
std::wstring lower(std::wstring s) {
    std::transform(s.begin(),s.end(),s.begin(),[](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return s;
}
std::wstring trim(const std::wstring& s) {
    const auto b=s.find_first_not_of(L" \t\r\n\"");
    if(b==std::wstring::npos)return {};
    const auto e=s.find_last_not_of(L" \t\r\n\"");
    return s.substr(b,e-b+1);
}
std::wstring env(const wchar_t* key) {
    DWORD needed=GetEnvironmentVariableW(key,nullptr,0);
    if(!needed || needed>32767)return {};
    std::wstring out(needed,L'\0');
    DWORD n=GetEnvironmentVariableW(key,out.data(),needed);
    if(!n || n>=needed)return {};
    out.resize(n);
    return out;
}
std::wstring expand(std::wstring text,const fs::path& tc={}) {
    const auto needle=L"%commander_path%";
    for(;;) {
        auto p=lower(text).find(needle);
        if(p==std::wstring::npos)break;
        if(tc.empty())throw Error("Unresolved COMMANDER_PATH");
        text.replace(p,std::wcslen(needle),tc.wstring());
    }
    DWORD n=ExpandEnvironmentStringsW(text.c_str(),nullptr,0);
    if(!n || n>32767)throw Error("Invalid environment expansion");
    std::wstring result(n,L'\0');
    DWORD count=ExpandEnvironmentStringsW(text.c_str(),result.data(),n);
    if(!count || count>n)throw Error("Cannot expand environment variables");
    result.resize(count-1);
    if(result.find(L'%')!=std::wstring::npos)
        throw Error("Unresolved variable in a Total Commander configuration path");
    return result;
}
std::wstring registry(HKEY hive,const wchar_t* name,DWORD view=0) {
    const wchar_t* key=L"Software\\Ghisler\\Total Commander";
    DWORD cb=0;
    DWORD flags=RRF_RT_REG_SZ|RRF_RT_REG_EXPAND_SZ|view;
    if(RegGetValueW(hive,key,name,flags,nullptr,nullptr,&cb)!=ERROR_SUCCESS ||
       cb<sizeof(wchar_t) || cb>65536)return {};
    std::wstring out(cb/sizeof(wchar_t)+1,L'\0');
    if(RegGetValueW(hive,key,name,flags,nullptr,out.data(),&cb)!=ERROR_SUCCESS)
        return {};
    out.resize(std::wcslen(out.c_str()));
    return out;
}
std::wstring tc_registry(const wchar_t* name) {
    for(const auto& s : {
        registry(HKEY_CURRENT_USER,name),
        registry(HKEY_LOCAL_MACHINE,name,RRF_SUBKEY_WOW6464KEY),
        registry(HKEY_LOCAL_MACHINE,name,RRF_SUBKEY_WOW6432KEY),
        registry(HKEY_LOCAL_MACHINE,name)}) {
        if(!s.empty())return s;
    }
    return {};
}
bool exists_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p,ec) && !fs::is_symlink(p,ec);
}
bool same_path(const fs::path& a,const fs::path& b) {
    return lower(a.lexically_normal().wstring())==lower(b.lexically_normal().wstring());
}
fs::path discover_tc(const Options& o) {
    if(!o.tc.empty()) {
        if(!exists_file(o.tc/L"TOTALCMD.EXE") &&
           !exists_file(o.tc/L"TOTALCMD64.EXE"))
            throw Error("The selected directory contains no Total Commander executable");
        return fs::absolute(o.tc);
    }
    for(const auto& value : {env(L"COMMANDER_PATH"),tc_registry(L"InstallDir")}) {
        if(value.empty())continue;
        fs::path dir(expand(value));
        if(exists_file(dir/L"TOTALCMD.EXE") || exists_file(dir/L"TOTALCMD64.EXE"))
            return fs::absolute(dir);
    }
    throw Error("Total Commander was not found. Specify --tc-dir.");
}
std::pair<bool,bool> running_tc() {
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snap==INVALID_HANDLE_VALUE)
        throw Error("Cannot enumerate running processes safely");
    PROCESSENTRY32W info{};
    info.dwSize=sizeof(info);
    bool a=false,b=false;
    if(Process32FirstW(snap,&info)) {
        do {
            const auto name=lower(info.szExeFile);
            if(name==L"totalcmd.exe")a=true;
            else if(name==L"totalcmd64.exe")b=true;
        }while(Process32NextW(snap,&info));
    }
    CloseHandle(snap);
    return {a,b};
}
int select_arch(const fs::path& tc,const Options& o) {
    const bool x86=exists_file(tc/L"TOTALCMD.EXE");
    const bool x64=exists_file(tc/L"TOTALCMD64.EXE");
    if(o.arch) {
        if((o.arch==32 && !x86)||(o.arch==64 && !x64))
            throw Error("Requested Total Commander architecture is not installed");
        return o.arch;
    }
    if(x86&&!x64)return 32;
    if(x64&&!x86)return 64;
    if(!x86&&!x64)throw Error("No Total Commander executable found");
    const auto [run32,run64]=running_tc();
    if(run32!=run64)return run64?64:32;
    if(o.quiet)throw Error("Both TC architectures installed; pass --arch 32 or --arch 64");
    // Never silently infer TC architecture from the Windows operating system.
    int choice=MessageBoxW(nullptr,
        L"Both Total Commander 32-bit and 64-bit are installed.\n\n"
        L"YES: install the 64-bit ISO-WCX plugin\n"
        L"NO: install the 32-bit ISO-WCX plugin\n"
        L"CANCEL: leave the installation unchanged.",
        L"ISO-WCX - Select Total Commander",MB_YESNOCANCEL|MB_ICONQUESTION);
    if(choice==IDYES)return 64;
    if(choice==IDNO)return 32;
    throw Error("User cancelled architecture selection");
}
void require_tc_closed(bool quiet) {
    for(int attempt=0;attempt<3;attempt++) {
        const auto [a,b]=running_tc();
        if(!a&&!b)return;
        if(quiet)throw Error("Close Total Commander before changing plugins/INI");
        if(MessageBoxW(nullptr,
            L"Close ALL Total Commander windows, then click Retry.\n\n"
            L"The installer will resume automatically and will not overwrite\n"
            L"your toolbar buttons or custom commands.",
            L"ISO-WCX - Total Commander is running",
            MB_RETRYCANCEL|MB_ICONINFORMATION)!=IDRETRY)
            throw Error("Installation cancelled: Total Commander is still running");
    }
    if(const auto [a,b]=running_tc(); a||b)
        throw Error("Total Commander is still running; no settings were changed");
}
enum class Encoding { ansi, utf8, utf16le, utf16be };
std::vector<unsigned char> read_bytes(const fs::path& path,std::uint64_t limit) {
    std::ifstream stream(path,std::ios::binary|std::ios::ate);
    if(!stream || stream.tellg()<0 ||
       static_cast<std::uint64_t>(stream.tellg())>limit)
        throw Error("Missing or oversized input file");
    const auto n=static_cast<std::size_t>(stream.tellg());
    std::vector<unsigned char> buf(n);
    stream.seekg(0);
    if(n && !stream.read(reinterpret_cast<char*>(buf.data()),
                         static_cast<std::streamsize>(n)))
        throw Error("Cannot read the requested file");
    return buf;
}
void write_bytes(const fs::path& file,const std::vector<unsigned char>& data) {
    std::ofstream out(file,std::ios::binary|std::ios::trunc);
    if(!out)throw Error("Cannot create pending installation file");
    if(!data.empty())out.write(reinterpret_cast<const char*>(data.data()),
                               static_cast<std::streamsize>(data.size()));
    out.flush();
    if(!out.good())throw Error("Cannot write pending installation file");
    out.close();
    if(!out.good())throw Error("Cannot close pending installation file");
}
std::wstring from_multibyte(const unsigned char* p,int n,UINT cp,DWORD flags) {
    if(n==0)return {};
    int count=MultiByteToWideChar(cp,flags,reinterpret_cast<const char*>(p),n,
                                  nullptr,0);
    if(count<=0)throw Error("Invalid INI character encoding");
    std::wstring s(static_cast<std::size_t>(count),L'\0');
    if(MultiByteToWideChar(cp,flags,reinterpret_cast<const char*>(p),n,
                           s.data(),count)!=count)
        throw Error("Cannot decode Total Commander INI");
    return s;
}
std::vector<unsigned char> to_multibyte(const std::wstring& s,UINT cp,bool strict) {
    if(s.empty())return {};
    BOOL usedDefault=FALSE;
    BOOL* fail=strict?&usedDefault:nullptr;
    DWORD flags=strict?WC_NO_BEST_FIT_CHARS:0;
    int n=WideCharToMultiByte(cp,flags,s.data(),static_cast<int>(s.size()),
                              nullptr,0,nullptr,fail);
    if(n<=0 || usedDefault)throw Error("INI path cannot be encoded without data loss");
    std::vector<unsigned char> data(static_cast<std::size_t>(n));
    usedDefault=FALSE;
    if(WideCharToMultiByte(cp,flags,s.data(),static_cast<int>(s.size()),
                           reinterpret_cast<char*>(data.data()),n,nullptr,fail)!=n ||
       usedDefault)
        throw Error("Cannot encode Total Commander INI losslessly");
    return data;
}
struct Doc {
    fs::path path;
    std::vector<unsigned char> original;
    std::wstring content, eol=L"\r\n";
    Encoding encoding=Encoding::ansi;
};
Doc read_ini(const fs::path& path) {
    if(!exists_file(path))throw Error("Total Commander INI not found");
    Doc d;
    d.path=fs::absolute(path).lexically_normal();
    d.original=read_bytes(d.path,16ULL*1024*1024);
    const auto& b=d.original;
    std::size_t skip=0;
    if(b.size()>=3 && b[0]==0xef && b[1]==0xbb && b[2]==0xbf) {
        d.encoding=Encoding::utf8;skip=3;
    } else if(b.size()>=2 && b[0]==0xff && b[1]==0xfe) {
        d.encoding=Encoding::utf16le;skip=2;
    } else if(b.size()>=2 && b[0]==0xfe && b[1]==0xff) {
        d.encoding=Encoding::utf16be;skip=2;
    } else {
        // Recognize valid UTF-8 without BOM; legacy ANSI otherwise.
        // Round-trip validation below guarantees no original bytes change.
        d.encoding=Encoding::utf8;
        if(!b.empty()) {
            if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,
                    reinterpret_cast<const char*>(b.data()),
                    static_cast<int>(b.size()),nullptr,0)<=0)
                d.encoding=Encoding::ansi;
        }
    }
    if(d.encoding==Encoding::utf16le || d.encoding==Encoding::utf16be) {
        if((b.size()-skip)%2)throw Error("Truncated UTF-16 INI");
        for(std::size_t i=skip;i<b.size();i+=2) {
            unsigned x=d.encoding==Encoding::utf16le
                ? unsigned(b[i]) | (unsigned(b[i+1])<<8)
                : (unsigned(b[i])<<8) | unsigned(b[i+1]);
            d.content.push_back(static_cast<wchar_t>(x));
        }
    } else {
        const auto cp=d.encoding==Encoding::utf8?CP_UTF8:CP_ACP;
        d.content=from_multibyte(b.data()+skip,static_cast<int>(b.size()-skip),
            cp,d.encoding==Encoding::utf8?MB_ERR_INVALID_CHARS:0);
    }
    if(d.content.find(L'\0')!=std::wstring::npos)
        throw Error("Unexpected NUL in Total Commander configuration");
    d.eol=d.content.find(L"\r\n")!=std::wstring::npos?L"\r\n":L"\n";
    return d;
}
std::vector<unsigned char> encode_ini(const Doc& d,const std::wstring& text) {
    std::vector<unsigned char> out;
    const auto& b=d.original;
    if(d.encoding==Encoding::utf16le || d.encoding==Encoding::utf16be) {
        out.assign(b.begin(),b.begin()+2);
        for(wchar_t c:text) {
            const auto val=static_cast<unsigned>(c);
            if(d.encoding==Encoding::utf16le) {
                out.push_back(static_cast<unsigned char>(val&0xff));
                out.push_back(static_cast<unsigned char>((val>>8)&0xff));
            } else {
                out.push_back(static_cast<unsigned char>((val>>8)&0xff));
                out.push_back(static_cast<unsigned char>(val&0xff));
            }
        }
    } else {
        const auto cp=d.encoding==Encoding::utf8?CP_UTF8:CP_ACP;
        const auto flags=d.encoding==Encoding::ansi;
        auto body=to_multibyte(text,cp,flags);
        if(d.encoding==Encoding::utf8 && b.size()>=3 &&
           b[0]==0xef && b[1]==0xbb && b[2]==0xbf)
            out.insert(out.end(),b.begin(),b.begin()+3);
        out.insert(out.end(),body.begin(),body.end());
    }
    return out;
}
struct Line {
    std::wstring body,end;
};
std::vector<Line> lines(const std::wstring& text) {
    std::vector<Line> out;
    for(std::size_t pos=0;pos<text.size();) {
        const auto e=text.find(L'\n',pos);
        if(e==std::wstring::npos) {
            out.push_back({text.substr(pos),L""});
            break;
        }
        auto body=text.substr(pos,e-pos);
        std::wstring ending=L"\n";
        if(!body.empty()&&body.back()==L'\r') {
            body.pop_back();
            ending=L"\r\n";
        }
        out.push_back({std::move(body),std::move(ending)});
        pos=e+1;
    }
    return out;
}
std::optional<std::wstring> section(const std::wstring& raw) {
    const auto s=trim(raw);
    if(s.size()>2 && s.front()==L'[' && s.back()==L']')
        return lower(trim(s.substr(1,s.size()-2)));
    return {};
}
std::optional<std::pair<std::wstring,std::wstring>> assignment(const std::wstring& raw) {
    const auto eq=raw.find(L'=');
    if(eq==std::wstring::npos)return {};
    auto key=lower(trim(raw.substr(0,eq)));
    if(key.empty() || key.front()==L';' || key.front()==L'#')return {};
    return std::pair{key,trim(raw.substr(eq+1))};
}
struct Entries {
    std::map<std::wstring,std::wstring> values;
    std::map<std::wstring,std::size_t> positions;
    bool found=false;
};
Entries read_section(const Doc& d,const std::wstring& which) {
    Entries result;
    std::wstring current;
    std::size_t i=0;
    for(const auto& l:lines(d.content)) {
        if(const auto head=section(l.body)) {
            current=*head;
            if(current==which) {
                if(result.found)throw Error("Duplicate INI section; refusing ambiguous edit");
                result.found=true;
            }
        } else if(current==which) {
            if(const auto entry=assignment(l.body)) {
                if(result.values.count(entry->first))
                    throw Error("Duplicate packer INI key; refusing ambiguous edit");
                result.values[entry->first]=entry->second;
                result.positions[entry->first]=i;
            }
        }
        ++i;
    }
    return result;
}
std::optional<std::wstring> value(const Doc& doc,const std::wstring& sec,
                                   const std::wstring& key) {
    const auto e=read_section(doc,sec);
    const auto at=e.values.find(key);
    if(at==e.values.end())return {};
    return at->second;
}
fs::path discover_ini(const Options& o,const fs::path& tc) {
    if(!o.ini.empty())return fs::absolute(o.ini);
    if(const auto e=env(L"COMMANDER_INI"); !e.empty())
        return fs::path(expand(e,tc));
    for(const auto& r:{tc_registry(L"IniFileName")}) {
        if(!r.empty())return fs::path(expand(r,tc));
    }
    const auto local=tc/L"wincmd.ini";
    if(exists_file(local)) {
        const auto doc=read_ini(local);
        if(value(doc,L"configuration",L"useiniinprogramdir")==L"7")
            return local;
    }
    const auto appdata=env(L"APPDATA");
    if(appdata.empty())throw Error("Cannot locate Total Commander INI; specify --ini");
    return fs::path(appdata)/L"GHISLER"/L"wincmd.ini";
}
fs::path section_path(const Doc& root,const std::wstring& sec,
                      const fs::path& tc) {
    auto redirect=value(root,sec,L"redirectsection");
    if(!redirect || redirect->empty() || *redirect==L"0")
        return root.path;
    if(*redirect==L"1") {
        redirect=value(root,L"configuration",L"alternateuserini");
        if(!redirect || redirect->empty())throw Error("Missing AlternateUserIni");
    }
    fs::path path(expand(*redirect,tc));
    if(path.is_relative())path=root.path.parent_path()/path;
    if(!exists_file(path))throw Error("RedirectSection target does not exist");
    return fs::absolute(path).lexically_normal();
}
std::wstring own_archive_name(const std::wstring& raw) {
    const auto comma=raw.find(L',');
    if(comma==std::wstring::npos)return {};
    return trim(raw.substr(comma+1));
}
bool is_own(const std::wstring& s) {
    if(s.empty())return false;
    const fs::path path(s);
    if(lower(path.filename().wstring())!=L"iso-wcx.wcx" &&
       lower(path.filename().wstring())!=L"iso-wcx.wcx64")
        return false;
    return lower(path.parent_path().filename().wstring())==L"iso-wcx";
}
bool is_missing_legacy_iso(const std::wstring& raw,const fs::path& tc) {
    const auto file=own_archive_name(raw);
    if(file.empty())return false;
    const fs::path path(file);
    if(lower(path.filename().wstring())!=L"iso.wcx" ||
       lower(path.parent_path().filename().wstring())!=L"iso")
        return false;
    try {
        return !exists_file(fs::path(expand(file,tc)));
    }catch(...){return false;}
}
const std::array<const wchar_t*,23> extensions={
    L"iso",L"udf",L"cue",L"ccd",L"nrg",L"mds",L"mdx",
    L"cso",L"zso",L"isz",L"daa",L"img",L"raw",L"vhd",
    L"vhdx",L"wim",L"esd",L"swm",L"ima",L"vfd",L"flp",L"bif",L"imz"
};
struct Plan {
    fs::path path;
    Doc original;
    std::vector<unsigned char> updated;
    fs::path stage,backup;
    bool committed=false;
};
std::wstring render(const Doc& d,const std::wstring& sec,
                    const std::map<std::wstring,std::wstring>& replacements,
                    const std::vector<std::wstring>& additions,bool drop_checksum) {
    const auto old=lines(d.content);
    std::wstring output,current;
    bool found=false;
    for(const auto& l:old) {
        if(const auto h=section(l.body)) {
            current=*h;
            if(current==sec) {
                found=true;
                output+=l.body;
                output+=l.end.empty()?d.eol:l.end;
                for(const auto& add:additions)output+=add+d.eol;
                continue;
            }
        }
        if(current==sec) {
            if(const auto entry=assignment(l.body)) {
                if(const auto r=replacements.find(entry->first);
                   r!=replacements.end()) {
                    // Keep original user indentation; only WCX value is replaced.
                    const auto first=l.body.find_first_not_of(L" \t");
                    if(first!=std::wstring::npos)
                        output+=l.body.substr(0,first);
                    output+=entry->first+L"="+r->second+l.end;
                    continue;
                }
            }
        }
        if(drop_checksum && current==L"packerplugins64") {
            if(const auto entry=assignment(l.body);
               entry && entry->first==L"$checksum$")
                continue;
        }
        output+=l.body+l.end;
    }
    if(!found && !additions.empty()) {
        if(!output.empty()&&output.back()!=L'\n')output+=d.eol;
        output+=L"[PackerPlugins]"+d.eol;
        for(const auto& add:additions)output+=add+d.eol;
    }
    return output;
}
std::wstring render_cache(const Doc& d) {
    return render(d,L"packerplugins64",{}, {},true);
}
std::vector<unsigned char> load_resource(int name,std::uint16_t expectedMachine) {
    HRSRC res=FindResourceW(nullptr,MAKEINTRESOURCEW(name),RT_RCDATA);
    if(!res)throw Error("Embedded WCX resource is missing");
    HGLOBAL handle=LoadResource(nullptr,res);
    if(!handle)throw Error("Cannot load embedded WCX");
    const DWORD n=SizeofResource(nullptr,res);
    const auto* ptr=static_cast<const unsigned char*>(LockResource(handle));
    if(!ptr || n<256)throw Error("Empty embedded WCX");
    std::vector<unsigned char> out(ptr,ptr+n);
    if(out[0]!=0x4d || out[1]!=0x5a)throw Error("Embedded plugin is not PE");
    const std::uint32_t offset=std::uint32_t(out[60])|
        (std::uint32_t(out[61])<<8)|(std::uint32_t(out[62])<<16)|
        (std::uint32_t(out[63])<<24);
    if(offset>n-6 || out[offset]!='P' || out[offset+1]!='E' ||
       out[offset+2]!=0 || out[offset+3]!=0 ||
       ((unsigned(out[offset+5])<<8)|unsigned(out[offset+4]))!=expectedMachine)
        throw Error("Embedded plugin has the wrong CPU architecture");
    return out;
}
fs::path unique(const fs::path& file,const wchar_t* kind) {
    SYSTEMTIME now{};GetLocalTime(&now);
    for(unsigned i=0;i<100;i++) {
        std::wstring out=file.wstring()+kind+
            std::to_wstring(now.wYear)+L"-"+
            std::to_wstring(now.wMonth)+L"-"+
            std::to_wstring(now.wDay)+L"-"+
            std::to_wstring(GetCurrentProcessId())+L"-"+
            std::to_wstring(GetTickCount64())+L"-"+
            std::to_wstring(i);
        if(!fs::exists(out))return fs::path(out);
    }
    throw Error("Cannot allocate a unique temporary filename");
}
void replace_with_backup(const fs::path& dest,const fs::path& staged,
                         const fs::path& backup) {
    if(!ReplaceFileW(dest.c_str(),staged.c_str(),backup.c_str(),
                     REPLACEFILE_IGNORE_MERGE_ERRORS,nullptr,nullptr))
        throw Error("Atomic INI replacement failed; check write permissions");
}
void restore(const Plan& p) {
    if(p.committed && exists_file(p.backup))
        CopyFileW(p.backup.c_str(),p.path.c_str(),FALSE);
}
std::wstring install(const Options& opts) {
    const fs::path tc=discover_tc(opts);
    const int arch=select_arch(tc,opts);
    const auto dll=load_resource(arch==64?102:101,
                                  arch==64?IMAGE_FILE_MACHINE_AMD64:IMAGE_FILE_MACHINE_I386);
    const fs::path root_ini=discover_ini(opts,tc);
    Doc root=read_ini(root_ini);
    const auto packer_path=section_path(root,L"packerplugins",tc);
    const auto cache_path=section_path(root,L"packerplugins64",tc);
    Doc packer=same_path(packer_path,root_ini)?root:read_ini(packer_path);
    Doc cache=same_path(cache_path,packer_path)?packer:
              same_path(cache_path,root_ini)?root:read_ini(cache_path);
    const auto existing=read_section(packer,L"packerplugins");
    fs::path target_dir;
    if(!opts.destination.empty())target_dir=fs::absolute(opts.destination);
    else {
        const auto pos=existing.values.find(L"iso");
        if(pos!=existing.values.end() && is_own(own_archive_name(pos->second))) {
            try {
                fs::path previous(expand(own_archive_name(pos->second),tc));
                if(!previous.is_relative() && lower(previous.parent_path().filename().wstring())==L"iso-wcx")
                    target_dir=previous.parent_path();
            }catch(...){}
        }
        if(target_dir.empty())target_dir=tc/L"plugins"/L"wcx"/L"ISO-WCX";
    }
    target_dir=target_dir.lexically_normal();
    const auto destination=target_dir/
        (arch==64?L"ISO-WCX.wcx64":L"ISO-WCX.wcx");
    const auto replacement=L"158,"+destination.wstring();
    std::map<std::wstring,std::wstring> changes;
    std::vector<std::wstring> additions;
    unsigned preserved=0;
    for(const auto* ext:extensions) {
        const std::wstring key(ext);
        const auto it=existing.values.find(key);
        if(it==existing.values.end()) {
            additions.push_back(key+L"="+replacement);
        } else if(is_own(own_archive_name(it->second)) ||
                  (key==L"iso" && is_missing_legacy_iso(it->second,tc))) {
            if(it->second!=replacement)changes[key]=replacement;
        } else ++preserved;
    }
    const unsigned updated=static_cast<unsigned>(changes.size()+additions.size());
    // The only configuration entries affected are the selected packer
    // extensions and the derived 64-bit WCX checksum. No toolbar entries.
    std::vector<Plan> plans;
    auto add_plan=[&](const Doc& doc,std::wstring text) {
        if(text==doc.content)return;
        Plan p;
        p.path=doc.path;
        p.original=doc;
        p.updated=encode_ini(doc,text);
        plans.push_back(std::move(p));
    };
    const std::wstring pack_text=render(packer,L"packerplugins",changes,additions,
                                        same_path(packer_path,cache_path));
    add_plan(packer,pack_text);
    if(!same_path(cache_path,packer_path))
        add_plan(cache,render_cache(cache));
    std::wstring summary=L"Total Commander: "+tc.wstring()+
        L"\nArchitecture: "+std::to_wstring(arch)+L"-bit"+
        L"\nPlugin: "+destination.wstring()+
        L"\nImage associations added/updated: "+std::to_wstring(updated)+
        L"\nOther packer assignments preserved: "+std::to_wstring(preserved)+
        L"\nZIP/RAR/7z and toolbar commands remain unchanged.";
    if(opts.dry_run)return L"READ ONLY PREVIEW\n\n"+summary;
    require_tc_closed(opts.quiet);
    // Reject linked configuration files (symlink/junction redirect attacks).
    for(const auto& p:plans) {
        const DWORD attrs=GetFileAttributesW(p.path.c_str());
        if(attrs==INVALID_FILE_ATTRIBUTES || (attrs&FILE_ATTRIBUTE_REPARSE_POINT))
            throw Error("Refusing to replace linked or inaccessible TC INI");
        if(read_bytes(p.path,16ULL*1024*1024)!=p.original.original)
            throw Error("Total Commander settings changed during preflight");
    }
    fs::create_directories(target_dir);
    if(fs::is_symlink(target_dir))throw Error("Linked plugin directory is not supported");
    // Prepare ALL stages before changing any original file.
    const fs::path dll_stage=unique(destination,L".iso-wcx-pending-");
    const fs::path dll_backup=unique(destination,L".iso-wcx-backup-");
    bool dll_installed=false;
    const bool dll_existed=exists_file(destination);
    try {
        write_bytes(dll_stage,dll);
        for(auto& p:plans) {
            p.stage=unique(p.path,L".iso-wcx-pending-");
            p.backup=unique(p.path,L".iso-wcx-backup-");
            write_bytes(p.stage,p.updated);
        }
        if(dll_existed)replace_with_backup(destination,dll_stage,dll_backup);
        else if(!MoveFileExW(dll_stage.c_str(),destination.c_str(),MOVEFILE_WRITE_THROUGH))
            throw Error("Cannot install the selected WCX binary");
        dll_installed=true;
        for(auto& p:plans) {
            if(read_bytes(p.path,16ULL*1024*1024)!=p.original.original)
                throw Error("INI changed immediately before commit");
        }
        for(auto& p:plans) {
            replace_with_backup(p.path,p.stage,p.backup);
            p.committed=true;
        }
    }catch(...) {
        for(auto it=plans.rbegin();it!=plans.rend();++it)restore(*it);
        if(dll_installed) {
            if(dll_existed && exists_file(dll_backup))
                CopyFileW(dll_backup.c_str(),destination.c_str(),FALSE);
            else if(!dll_existed)DeleteFileW(destination.c_str());
        }
        if(fs::exists(dll_stage))fs::remove(dll_stage);
        for(const auto& p:plans)
            if(!p.stage.empty()&&fs::exists(p.stage))fs::remove(p.stage);
        throw;
    }
    if(fs::exists(dll_stage))fs::remove(dll_stage);
    for(const auto& p:plans)
        if(!p.stage.empty()&&fs::exists(p.stage))fs::remove(p.stage);
    summary+=L"\n\nInstallation completed. Restart Total Commander.";
    return summary;
}
Options arguments(int argc,wchar_t** argv) {
    Options o;
    for(int i=1;i<argc;i++) {
        const auto a=lower(argv[i]);
        auto next=[&]() {
            if(i+1>=argc)throw Error("Missing value after command-line option");
            return std::wstring(argv[++i]);
        };
        if(a==L"--tc-dir")o.tc=fs::path(next());
        else if(a==L"--ini")o.ini=fs::path(next());
        else if(a==L"--plugin-dir")o.destination=fs::path(next());
        else if(a==L"--arch") {
            const auto v=next();
            if(v==L"32")o.arch=32;
            else if(v==L"64")o.arch=64;
            else throw Error("--arch must be 32 or 64");
        } else if(a==L"--quiet")o.quiet=true;
        else if(a==L"--dry-run")o.dry_run=true;
        else throw Error("Unknown ISO-WCX installer option");
    }
    return o;
}
} // namespace
int WINAPI wWinMain(HINSTANCE,HINSTANCE,LPWSTR,int) {
    int argc=0;
    wchar_t** argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(!argv)return 2;
    bool quiet=false;
    for(int i=1;i<argc;i++)
        if(lower(argv[i])==L"--quiet")quiet=true;
    try {
        const Options opts=arguments(argc,argv);
        LocalFree(argv);
        const std::wstring message=install(opts);
        if(!opts.quiet)
            MessageBoxW(nullptr,message.c_str(),L"ISO-WCX Installer",MB_ICONINFORMATION|MB_OK);
        return 0;
    }catch(const std::exception& e) {
        LocalFree(argv);
        if(!quiet) {
            std::wstring msg=L"ISO-WCX installation stopped without overwriting "
                             L"unrelated Total Commander settings.\n\n";
            const auto* what=e.what();
            if(what) {
                int n=MultiByteToWideChar(CP_UTF8,0,what,-1,nullptr,0);
                if(n>0) {
                    std::wstring wide(static_cast<std::size_t>(n),L'\0');
                    MultiByteToWideChar(CP_UTF8,0,what,-1,wide.data(),n);
                    msg+=wide.c_str();
                }
            }
            msg+=L"\n\nCheck TC installation path, INI permissions, or use --tc-dir.";
            MessageBoxW(nullptr,msg.c_str(),L"ISO-WCX Installer",MB_ICONERROR|MB_OK);
        }
        return 2;
    }
}
