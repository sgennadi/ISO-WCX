// Native, opt-in Total Commander association repair for ISO-WCX Configure.
// A separate Windows rundll32 process waits until all TC windows exit.
// No script, installer EXE, service or third-party library is distributed.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "iso_wcx/tc_registration_repair.hpp"
#include "iso_wcx/modern_dialog.hpp"
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace iso_wcx {
namespace {
namespace fs=std::filesystem;
using Bytes=std::vector<std::uint8_t>;
enum class Encoding { ansi, utf8, utf16le, utf16be };
struct Line {std::wstring text; Bytes raw; bool removed=false;};
struct Document {Encoding encoding=Encoding::ansi;Bytes bom;std::vector<Line> lines;std::wstring eol=L"\r\n";};
struct Plan {AssociationFixPlan visible;Document doc;Bytes original;};

void fail(std::string message){throw std::runtime_error(std::move(message));}
std::wstring lower(std::wstring s){
    std::transform(s.begin(),s.end(),s.begin(),[](wchar_t c){return static_cast<wchar_t>(std::towlower(c));});
    return s;
}
std::wstring trim(std::wstring_view s){
    const auto b=s.find_first_not_of(L" \t\r\n\"");
    if(b==s.npos)return {};
    const auto e=s.find_last_not_of(L" \t\r\n\"");
    return std::wstring(s.substr(b,e-b+1));
}
bool same(std::wstring a,std::wstring b){return lower(std::move(a))==lower(std::move(b));}
bool safe_file(const fs::path& file) {
    const DWORD attr=GetFileAttributesW(file.c_str());
    return attr!=INVALID_FILE_ATTRIBUTES &&
           !(attr&FILE_ATTRIBUTE_DIRECTORY) && !(attr&FILE_ATTRIBUTE_REPARSE_POINT);
}
Bytes read_bytes(const fs::path& path){
    if(!safe_file(path))fail("Total Commander INI is missing or is a reparse point");
    std::ifstream stream(path,std::ios::binary|std::ios::ate);
    if(!stream)fail("Cannot open Total Commander INI");
    const auto size=stream.tellg();
    if(size<0 || size>16*1024*1024)fail("INI size exceeds safe 16-MiB limit");
    Bytes result(static_cast<std::size_t>(size));
    stream.seekg(0);
    if(!result.empty())stream.read(reinterpret_cast<char*>(result.data()),
                                  static_cast<std::streamsize>(result.size()));
    if(!stream && !result.empty())fail("Cannot read Total Commander INI");
    return result;
}
std::wstring decode(const Bytes& bytes,Encoding encoding){
    if(bytes.empty())return {};
    if(encoding==Encoding::utf16le || encoding==Encoding::utf16be){
        if(bytes.size()%2)fail("Invalid UTF-16 INI length");
        std::wstring result;
        result.reserve(bytes.size()/2);
        for(std::size_t i=0;i<bytes.size();i+=2){
            const unsigned value=encoding==Encoding::utf16le?
                (static_cast<unsigned>(bytes[i])|
                 (static_cast<unsigned>(bytes[i+1])<<8)):
                ((static_cast<unsigned>(bytes[i])<<8)|
                 static_cast<unsigned>(bytes[i+1]));
            if(!value)fail("NUL character in INI");
            result.push_back(static_cast<wchar_t>(value));
        }
        return result;
    }
    const UINT cp=encoding==Encoding::utf8?CP_UTF8:CP_ACP;
    const DWORD flags=encoding==Encoding::utf8?MB_ERR_INVALID_CHARS:0;
    if(bytes.size()>INT_MAX)fail("INI too long");
    const auto count=MultiByteToWideChar(cp,flags,
          reinterpret_cast<const char*>(bytes.data()),
          static_cast<int>(bytes.size()),nullptr,0);
    if(!count)fail("INI cannot be decoded");
    std::wstring result(static_cast<std::size_t>(count),L'\0');
    if(!MultiByteToWideChar(cp,flags,
          reinterpret_cast<const char*>(bytes.data()),
          static_cast<int>(bytes.size()),result.data(),count))
        fail("INI decode failed");
    if(result.find(L'\0')!=result.npos)fail("NUL character in INI");
    return result;
}
Bytes encode(std::wstring_view value,Encoding encoding){
    Bytes out;
    if(encoding==Encoding::utf16le || encoding==Encoding::utf16be){
        out.reserve(value.size()*2);
        for(wchar_t c:value) {
            const auto v=static_cast<unsigned>(c);
            if(encoding==Encoding::utf16le){
                out.push_back(static_cast<std::uint8_t>(v));
                out.push_back(static_cast<std::uint8_t>(v>>8));
            }else{
                out.push_back(static_cast<std::uint8_t>(v>>8));
                out.push_back(static_cast<std::uint8_t>(v));
            }
        }
        return out;
    }
    if(value.empty())return {};
    if(value.size()>INT_MAX)fail("INI line too large");
    const UINT cp=encoding==Encoding::utf8?CP_UTF8:CP_ACP;
    const int length=WideCharToMultiByte(cp,0,value.data(),
        static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    if(length<=0)fail("Cannot encode INI line");
    out.resize(static_cast<std::size_t>(length));
    if(!WideCharToMultiByte(cp,0,value.data(),
        static_cast<int>(value.size()),reinterpret_cast<char*>(out.data()),
        length,nullptr,nullptr))fail("Cannot write INI encoding");
    // No silent ANSI codepage degradation in a path we rewrite.
    if(decode(out,encoding)!=value)fail("INI path cannot roundtrip in original encoding");
    return out;
}
Document parse(const Bytes& bytes) {
    Document doc;
    std::size_t off=0;
    if(bytes.size()>=3 && bytes[0]==239 && bytes[1]==187 && bytes[2]==191){
        doc.encoding=Encoding::utf8;off=3;
    }else if(bytes.size()>=2 && bytes[0]==255 && bytes[1]==254){
        doc.encoding=Encoding::utf16le;off=2;
    }else if(bytes.size()>=2 && bytes[0]==254 && bytes[1]==255){
        doc.encoding=Encoding::utf16be;off=2;
    }
    doc.bom.insert(doc.bom.end(),bytes.begin(),bytes.begin()+off);
    const bool wide=doc.encoding==Encoding::utf16le || doc.encoding==Encoding::utf16be;
    if(wide && (bytes.size()-off)%2)fail("Malformed UTF-16 INI");
    const auto character=[&](std::size_t i)->unsigned {
        if(wide)return doc.encoding==Encoding::utf16le?
            (static_cast<unsigned>(bytes[i])|
             (static_cast<unsigned>(bytes[i+1])<<8)):
            ((static_cast<unsigned>(bytes[i])<<8)|
             static_cast<unsigned>(bytes[i+1]));
        return bytes[i];
    };
    const std::size_t stride=wide?2u:1u;
    std::size_t position=off;
    while(position<bytes.size()) {
        const auto begin=position;
        while(position<bytes.size() && character(position)!=13 && character(position)!=10)
            position+=stride;
        if(position<bytes.size()){
            const auto first=character(position);
            position+=stride;
            if(first==13 && position<bytes.size() && character(position)==10)
                position+=stride;
        }
        Bytes raw(bytes.begin()+begin,bytes.begin()+position);
        auto text=decode(raw,doc.encoding);
        if(text.ends_with(L"\r\n")){
            if(doc.lines.empty())doc.eol=L"\r\n";
        }else if(text.ends_with(L"\n")){
            if(doc.lines.empty())doc.eol=L"\n";
        }
        doc.lines.push_back({std::move(text),std::move(raw),false});
    }
    return doc;
}
std::wstring body(std::wstring s){
    while(!s.empty() && (s.back()==L'\r'||s.back()==L'\n'))s.pop_back();
    return s;
}
std::wstring newline(const std::wstring& s){
    const auto n=s.find_first_of(L"\r\n");
    return n==s.npos?L"":s.substr(n);
}
void replace_line(Document& doc,std::size_t at,std::wstring text) {
    doc.lines[at].text=std::move(text);
    doc.lines[at].raw=encode(doc.lines[at].text,doc.encoding);
}
void insert_line(Document& doc,std::size_t at,std::wstring text) {
    doc.lines.insert(doc.lines.begin()+static_cast<std::ptrdiff_t>(at),
                     {text,encode(text,doc.encoding),false});
}
Bytes assemble(const Document& doc) {
    Bytes out=doc.bom;
    for(const auto& line:doc.lines)
        if(!line.removed)out.insert(out.end(),line.raw.begin(),line.raw.end());
    return out;
}
std::optional<std::wstring> section(std::wstring_view text){
    const auto t=trim(text);
    if(t.size()<3 || t.front()!=L'[' || t.back()!=L']')return std::nullopt;
    return lower(trim(std::wstring_view(t).substr(1,t.size()-2)));
}
std::optional<std::pair<std::wstring,std::wstring>> key_value(std::wstring_view text){
    auto x=trim(text);
    if(x.empty() || x[0]==L';' || x[0]==L'#')return std::nullopt;
    const auto equals=x.find(L'=');
    if(equals==x.npos)return std::nullopt;
    return std::make_pair(lower(trim(std::wstring_view(x).substr(0,equals))),
                          trim(std::wstring_view(x).substr(equals+1)));
}
std::wstring file_value(const fs::path& path) {
    return path.wstring();
}
std::wstring expand(std::wstring text){
    const DWORD n=ExpandEnvironmentStringsW(text.c_str(),nullptr,0);
    if(!n||n>32768u)return text;
    std::wstring result(n,L'\0');
    const DWORD count=ExpandEnvironmentStringsW(text.c_str(),result.data(),n);
    if(!count||count>n)return text;
    result.resize(count-1);
    return result;
}
bool own_entry(std::wstring_view value,const fs::path& installed){
    const auto comma=value.find(L',');
    if(comma==value.npos)return false;
    auto path=fs::path(expand(trim(value.substr(comma+1))));
    if(path.empty())return false;
    if(same(path.lexically_normal().wstring(),installed.lexically_normal().wstring()))
        return true;
    // Permit migration from an older location for the same ISO-WCX product,
    // but never take over an unrelated WCX or a separate legacy ISO.WCX.
    return same(path.filename().wstring(),L"ISO-WCX.wcx") &&
           same(path.parent_path().filename().wstring(),L"ISO-WCX");
}
bool legacy_iso_entry(std::wstring_view value){
    const auto comma=value.find(L',');
    if(comma==value.npos)return false;
    const auto path=fs::path(expand(trim(value.substr(comma+1))));
    // A different product must never be claimed merely because it handles ISO.
    // Only the familiar legacy ISO\iso.wcx plugin is eligible, and only
    // after a second, explicit operator confirmation in Configure.
    return !path.empty() && same(path.filename().wstring(),L"iso.wcx") &&
           same(path.parent_path().filename().wstring(),L"iso");
}
bool has_tc_process(){
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snapshot==INVALID_HANDLE_VALUE)fail("Cannot examine running Total Commander processes");
    PROCESSENTRY32W p{};p.dwSize=sizeof(p);
    bool found=false;
    if(!Process32FirstW(snapshot,&p)){CloseHandle(snapshot);fail("Cannot enumerate Total Commander processes");}
    do{
        if(_wcsicmp(p.szExeFile,L"TOTALCMD.EXE") &&
           _wcsicmp(p.szExeFile,L"TOTALCMD64.EXE"))continue;
        found=true;break;
    }while(Process32NextW(snapshot,&p));
    CloseHandle(snapshot);
    return found;
}
Plan calculate(const fs::path& ini,const fs::path& plugin32,
               bool replace_legacy_iso=false){
    if(!safe_file(plugin32))fail("Installed ISO-WCX.wcx was not found");
    if(!safe_file(plugin32.parent_path()/L"ISO-WCX.wcx64"))
        fail("Companion ISO-WCX.wcx64 was not found");
    Plan plan;
    plan.visible.ini=ini;
    plan.original=read_bytes(ini);
    plan.doc=parse(plan.original);
    auto& lines=plan.doc.lines;
    std::size_t begin=lines.size(),end=lines.size();
    std::wstring current;
    unsigned sections=0;
    bool cache_redirect=false;
    bool pack_redirect=false;
    for(std::size_t i=0;i<lines.size();++i){
        const auto raw=body(lines[i].text);
        if(auto heading=section(raw)){
            if(current==L"packerplugins" && end==lines.size())end=i;
            current=*heading;
            if(current==L"packerplugins"){++sections;begin=i+1;}
            continue;
        }
        if(auto kv=key_value(raw)) {
            if(kv->first==L"redirectsection" && kv->second!=L"0" && !kv->second.empty()){
                if(current==L"packerplugins")pack_redirect=true;
                if(current==L"packerplugins64")cache_redirect=true;
            }
        }
    }
    if(sections==1 && begin==lines.size() && end==lines.size() && !lines.empty() &&
       newline(lines.back().text).empty()) {
        replace_line(plan.doc,lines.size()-1,lines.back().text+plan.doc.eol);
    }
    if(sections>1)fail("Duplicate [PackerPlugins] sections; manual resolution needed");
    if(pack_redirect || cache_redirect)
        fail("PackerPlugins or PackerPlugins64 is redirected; cannot safely edit multiple INIs");
    if(sections==1 && end==lines.size())end=lines.size();
    if(sections==0) {
        // Support a valid INI without an existing packer section.
        if(!lines.empty() && newline(lines.back().text).empty())
            replace_line(plan.doc,lines.size()-1,lines.back().text+plan.doc.eol);
        insert_line(plan.doc,lines.size(),L"[PackerPlugins]"+plan.doc.eol);
        begin=lines.size();end=lines.size();
    }
    struct Target {const wchar_t* key;unsigned caps;};
    constexpr std::array<Target,5> targets{{
        {L"wim",159},{L"iso",159},{L"udf",159},{L"esd",192},{L"swm",192}
    }};
    // Read all original rows and reject duplicate keys before any mutation.
    struct Found {bool present=false;std::size_t index=0;std::wstring value;};
    std::array<Found,5> found{};
    for(std::size_t i=begin;i<end;++i){
        const auto kv=key_value(body(lines[i].text));
        if(!kv)continue;
        for(std::size_t j=0;j<targets.size();++j){
            if(kv->first!=targets[j].key)continue;
            if(found[j].present)fail("Duplicate WCX extension key in PackerPlugins");
            found[j]={true,i,kv->second};
        }
    }
    std::vector<std::wstring> additions;
    for(std::size_t j=0;j<targets.size();++j) {
        const auto desired=std::to_wstring(targets[j].caps)+L","+file_value(plugin32);
        const auto name=std::wstring(targets[j].key);
        if(!found[j].present) {
            additions.push_back(name+L"="+desired+plan.doc.eol);
            plan.visible.summary+=L"ADD "+name+L"="+std::to_wstring(targets[j].caps)+L"\n";
            ++plan.visible.changes;
            continue;
        }
        const bool owned=own_entry(found[j].value,plugin32);
        const bool legacy_iso=name==L"iso" && legacy_iso_entry(found[j].value);
        if(legacy_iso)plan.visible.legacy_iso_available=true;
        if(!owned && !(replace_legacy_iso && legacy_iso)) {
            ++plan.visible.conflicts;
            plan.visible.summary+=L"SKIP "+name+L": currently assigned to another plugin\n";
            continue;
        }
        if(same(trim(found[j].value),desired)) {
            plan.visible.summary+=L"OK   "+name+L"="+std::to_wstring(targets[j].caps)+L"\n";
            continue;
        }
        auto raw=body(lines[found[j].index].text);
        const auto eq=raw.find(L'=');
        const auto prefix=raw.substr(0,eq+1);
        const auto eol=newline(lines[found[j].index].text);
        replace_line(plan.doc,found[j].index,prefix+desired+eol);
        plan.visible.summary+=(legacy_iso && !owned ? L"REPLACE legacy " : L"FIX ")+
            name+L" -> "+std::to_wstring(targets[j].caps)+L"\n";
        ++plan.visible.changes;
    }
    if(!additions.empty()) {
        // Inserting after the existing header avoids modifying unrelated rows.
        for(std::size_t i=additions.size();i>0;--i)
            insert_line(plan.doc,begin,additions[i-1]);
    }
    if(plan.visible.changes) {
        // PackerPlugins64 is a derived TC cache. Delete only its checksum,
        // so TC recomputes the extension list when restarted.
        bool in_cache=false;
        for(auto& line:plan.doc.lines){
            if(auto heading=section(body(line.text))) {
                in_cache=*heading==L"packerplugins64";
                continue;
            }
            if(in_cache) {
                const auto kv=key_value(body(line.text));
                if(kv && kv->first==L"$checksum$")line.removed=true;
            }
        }
    }
    if(!plan.visible.changes)
        plan.visible.summary+=L"No changes required.\n";
    if(plan.visible.conflicts)
        plan.visible.summary+=L"Other WCX registrations are preserved.\n";
    return plan;
}
void write_bytes(HANDLE file,const Bytes& data){
    std::size_t at=0;
    while(at<data.size()){
        const DWORD length=static_cast<DWORD>(std::min<std::size_t>(data.size()-at,1024u*1024u));
        DWORD done=0;
        if(!WriteFile(file,data.data()+at,length,&done,nullptr) || done!=length)
            fail("Cannot write staged Total Commander INI");
        at+=done;
    }
}
void commit(const Plan& plan){
    const auto newer=assemble(plan.doc);
    if(newer==plan.original)return;
    if(read_bytes(plan.visible.ini)!=plan.original)
        fail("wincmd.ini changed since the preview; retry");
    const auto salt=std::to_wstring(GetCurrentProcessId())+L"-"+
                    std::to_wstring(GetTickCount64());
    auto stage=plan.visible.ini;
    stage+=L".iso-wcx-"+salt+L".tmp";
    auto backup=plan.visible.ini;
    backup+=L".iso-wcx-"+salt+L".bak";
    HANDLE file=CreateFileW(stage.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)fail("Cannot create staged INI beside original");
    try{
        write_bytes(file,newer);
        if(!FlushFileBuffers(file))fail("Cannot flush staged INI");
        CloseHandle(file);file=INVALID_HANDLE_VALUE;
        if(has_tc_process())fail("Total Commander is running; no INI changes allowed");
        if(read_bytes(plan.visible.ini)!=plan.original)
            fail("wincmd.ini changed during repair; retry");
        if(!ReplaceFileW(plan.visible.ini.c_str(),stage.c_str(),backup.c_str(),0,nullptr,nullptr))
            fail("Cannot atomically replace Total Commander INI");
    }catch(...){
        if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
        DeleteFileW(stage.c_str());
        throw;
    }
}
fs::path active_ini(){
    auto read_env=[](const wchar_t* key)->std::wstring{
        const DWORD n=GetEnvironmentVariableW(key,nullptr,0);
        if(!n||n>32768)return {};
        std::wstring value(n,L'\0');
        const DWORD len=GetEnvironmentVariableW(key,value.data(),n);
        if(!len||len>=n)return {};
        value.resize(len);return value;
    };
    auto ini=read_env(L"COMMANDER_INI");
    if(ini.empty()){
        std::array<wchar_t,32768> reg{};
        DWORD count=static_cast<DWORD>(reg.size()*sizeof(wchar_t));
        for(HKEY hive:{HKEY_CURRENT_USER,HKEY_LOCAL_MACHINE}){
            count=static_cast<DWORD>(reg.size()*sizeof(wchar_t));
            if(RegGetValueW(hive,L"Software\\Ghisler\\Total Commander",
                L"IniFileName",RRF_RT_REG_SZ|RRF_RT_REG_EXPAND_SZ,
                nullptr,reg.data(),&count)==ERROR_SUCCESS){
                ini=reg.data();break;
            }
        }
    }
    if(ini.empty()){
        auto appdata=read_env(L"APPDATA");
        if(!appdata.empty())ini=(fs::path(appdata)/L"GHISLER"/L"wincmd.ini").wstring();
    }
    if(ini.empty())fail("Active wincmd.ini not found");
    return fs::path(expand(ini)).lexically_normal();
}
fs::path plugin_path(HINSTANCE plugin) {
    std::array<wchar_t,32768> buffer{};
    const auto n=GetModuleFileNameW(plugin,buffer.data(),static_cast<DWORD>(buffer.size()));
    if(!n||n>=buffer.size())fail("Cannot locate active ISO-WCX module");
    const auto folder=fs::path(std::wstring(buffer.data(),n)).parent_path();
    return folder/L"ISO-WCX.wcx";
}
fs::path module_path(HINSTANCE plugin) {
    std::array<wchar_t,32768> buffer{};
    const auto n=GetModuleFileNameW(plugin,buffer.data(),static_cast<DWORD>(buffer.size()));
    if(!n||n>=buffer.size())fail("Cannot locate active WCX binary");
    return fs::path(std::wstring(buffer.data(),n));
}
void report_worker(std::wstring status){
    wchar_t data[32768]{};
    const auto size=GetEnvironmentVariableW(L"LOCALAPPDATA",data,32768);
    if(size>0 && size<32768){
        try {
            const fs::path destination=fs::path(data)/L"ISO-WCX"/L"Reports"/
                                        L"ISO-WCX-Association-Fix.txt";
            fs::create_directories(destination.parent_path());
            std::wofstream out(destination,std::ios::trunc);
            if(out)out<<status;
        }catch(...){}
    }
    modern_dialog::show(nullptr,status.c_str(),L"ISO-WCX - Association Fix",MB_OK|
                (status.starts_with(L"SUCCESS")?MB_ICONINFORMATION:MB_ICONWARNING));
}
} // namespace

AssociationFixPlan preview_association_fix(
        const fs::path& ini,const fs::path& plugin32,
        bool replace_legacy_iso){
    try {return calculate(ini,plugin32,replace_legacy_iso).visible;}
    catch(const std::exception& e){
        AssociationFixPlan plan;plan.ini=ini;
        std::wstring message;
        const char* p=e.what();
        while(*p)message.push_back(static_cast<unsigned char>(*p++));
        plan.summary=L"Cannot repair associations: "+message+L"\n";
        plan.conflicts=1;
        return plan;
    }
}
bool apply_association_fix_offline(
        const fs::path& ini,const fs::path& plugin32,
        std::wstring& summary,bool replace_legacy_iso){
    try{
        if(has_tc_process())fail("Close all Total Commander windows first");
        const auto plan=calculate(ini,plugin32,replace_legacy_iso);
        summary=plan.visible.summary;
        if(plan.visible.changes)commit(plan);
        summary=L"SUCCESS: association repair completed.\n"+summary+
                L"Restart Total Commander and recheck registration.";
        return true;
    }catch(const std::exception& e){
        summary=L"ERROR: ";const char* p=e.what();
        while(*p)summary.push_back(static_cast<unsigned char>(*p++));
        return false;
    }
}
bool offer_association_fix(HWND parent,HINSTANCE plugin,bool russian,
                           std::wstring& status){
    try{
        const auto ini=active_ini();
        const auto binary=plugin_path(plugin);
        const auto initial=calculate(ini,binary).visible;
        bool replace_legacy_iso=false;
        if(initial.legacy_iso_available){
            const auto question=russian?
                L".iso уже связано со старым ISO\\iso.wcx.\n"
                L"Заменить ТОЛЬКО эту привязку на ISO-WCX?\n"
                L"Остальные плагины не изменятся.":
                L".iso is assigned to the older ISO\\iso.wcx plugin.\n"
                L"Replace ONLY this .iso association with ISO-WCX?\n"
                L"Other plugins will remain unchanged.";
            replace_legacy_iso=modern_dialog::confirm(parent,modern_dialog::Tone::warning,
                L"ISO-WCX - Legacy ISO Association",
                russian?L"Заменить старую привязку ISO?":L"Replace legacy ISO association?",
                question,
                russian?L"Заменить только ISO":L"Replace ISO only",
                russian?L"Оставить старую":L"Keep existing",
                russian?L"Остальные плагины не изменяются.":
                        L"Other WCX plugins remain unchanged.");
        }
        const auto plan=calculate(ini,binary,replace_legacy_iso).visible;
        if(!plan.changes){
            status=plan.conflicts?L"No safe automatic changes; see conflicts.":
                                   L"All ISO-WCX associations are already correct.";
            modern_dialog::show(parent,(plan.summary+L"\n"+status).c_str(),
                        L"ISO-WCX - Associations",MB_OK|MB_ICONINFORMATION);
            return false;
        }
        const auto prompt=plan.summary+
            (russian?L"\nПрименить исправления после полного закрытия Total Commander?\n"
                     L"Будет создана резервная копия wincmd.ini.":
                     L"\nApply these changes after ALL Total Commander windows close?\n"
                     L"A backup of wincmd.ini will be created.");
        if(!modern_dialog::confirm(parent,modern_dialog::Tone::question,
            L"ISO-WCX - Fix Associations",
            russian?L"Применить исправления WCX?":L"Apply WCX association repair?",
            prompt,
            russian?L"Запланировать исправление":L"Schedule repair",
            russian?L"Отмена":L"Cancel",
            russian?L"После закрытия TC, с резервной копией INI.":
                    L"After TC exits, with an INI backup.")){
            status=L"Association fix cancelled.";return false;
        }
        const auto source=module_path(plugin);
        std::array<wchar_t,32768> sys{};
        const auto n=GetSystemDirectoryW(sys.data(),static_cast<UINT>(sys.size()));
        if(!n||n>=sys.size())fail("Cannot locate Windows rundll32.exe");
        const fs::path runner=fs::path(sys.data())/L"rundll32.exe";
        // Command paths are quoted, validated OS filesystem paths. The Win32
        // rundll32 process will load the 32-bit WCX, x64 the 64-bit WCX.
        const std::wstring command=L"\""+runner.wstring()+L"\" \""+
            source.wstring()+L"\",IsoWcxAssociationFixWorkerW \""+
            ini.wstring()+L"\" \""+binary.wstring()+L"\""+
            (replace_legacy_iso?L" --legacy-iso":L"");
        std::vector<wchar_t> cmd(command.begin(),command.end());cmd.push_back(0);
        STARTUPINFOW si{};si.cb=sizeof(si);
        PROCESS_INFORMATION pi{};
        if(!CreateProcessW(runner.c_str(),cmd.data(),nullptr,nullptr,FALSE,
                CREATE_NEW_PROCESS_GROUP,nullptr,nullptr,&si,&pi))
            fail("Cannot start built-in Windows association repair worker");
        CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
        status=russian?L"Запланировано. Закройте все окна Total Commander."
                      :L"Fix scheduled. Close all Total Commander windows.";
        return true;
    }catch(const std::exception& e){
        status=L"Association fix unavailable: ";
        const char* p=e.what();
        while(*p)status.push_back(static_cast<unsigned char>(*p++));
        modern_dialog::show(parent,status.c_str(),L"ISO-WCX - Associations",MB_OK|MB_ICONWARNING);
        return false;
    }
}
void run_association_fix_worker(HINSTANCE module, std::wstring_view arguments) noexcept {
    std::wstring status;
    try{
        std::wstring command(arguments);
        int argc=0;
        LPWSTR* argv=CommandLineToArgvW(command.c_str(),&argc);
        if(!argv || (argc!=2 && argc!=3) ||
           (argc==3 && std::wstring_view(argv[2])!=L"--legacy-iso")){
            if(argv)LocalFree(argv);
            fail("Invalid association worker arguments");
        }
        const bool replace_legacy_iso=argc==3;
        const fs::path ini(argv[0]),binary(argv[1]);
        LocalFree(argv);
        if(!safe_file(binary) || !safe_file(binary.parent_path()/L"ISO-WCX.wcx64"))
            fail("Installed ISO-WCX plugin was removed before repair");
        // The worker must come from the exact installed WCX directory that
        // was approved in Configure. Do not accept an arbitrary user-provided
        // DLL path or a stale copy unpacked somewhere else. This check works
        // for both x86 (ISO-WCX.wcx) and x64 (ISO-WCX.wcx64) rundll32 hosts.
        if(!module || !fs::equivalent(binary,plugin_path(module)))
            fail("Association worker does not match the active ISO-WCX installation");
        if(!safe_file(ini) || !fs::equivalent(ini,active_ini()))
            fail("Total Commander changed its active INI; repair cancelled");
        // TC owns its INI while running. Never mutate until the host has
        // actually exited. Time out without side effects if it stays open.
        for(unsigned elapsed=0;elapsed<600;++elapsed){
            if(!has_tc_process())break;
            Sleep(1000);
        }
        if(has_tc_process())fail("Total Commander remained open; fix was not applied");
        if(!apply_association_fix_offline(ini,binary,status,replace_legacy_iso))
            report_worker(status);
        else
            report_worker(status);
    }catch(const std::exception& e){
        status=L"ERROR: ";const char* p=e.what();
        while(*p)status.push_back(static_cast<unsigned char>(*p++));
        report_worker(status);
    }catch(...){report_worker(L"ERROR: Unexpected association repair failure");}
}
} // namespace iso_wcx
#endif
