// Total Commander WCX registration audit: read-only, no helper EXE.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "iso_wcx/tc_registration_audit.hpp"
#include "iso_wcx/tc_association_flags.hpp"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace iso_wcx {
namespace {
namespace fs=std::filesystem;
std::wstring env(const wchar_t* key) {
    const DWORD count=GetEnvironmentVariableW(key,nullptr,0);
    if(!count||count>32768)return {};
    std::wstring out(count,L'\0');
    DWORD read=GetEnvironmentVariableW(key,out.data(),count);
    if(read>=count)return {};
    out.resize(read);
    return out;
}
std::wstring lower(std::wstring text) {
    std::transform(text.begin(),text.end(),text.begin(),
                   [](wchar_t c){return static_cast<wchar_t>(std::towlower(c));});
    return text;
}
std::wstring trim(std::wstring text) {
    const auto start=text.find_first_not_of(L" \t\r\n\"");
    if(start==text.npos)return {};
    const auto end=text.find_last_not_of(L" \t\r\n\"");
    return text.substr(start,end-start+1);
}
std::wstring expand(std::wstring text) {
    const DWORD n=ExpandEnvironmentStringsW(text.c_str(),nullptr,0);
    if(!n||n>32768)return text;
    std::wstring out(n,L'\0');
    DWORD copied=ExpandEnvironmentStringsW(text.c_str(),out.data(),n);
    if(!copied||copied>n)return text;
    out.resize(copied-1);
    return out;
}
std::wstring reg_ini() {
    constexpr auto subkey=L"Software\\Ghisler\\Total Commander";
    std::vector<wchar_t> data(32768,L'\0');
    DWORD bytes=static_cast<DWORD>(data.size()*sizeof(wchar_t));
    for(const HKEY hive:{HKEY_CURRENT_USER,HKEY_LOCAL_MACHINE}) {
        bytes=static_cast<DWORD>(data.size()*sizeof(wchar_t));
        if(RegGetValueW(hive,subkey,L"IniFileName",RRF_RT_REG_SZ|
           RRF_RT_REG_EXPAND_SZ,nullptr,data.data(),&bytes)==ERROR_SUCCESS)
           return expand(data.data());
    }
    return {};
}
std::wstring profile(const fs::path& file,const wchar_t* section,
                     const wchar_t* key) {
    std::vector<wchar_t> buffer(32768,L'\0');
    constexpr wchar_t absent[]=L"\x01";
    const DWORD n=GetPrivateProfileStringW(section,key,absent,buffer.data(),
                                           static_cast<DWORD>(buffer.size()),
                                           file.c_str());
    if(n>=buffer.size()-2)return {};
    return std::wstring_view(buffer.data(),n)==absent?
           std::wstring{}:std::wstring(buffer.data(),n);
}
fs::path redirect(const fs::path& file,const wchar_t* section) {
    const auto value=trim(profile(file,section,L"RedirectSection"));
    if(value.empty()||value==L"0")return file;
    std::wstring location=value;
    if(value==L"1")
        location=trim(profile(file,L"Configuration",L"AlternateUserIni"));
    if(location.empty())return {};
    fs::path path(expand(location));
    if(path.is_relative())path=file.parent_path()/path;
    return path.lexically_normal();
}
bool is_file(const fs::path& file) {
    std::error_code e;
    return fs::is_regular_file(file,e) && !fs::is_symlink(file,e);
}
bool contains_cache_extension(std::wstring value,std::wstring wanted) {
    value=lower(value);
    wanted=lower(wanted);
    std::size_t start=0;
    while(start<value.size()) {
        const auto end=value.find_first_of(L"*;,",start);
        const auto item=trim(value.substr(start,end==value.npos?value.npos:end-start));
        if(item==wanted)return true;
        if(end==value.npos)break;
        start=end+1;
    }
    return false;
}
void put(std::wofstream& out,const std::wstring& s){
    out<<s<<L"\n";
}
} // namespace

std::filesystem::path export_tc_registration_audit() noexcept {
    try {
        auto local=env(L"LOCALAPPDATA");
        if(local.empty())return {};
        const fs::path report=fs::path(local)/L"ISO-WCX"/L"Reports"/
                              L"ISO-WCX-TC-Associations.txt";
        fs::create_directories(report.parent_path());
        std::wofstream out(report,std::ios::trunc);
        if(!out)return {};
        put(out,L"ISO-WCX | Total Commander registration diagnostics (read-only)");
        put(out,L"Installed WCX reports GetPackerCaps=222; 158 and 159 are also writable.");
        put(out,L"158 = modify+multiple+delete+options+search; 159 adds NEW.");
        put(out,L"192 = content+search ONLY: F5 and Delete unavailable by registration.");
        put(out,L"Nothing is modified; close Total Commander before changing wincmd.ini.");
        put(out,L"WARNING: This private report contains file paths. Review before sharing.");
        auto ini=env(L"COMMANDER_INI");
        if(ini.empty())ini=reg_ini();
        if(ini.empty()) {
            const auto appdata=env(L"APPDATA");
            if(!appdata.empty())ini=(fs::path(appdata)/L"Ghisler"/L"wincmd.ini").wstring();
        }
        if(ini.empty()||!is_file(ini)) {
            put(out,L"STATUS: active wincmd.ini not found. Open Help > About in TC.");
            return report;
        }
        auto ini_path=fs::path(ini).lexically_normal();
        auto associations=redirect(ini_path,L"PackerPlugins");
        auto cache=redirect(ini_path,L"PackerPlugins64");
        put(out,L"Main INI: "+ini_path.wstring());
        put(out,L"Associations INI: "+associations.wstring());
        put(out,L"64-bit cache INI: "+cache.wstring());
        if(!is_file(associations)) {
            put(out,L"STATUS: associations INI not found or redirected incorrectly.");
            return report;
        }
        std::wstring tc_path=env(L"COMMANDER_PATH");
        if(tc_path.empty()){
            std::array<wchar_t,32768> executable{};
            const DWORD n=GetModuleFileNameW(nullptr,executable.data(),
                                              static_cast<DWORD>(executable.size()));
            if(n>0&&n<executable.size())
                tc_path=fs::path(std::wstring(executable.data(),n)).parent_path().wstring();
        }
        const std::wstring exts=is_file(cache)?
              profile(cache,L"PackerPlugins64",L"exts"):L"";
        put(out,L"Derived 64-bit [PackerPlugins64] exts: "+
                (exts.empty()?L"not found":L"present"));
        for(const wchar_t* extension:{L"wim",L"iso",L"udf",L"esd",L"swm"}) {
            const std::wstring value=profile(associations,L"PackerPlugins",extension);
            put(out,L"");
            put(out,L"["+std::wstring(extension)+L"]");
            if(value.empty()){
                put(out,L"STATUS: NOT REGISTERED");
                continue;
            }
            put(out,L"Entry: "+value);
            const unsigned mask=tc_association::flags(value);
            put(out,L"Capabilities: "+std::to_wstring(mask));
            const bool editable=std::wstring_view(extension)!=L"esd" &&
                                std::wstring_view(extension)!=L"swm";
            if(editable&&!tc_association::can_edit(mask))
                put(out,L"ERROR: WCX registration DISABLES F5/Delete (MODIFY/DELETE flags missing).");
            else if(editable)
                put(out,L"EDIT FLAGS: OK (F5/Delete bits present)");
            else
                put(out,L"ESD/SWM: read-only; no F5/Delete expected.");
            if(tc_association::can_create(mask))
                put(out,L"NOTE: NEW archive bit enabled; ISO-WCX does not create WIMs from scratch.");
            const auto delimiter=value.find(L',');
            if(delimiter==value.npos){
                put(out,L"ERROR: Missing comma/path in PackerPlugins entry.");
                continue;
            }
            std::wstring path=trim(value.substr(delimiter+1));
            constexpr std::wstring_view token=L"%commander_path%";
            const auto idx=lower(path).find(token);
            if(idx!=std::wstring::npos&&!tc_path.empty())
                path.replace(idx,token.size(),tc_path);
            const fs::path dll(expand(path));
            put(out,L"Registered x86 WCX: "+dll.wstring());
            if(lower(dll.filename().wstring())!=L"iso-wcx.wcx")
                put(out,L"ERROR: different/legacy plugin registered for this extension.");
            else if(!is_file(dll))
                put(out,L"ERROR: ISO-WCX.wcx missing at configured path.");
            else
                put(out,L"Registered x86 WCX: EXISTS.");
            const fs::path dll64=dll.parent_path()/L"ISO-WCX.wcx64";
            put(out,L"Companion x64 WCX: "+dll64.wstring());
            if(!is_file(dll64))
                put(out,L"ERROR: ISO-WCX.wcx64 missing. 64-bit TC cannot use this registration.");
            if(!exts.empty()&&!contains_cache_extension(exts,extension))
                put(out,L"WARNING: 64-bit extension cache does not list this type.");
        }
        put(out,L"");
        put(out,L"Recommended: for writable WIM/ISO/UDF use 158 or 222, NOT 192.");
        put(out,L"No auto-edit while Total Commander is running; back up wincmd.ini first.");
        out.flush();
        return out?report:fs::path{};
    }catch(...){return {};}
}
} // namespace iso_wcx
#endif
