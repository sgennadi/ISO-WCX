// Built-in Total Commander WCX association migration (no installer EXE).
// Repair only ISO-WCX's own records and never overwrite an unrelated WCX.
// User confirmation, backups, source-identity checks, and a TC restart are
// required because pluginst.inf has no uninstall/cleanup operation.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "iso_wcx/tc_registration_repair.hpp"
#include "iso_wcx/wcx_updater.hpp"
#include "iso_wcx/edit_diagnostics.hpp"
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <cstdint>
#include <climits>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace iso_wcx {
namespace {
namespace fs=std::filesystem;
using namespace tc_registration;
enum class Coding { Utf8Bom,Utf8,Ansi,Utf16LE };
struct Document {
    fs::path path;
    std::string bytes;
    std::wstring text;
    Coding coding{};
};
void check(bool ok,const char* message) {
    if(!ok)throw std::runtime_error(message);
}
std::wstring environment(const wchar_t* name) {
    DWORD length=GetEnvironmentVariableW(name,nullptr,0);
    if(!length || length>32768)return {};
    std::wstring s(length,L'\0');
    const DWORD n=GetEnvironmentVariableW(name,s.data(),length);
    if(!n || n>=length)return {};
    s.resize(n);
    return s;
}
std::wstring expand(std::wstring_view source) {
    if(source.empty())return {};
    std::wstring argument(source);
    const DWORD length=ExpandEnvironmentStringsW(argument.c_str(),nullptr,0);
    check(length>0 && length<=32768,"Cannot expand Total Commander path");
    std::wstring s(length,L'\0');
    const DWORD n=ExpandEnvironmentStringsW(argument.c_str(),s.data(),length);
    check(n>0 && n<=length,"Cannot expand Total Commander path");
    s.resize(n-1);
    return s;
}
std::wstring registry_ini() {
    DWORD bytes=0;
    constexpr wchar_t key[]=L"Software\\Ghisler\\Total Commander";
    const DWORD flags=RRF_RT_REG_SZ|RRF_RT_REG_EXPAND_SZ;
    if(RegGetValueW(HKEY_CURRENT_USER,key,L"IniFileName",flags,
                    nullptr,nullptr,&bytes)!=ERROR_SUCCESS ||
       bytes<sizeof(wchar_t) || bytes>65536)return {};
    std::wstring s(bytes/sizeof(wchar_t)+1,L'\0');
    if(RegGetValueW(HKEY_CURRENT_USER,key,L"IniFileName",flags,
                    nullptr,s.data(),&bytes)!=ERROR_SUCCESS)return {};
    s.resize(std::wcslen(s.c_str()));
    return expand(s);
}
bool regular_ini(const fs::path& path) {
    const auto attr=GetFileAttributesW(path.c_str());
    return attr!=INVALID_FILE_ATTRIBUTES &&
           !(attr&FILE_ATTRIBUTE_DIRECTORY) &&
           !(attr&FILE_ATTRIBUTE_REPARSE_POINT);
}
fs::path normalized(fs::path path,const fs::path& base) {
    if(path.is_relative())path=base/path;
    return fs::absolute(path).lexically_normal();
}
// Read the running host's own command line (not a new shell or the
// registry). Windows CommandLineToArgvW handles quoted /i= arguments.
std::wstring running_tc_ini_override() {
    int argc=0;
    LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(!argv || argc<=0) {
        if(argv)LocalFree(argv);
        return {};
    }
    std::vector<std::wstring> words;
    try {
        words.reserve(static_cast<std::size_t>(argc));
        for(int i=0;i<argc;++i)words.emplace_back(argv[i]);
    }catch(...) {
        LocalFree(argv);
        throw;
    }
    LocalFree(argv);
    return command_line_ini_override(words);
}
fs::path discover_ini(const fs::path& tc_dir) {
    const auto override=running_tc_ini_override();
    if(!override.empty()) {
        const auto path=normalized(fs::path(expand(override)),tc_dir);
        check(regular_ini(path),
              "TC /i= overrides the registry but names a missing/unsafe INI");
        edit_diagnostics::log(L"TC_ACTIVE_WCX_INI_SOURCE=/i",path);
        return path;
    }
    const auto env=environment(L"COMMANDER_INI");
    if(!env.empty()) {
        const auto path=normalized(fs::path(expand(env)),tc_dir);
        check(regular_ini(path),"COMMANDER_INI is not a regular configuration file");
        return path;
    }
    const auto portable=tc_dir/L"wincmd.ini";
    if(regular_ini(portable) &&
       GetPrivateProfileIntW(L"Configuration",L"UseIniInProgramDir",0,
                             portable.c_str())==7)return portable;
    const auto reg=registry_ini();
    if(!reg.empty()) {
        const auto path=normalized(fs::path(reg),tc_dir);
        check(regular_ini(path),"Registered Total Commander INI does not exist");
        return path;
    }
    const auto appdata=environment(L"APPDATA");
    check(!appdata.empty(),"Cannot discover current Total Commander INI");
    const auto path=fs::path(appdata)/L"Ghisler"/L"wincmd.ini";
    check(regular_ini(path),"wincmd.ini not found; configure TC profile first");
    return path;
}
std::wstring decode(const char* bytes,std::size_t count,UINT codepage,DWORD flags) {
    if(!count)return {};
    check(count<static_cast<std::size_t>(INT_MAX),"INI is too large");
    const int n=MultiByteToWideChar(codepage,flags,bytes,static_cast<int>(count),
                                   nullptr,0);
    check(n>0,"INI decoding failed");
    std::wstring result(n,L'\0');
    check(MultiByteToWideChar(codepage,flags,bytes,static_cast<int>(count),
                              result.data(),n)==n,"INI decoding failed");
    return result;
}
std::string encode(std::wstring_view input,UINT codepage,DWORD flags,
                   bool exact_ansi=false) {
    if(input.empty())return {};
    check(input.size()<static_cast<std::size_t>(INT_MAX),"INI is too large");
    BOOL substituted=FALSE;
    BOOL* flag=exact_ansi?&substituted:nullptr;
    const int length=WideCharToMultiByte(
        codepage,flags,input.data(),static_cast<int>(input.size()),
        nullptr,0,nullptr,flag);
    check(length>0,"INI encoding failed");
    std::string out(length,'\0');
    check(WideCharToMultiByte(codepage,flags,input.data(),
              static_cast<int>(input.size()),out.data(),length,nullptr,
              flag)==length && !substituted,
          "INI contains a path that cannot be encoded without data loss");
    return out;
}
std::string encoded(const std::wstring& text,Coding coding) {
    if(coding==Coding::Utf16LE) {
        static_assert(sizeof(wchar_t)==2);
        std::string out("\xff\xfe",2);
        out.append(reinterpret_cast<const char*>(text.data()),
                   text.size()*sizeof(wchar_t));
        return out;
    }
    const UINT cp=coding==Coding::Ansi?CP_ACP:CP_UTF8;
    auto body=encode(text,cp,coding==Coding::Ansi?WC_NO_BEST_FIT_CHARS:
                     WC_ERR_INVALID_CHARS,coding==Coding::Ansi);
    if(coding==Coding::Utf8Bom)body.insert(0,"\xef\xbb\xbf",3);
    return body;
}
Document load(const fs::path& path) {
    check(regular_ini(path),"Refusing to modify missing or reparse-point INI");
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    check(bool(input),"Cannot read Total Commander INI");
    const auto n=input.tellg();
    check(n>=0 && n<=16*1024*1024,"INI exceeds 16 MiB safe-edit limit");
    input.seekg(0);
    std::string bytes(static_cast<std::size_t>(n),'\0');
    if(!bytes.empty())input.read(bytes.data(),static_cast<std::streamsize>(n));
    check(bool(input) || (n==0),"Cannot read complete Total Commander INI");
    Document document{path,bytes,{},Coding::Utf8};
    if(bytes.starts_with("\xff\xfe")) {
        check((bytes.size()%2)==0,"Odd-length UTF-16 Total Commander INI");
        document.coding=Coding::Utf16LE;
        document.text.resize((bytes.size()-2)/2);
        if(bytes.size()>2)
            std::memcpy(document.text.data(),bytes.data()+2,bytes.size()-2);
    } else if(bytes.starts_with("\xfe\xff")) {
        throw std::runtime_error("UTF-16BE INI requires manual repair; no changes made");
    } else {
        const bool bom=bytes.starts_with("\xef\xbb\xbf");
        const auto offset=bom?3u:0u;
        document.coding=bom?Coding::Utf8Bom:Coding::Utf8;
        if(bytes.size()>offset) {
            auto utf8=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,
                bytes.data()+offset,static_cast<int>(bytes.size()-offset),
                nullptr,0);
            if(utf8>0)
                document.text=decode(bytes.data()+offset,bytes.size()-offset,
                                     CP_UTF8,MB_ERR_INVALID_CHARS);
            else {
                check(!bom,"Invalid UTF-8 BOM INI; no changes made");
                document.coding=Coding::Ansi;
                document.text=decode(bytes.data(),bytes.size(),CP_ACP,0);
            }
        }
    }
    check(document.text.find(L'\0')==std::wstring::npos,
          "Embedded NUL in Total Commander INI; no changes made");
    check(encoded(document.text,document.coding)==document.bytes,
          "INI encoding is not lossless; refusing repair");
    return document;
}
fs::path redirect(const Document& primary,
                  std::wstring_view name) {
    auto choice=trim(value(primary.text,name,L"RedirectSection"));
    if(choice.empty() || choice==L"0")return primary.path;
    if(choice==L"1")
        choice=trim(value(primary.text,L"Configuration",L"AlternateUserIni"));
    check(!choice.empty(),"RedirectSection has no target");
    const auto path=normalized(fs::path(expand(choice)),primary.path.parent_path());
    check(regular_ini(path),"Redirected PackerPlugins INI is missing or unsafe");
    return path;
}
fs::path module_path(HINSTANCE module) {
    std::wstring buffer(32768,L'\0');
    const auto n=GetModuleFileNameW(module,buffer.data(),
                                   static_cast<DWORD>(buffer.size()));
    check(n>0 && n<buffer.size(),"Cannot locate loaded ISO-WCX module");
    buffer.resize(n);
    auto path=fs::path(buffer);
    check(fold(path.filename().wstring())==L"iso-wcx.wcx" ||
          fold(path.filename().wstring())==L"iso-wcx.wcx64",
          "Loaded WCX filename does not match ISO-WCX");
    path.replace_extension(L".wcx");
    check(fs::is_regular_file(path) &&
          fs::is_regular_file(path.parent_path()/L"ISO-WCX.wcx64"),
          "Both 32-bit WCX and 64-bit WCX64 must be installed");
    return path;
}
// A custom TC PluginBaseDir can be outside the Total Commander install tree.
// Derive the portable/INI base from the running HOST, never from the WCX DLL
// location. Refuse to edit TC's configuration from a foreign WCX host.
fs::path running_totalcmd_directory() {
    std::wstring executable(32768,L'\0');
    const auto length=GetModuleFileNameW(
        nullptr,executable.data(),static_cast<DWORD>(executable.size()));
    check(length>0 && length<executable.size(),
          "Cannot locate running Total Commander");
    executable.resize(length);
    const fs::path path(executable);
    const auto name=fold(path.filename().wstring());
    check(name==L"totalcmd.exe" || name==L"totalcmd64.exe",
          "WCX association repair requires a Total Commander process");
    return path.parent_path();
}
std::string backup_suffix() {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char buffer[90]{};
    sprintf_s(buffer,".iso-wcx-%04u%02u%02u-%02u%02u%02u-%lu.bak",
       t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,
       static_cast<unsigned long>(GetCurrentProcessId()));
    return buffer;
}
fs::path save_backup(const fs::path& path) {
    const std::string suffix=backup_suffix();
    auto backup=path;
    backup+=fs::path(suffix);
    check(CopyFileW(path.c_str(),backup.c_str(),TRUE)!=FALSE,
          "Cannot create a unique original INI backup");
    return backup;
}
void store(const Document& before,const std::wstring& new_text) {
    check(load(before.path).bytes==before.bytes,
          "Total Commander changed INI while repairing; no overwrite");
    auto after=encoded(new_text,before.coding);
    auto temp=before.path;
    temp+=L".iso-wcx-update-"+std::to_wstring(GetCurrentProcessId())+
          L"-"+std::to_wstring(GetTickCount64());
    HANDLE handle=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,
                 CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    check(handle!=INVALID_HANDLE_VALUE,"Cannot create temporary INI");
    bool ok=true;
    for(std::size_t at=0;at<after.size();) {
        const DWORD length=static_cast<DWORD>(
            std::min<std::size_t>(after.size()-at,1024u*1024u));
        DWORD written=0;
        if(!WriteFile(handle,after.data()+at,length,&written,nullptr) ||
           written!=length){ok=false;break;}
        at+=written;
    }
    if(!FlushFileBuffers(handle))ok=false;
    CloseHandle(handle);
    if(!ok){DeleteFileW(temp.c_str());throw std::runtime_error("INI staged write failed");}
    // ReplaceFileW preserves original NTFS metadata/ADS; backup is separate.
    if(!ReplaceFileW(before.path.c_str(),temp.c_str(),nullptr,0,nullptr,nullptr)){
        DeleteFileW(temp.c_str());
        throw std::runtime_error("Total Commander INI is locked; no repair applied");
    }
}
const wchar_t* label(bool ru,const wchar_t* en,const wchar_t* russian) {
    return ru?russian:en;
}
} // namespace

void repair_tc_registrations(HWND parent,HINSTANCE instance,bool russian,
                             bool quiet_when_clean) {
    try {
        const auto dll=module_path(instance);
        const auto tc_dir=running_totalcmd_directory();
        const auto primary=load(discover_ini(tc_dir));
        const auto assoc_path=redirect(primary,L"PackerPlugins");
        const auto cache_path=redirect(primary,L"PackerPlugins64");
        const auto assoc=assoc_path==primary.path?primary:load(assoc_path);
        const auto fixed=repair_packer_plugins(assoc.text,dll.wstring());
        // The INF registers only ISO and WIM to prevent 23 native
        // Associate dialogs. Offer the other 21 UNASSIGNED formats in one
        // confirmed operation when an image is first opened. Foreign
        // plugins and any explicit key (even an empty one) win.
        const auto added=register_missing_packer_plugins(
            fixed.text,dll.wstring());
        const bool registrations_changed=fixed.changed || added.changed;
        edit_diagnostics::log(
            L"WCX association inspection",{},
            L"fix="+std::to_wstring(fixed.extensions.size())+
            L" new="+std::to_wstring(added.extensions.size())+
            L" duplicates="+std::to_wstring(fixed.duplicates_removed)+
            L" conflicts="+std::to_wstring(fixed.conflicts.size()));
        std::wstring summary=
            label(russian,L"ISO-WCX registrations: ",L"Регистрации ISO-WCX: ")+
            std::to_wstring(fixed.extensions.size())+
            label(russian,L"\nDuplicate entries removed: ",
                          L"\nУдаляемые дубликаты: ")+
            std::to_wstring(fixed.duplicates_removed);
        if(!fixed.extensions.empty()) {
            summary+=label(russian,L"\nExtensions to repair: ",
                                    L"\nИсправляемые расширения: ");
            for(const auto& ext:fixed.extensions)
                summary+=ext+L" ";
        }
        if(!fixed.conflicts.empty()) {
            summary+=label(russian,L"\nUnchanged (other WCX installed): ",
                            L"\nСохранены привязки других WCX: ");
            for(const auto& ext:fixed.conflicts)summary+=ext+L" ";
        }
        if(added.changed) {
            summary+=label(russian,L"\nMissing image formats to register: ",
                                  L"\nНовые расширения для регистрации: ");
            summary+=std::to_wstring(added.extensions.size())+L"\n";
            for(const auto& ext:added.extensions)summary+=ext+L" ";
        }
        // A 64-bit Total Commander can retain an obsolete PackerPlugins64
        // checksum even though PackerPlugins already reads "159,ISO-WCX.wcx".
        // The old manual Repair button returned early here, so F5 stayed
        // unavailable after installation. Let EXPLICIT manual repair also
        // invalidate that cache, without changing any other WCX association.
        // On passive open keep the existing silent fast path: don't offer
        // a cache reset every time TC recreates its valid checksum.
        if(!registrations_changed && quiet_when_clean) {
            edit_diagnostics::log(L"WCX association check complete",{},
                                  L"all supported registrations already assigned");
            return;
        }
        Document cached=cache_path==assoc_path?assoc:load(cache_path);
        const auto cache_in_assoc=cache_path==assoc_path?
            invalidate_cache(added.text):Patch{};
        const auto saved_assoc_text=cache_path==assoc_path?
            cache_in_assoc.text:added.text;
        const auto saved_cache=cache_path==assoc_path?
            Patch{}:invalidate_cache(cached.text);
        const bool assoc_changed=registrations_changed ||
            (cache_path==assoc_path && cache_in_assoc.changed);
        const bool cache_changed=cache_path!=assoc_path && saved_cache.changed;
        if(!assoc_changed && !cache_changed) {
            if(quiet_when_clean)return;
            summary+=label(russian,
                L"\nNo obsolete ISO-WCX binding or TC x64 cache was found. Nothing changed.",
                L"\nУстаревших привязок ISO-WCX или кэша TC x64 нет. Изменений нет.");
            MessageBoxW(parent,summary.c_str(),L"ISO-WCX Associations",
                        MB_OK|MB_ICONINFORMATION);
            return;
        }
        if(registrations_changed) {
            summary+=label(russian,
                L"\n\nRepair existing ISO-WCX records and add only "
                L"unassigned supported image formats in ONE operation?\n"
                L"Foreign WCX handlers, toolbar and other settings stay untouched. "
                L"Backup the INI and restart Total Commander afterward.",
                L"\n\nИсправить записи ISO-WCX и привязать только свободные "
                L"расширения образов ОДНИМ действием?\n"
                L"Чужие плагины, кнопки и настройки сохраняются. "
                L"Создаётся копия INI; затем перезапустите Total Commander.");
        }else{
            summary+=label(russian,
                L"\n\nThe ISO-WCX flags match the 159/158 format policy. Reset ONLY the "
                L"TC x64 PackerPlugins64 cache checksum to force Total "
                L"Commander to rebuild its cached capabilities?\n"
                L"An INI backup will be saved; all other plugin bindings "
                L"will remain unchanged. Restart all TC windows afterward.",
                L"\n\nФлаги ISO-WCX соответствуют режиму 159/158. Сбросить ТОЛЬКО "
                L"контрольную сумму кэша PackerPlugins64, чтобы TC "
                L"перечитал возможности плагина?\n"
                L"Создаётся резервная копия INI. Привязки других "
                L"плагинов не меняются. После этого перезапустите все окна TC.");
        }
        edit_diagnostics::log(L"WCX association confirmation requested",{},
                              registrations_changed?L"registration update":
                                                    L"cache-only repair");
        if(MessageBoxW(parent,summary.c_str(),L"ISO-WCX Associations",
                 MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDYES) {
            edit_diagnostics::log(L"WCX association confirmation declined",{});
            return;
        }
        fs::path backup,cache_backup;
        if(assoc_changed)backup=save_backup(assoc.path);
        if(cache_changed)cache_backup=save_backup(cached.path);
        bool assoc_committed=false,cache_committed=false;
        // A user may edit TC settings while the confirmation dialog stays
        // open. Neither INI may be written unless BOTH source copies match.
        check(load(assoc.path).bytes==assoc.bytes,
              "PackerPlugins changed before repair; no INI replaced");
        if(cache_changed)
            check(load(cached.path).bytes==cached.bytes,
                  "PackerPlugins64 changed before repair; no INI replaced");
        try {
            if(assoc_changed) {
                store(assoc,saved_assoc_text);
                assoc_committed=true;
            }
            if(cache_changed) {
                store(cached,saved_cache.text);
                cache_committed=true;
            }
        }catch(...) {
            // Restore only files this action actually wrote, and only if
            // their bytes still match OUR changes (never overwrite user edits).
            std::error_code ignored;
            if(assoc_committed && !backup.empty() &&
               load(assoc.path).bytes==encoded(saved_assoc_text,assoc.coding))
                fs::copy_file(backup,assoc.path,
                    fs::copy_options::overwrite_existing,ignored);
            if(cache_committed && !cache_backup.empty() &&
               load(cached.path).bytes==encoded(saved_cache.text,cached.coding))
                fs::copy_file(cache_backup,cached.path,
                    fs::copy_options::overwrite_existing,ignored);
            throw;
        }
        const auto saved_path=!backup.empty()?backup:cache_backup;
        const auto message=std::wstring(label(russian,
            registrations_changed?L"ISO-WCX image extensions registered/repaired.\nRestart all Total Commander windows.\n\nBackup: ":
                                  L"TC x64 WCX capability cache reset.\nRestart all Total Commander windows.\n\nBackup: ",
            registrations_changed?L"Привязки ISO-WCX зарегистрированы/исправлены.\nПерезапустите все окна Total Commander.\n\nКопия: ":
                                  L"Кэш WCX (TC x64) сброшен.\nПерезапустите все окна Total Commander.\n\nКопия: "))+
            saved_path.wstring();
        edit_diagnostics::log(L"WCX association update committed",{},
                              registrations_changed?L"restart TC to reload masks":
                                                    L"TC64 checksum reset");
        MessageBoxW(parent,message.c_str(),L"ISO-WCX Associations",
                    MB_OK|MB_ICONINFORMATION);
    }catch(const std::exception& e) {
        std::wstring error=L"Association repair was not completed.\n";
        const auto message=std::string(e.what());
        for(unsigned char c:message)error.push_back(static_cast<wchar_t>(c));
        edit_diagnostics::log(L"WCX association inspection failed",{},error);
        if(quiet_when_clean)return; // Do not interrupt normal WIM opening.
        MessageBoxW(parent,error.c_str(),L"ISO-WCX Associations",
                    MB_OK|MB_ICONWARNING);
    }
}
// The standard Total Commander INF cannot remove old WCX associations. On
// the FIRST WIM open after reinstall, silently detect stale ISO-WCX records
// and offer a confirmed repair if needed. Do not slow down subsequent opens
// or show messages in tests, services or unrelated archive hosts.
void offer_tc_association_repair_once() {
    static std::atomic<bool> inspected{false};
    if(inspected.exchange(true,std::memory_order_relaxed))return;
    edit_diagnostics::log(L"WCX association auto-check started",{});
    wchar_t process[MAX_PATH]{};
    const auto n=GetModuleFileNameW(nullptr,process,MAX_PATH);
    if(!n || n>=MAX_PATH) {
        edit_diagnostics::log(L"WCX association check skipped",{},L"host path unavailable");
        return;
    }
    const auto* base=std::wcsrchr(process,L'\\');
    base=base?base+1:process;
    if(_wcsicmp(base,L"TOTALCMD.EXE")!=0 &&
       _wcsicmp(base,L"TOTALCMD64.EXE")!=0)return;
    wchar_t disabled[4]{};
    if(GetEnvironmentVariableW(L"ISO_WCX_DISABLE_ASSOC_REPAIR",
                               disabled,4)==1 && disabled[0]==L'1') {
        edit_diagnostics::log(L"WCX association check skipped",{},L"explicit opt-out");
        return;
    }
    HMODULE dll=nullptr;
    if(!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&offer_tc_association_repair_once),
            &dll)) {
        edit_diagnostics::log(L"WCX association check skipped",{},L"module handle unavailable");
        return;
    }
    const bool ru=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
    repair_tc_registrations(GetActiveWindow(),dll,ru,true);
}
} // namespace iso_wcx
#endif
