// Built-in ISO-WCX update settings for the Total Commander WCX ConfigurePacker ABI.
// Uses only Windows system WinHTTP, BCrypt and ShellExecute. No helper EXE,
// bundled DLL, mount service or write to a loaded WCX image.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "iso_wcx/wcx_updater.hpp"
#include "iso_wcx/image_report.hpp"
#include "iso_wcx/update_policy.hpp"

#include <bcrypt.h>
#include <commdlg.h>
#include <winhttp.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <memory>
#include <type_traits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef ISO_WCX_VERSION_MAJOR
#define ISO_WCX_VERSION_MAJOR 0
#define ISO_WCX_VERSION_MINOR 18
#define ISO_WCX_VERSION_PATCH 0
#endif

namespace iso_wcx {
namespace {
constexpr wchar_t registry_path[] = L"Software\\ISO-WCX\\Updates";
constexpr wchar_t wim_registry_path[] = L"Software\\ISO-WCX\\WIM";
constexpr wchar_t github_api[] =
    L"https://api.github.com/repos/sgennadi/ISO-WCX/releases/latest";
constexpr int id_never=4101, id_check=4102, id_install=4103,
              id_status=4104, id_export=4105, id_wim_fast=4106;
constexpr UINT check_later=WM_APP+191;
constexpr std::size_t max_response=64u*1024u;
constexpr std::size_t max_package=64u*1024u*1024u;
constexpr std::uint64_t seconds_per_day=86400;
constexpr update::Version installed{
    ISO_WCX_VERSION_MAJOR, ISO_WCX_VERSION_MINOR, ISO_WCX_VERSION_PATCH};

using Internet=std::unique_ptr<std::remove_pointer_t<HINTERNET>,
                  decltype(&WinHttpCloseHandle)>;

struct State {
    bool automatic=true;
    bool russian=false;
    bool wim_fast=false;
    std::wstring latest_tag;
    bool newer=false;
};

void require(bool ok, const char* error) {
    if (!ok) throw std::runtime_error(error);
}
std::wstring widen(std::string_view s) {
    std::wstring result;
    result.reserve(s.size());
    for (const unsigned char c: s) {
        require(c >= 32 && c <= 126, "unexpected non-ASCII update metadata");
        result.push_back(static_cast<wchar_t>(c));
    }
    return result;
}
std::string narrow(std::wstring_view value) {
    std::string out;
    out.reserve(value.size());
    for (const auto c: value) {
        require(c >= 32 && c <= 126, "invalid update version");
        out.push_back(static_cast<char>(c));
    }
    return out;
}
std::wstring version_name(update::Version v) {
    return L"v"+std::to_wstring(v.major)+L"."+std::to_wstring(v.minor)+
            L"."+std::to_wstring(v.patch);
}
bool read_dword(const wchar_t* name, DWORD fallback) {
    DWORD v=fallback;
    DWORD size=sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER,registry_path,name,
                     RRF_RT_REG_DWORD,nullptr,&v,&size)!=ERROR_SUCCESS)
        return fallback!=0;
    return v!=0;
}
std::uint64_t read_check_time() {
    std::uint64_t value=0;
    DWORD size=sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER,registry_path,L"LastCheckUnix",
               RRF_RT_REG_QWORD,nullptr,&value,&size)==ERROR_SUCCESS ? value:0;
}
std::uint64_t unix_now() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER count{};
    count.LowPart=ft.dwLowDateTime;
    count.HighPart=ft.dwHighDateTime;
    constexpr std::uint64_t epoch=116444736000000000ULL;
    if (count.QuadPart<epoch) return 0;
    return (count.QuadPart-epoch)/10000000ULL;
}
void write_reg(const wchar_t* name, DWORD type, const void* p, DWORD bytes) {
    HKEY key=nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,registry_path,0,nullptr,0,
           KEY_SET_VALUE,nullptr,&key,nullptr)!=ERROR_SUCCESS) return;
    RegSetValueExW(key,name,0,type,
                   static_cast<const BYTE*>(p),bytes);
    RegCloseKey(key);
}
void save_automatic(bool allow) {
    const DWORD value=allow?1:0;
    write_reg(L"AutoCheck",REG_DWORD,&value,sizeof(value));
}
bool read_wim_fast_setting() {
    DWORD value=0,size=sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER,wim_registry_path,
             L"FastMountFolders",RRF_RT_REG_DWORD,
             nullptr,&value,&size)==ERROR_SUCCESS && value==1;
}
bool save_wim_fast_setting(bool enabled) {
    HKEY key=nullptr;
    const auto opened=RegCreateKeyExW(
         HKEY_CURRENT_USER,wim_registry_path,0,nullptr,0,KEY_SET_VALUE,
         nullptr,&key,nullptr);
    if(opened!=ERROR_SUCCESS)return false;
    const DWORD value=enabled?1u:0u;
    const auto stored=RegSetValueExW(key,L"FastMountFolders",0,REG_DWORD,
                            reinterpret_cast<const BYTE*>(&value),sizeof(value));
    RegCloseKey(key);
    return stored==ERROR_SUCCESS;
}
void save_last_check() {
    const auto time=unix_now();
    write_reg(L"LastCheckUnix",REG_QWORD,&time,sizeof(time));
}
bool due_today() {
    const auto now=unix_now(), last=read_check_time();
    return !last || now<last || now-last>=seconds_per_day;
}
std::vector<std::uint8_t> http_get(const std::wstring& url,
                                    std::size_t maximum) {
    require(url.starts_with(L"https://"),"updater requires HTTPS");
    URL_COMPONENTS u{};
    u.dwStructSize=sizeof(u);
    u.dwSchemeLength=1;
    u.dwHostNameLength=1;
    u.dwUrlPathLength=1;
    u.dwExtraInfoLength=1;
    require(WinHttpCrackUrl(url.c_str(),0,0,&u) &&
            u.nScheme==INTERNET_SCHEME_HTTPS && u.dwHostNameLength,
            "invalid HTTPS release URL");
    const std::wstring host(u.lpszHostName,u.dwHostNameLength);
    std::wstring resource(u.lpszUrlPath,u.dwUrlPathLength);
    if(u.dwExtraInfoLength && u.lpszExtraInfo)
        resource.append(u.lpszExtraInfo,u.dwExtraInfoLength);
    if(resource.empty()) resource=L"/";
    Internet session(WinHttpOpen(L"ISO-WCX-Update/1.0",
                   WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                   WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0),
                   WinHttpCloseHandle);
    require(bool(session),"WinHTTP session unavailable");
    WinHttpSetTimeouts(session.get(),5000,5000,10000,10000);
    // Allow GitHub's HTTPS release-asset redirects, but never downgrade to
    // unencrypted HTTP (including an attacker-controlled redirect chain).
    DWORD policy=WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    require(WinHttpSetOption(session.get(),WINHTTP_OPTION_REDIRECT_POLICY,
              &policy,sizeof(policy))!=FALSE,
            "cannot enforce HTTPS-only update redirects");
    Internet conn(WinHttpConnect(session.get(),host.c_str(),
                  u.nPort,0),WinHttpCloseHandle);
    require(bool(conn),"cannot connect to release host");
    Internet req(WinHttpOpenRequest(conn.get(),L"GET",
                 resource.c_str(),nullptr,WINHTTP_NO_REFERER,
                 WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE),
                 WinHttpCloseHandle);
    require(bool(req),"cannot create release request");
    constexpr wchar_t headers[]=
          L"Accept: application/vnd.github+json\r\n"
          L"User-Agent: ISO-WCX-Native-Updater\r\n";
    require(WinHttpSendRequest(req.get(),headers,
                   static_cast<DWORD>(-1L),
                   WINHTTP_NO_REQUEST_DATA,0,0,0) &&
            WinHttpReceiveResponse(req.get(),nullptr),
            "HTTPS update request failed");
    DWORD status=0;
    DWORD bytes=sizeof(status);
    require(WinHttpQueryHeaders(req.get(),
                   WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
                   WINHTTP_HEADER_NAME_BY_INDEX,&status,&bytes,
                   WINHTTP_NO_HEADER_INDEX) && status==200,
            "GitHub release is unavailable");
    std::vector<std::uint8_t> result;
    for (;;) {
        DWORD available=0;
        require(WinHttpQueryDataAvailable(req.get(),&available)!=FALSE,
                "cannot read release response length");
        if (!available) break;
        require(available<=maximum-result.size(),
                "release download exceeds size limit");
        const auto offset=result.size();
        result.resize(offset+available);
        DWORD read=0;
        require(WinHttpReadData(req.get(),result.data()+offset,
                available,&read)!=FALSE && read>0 && read<=available,
                "truncated GitHub download");
        result.resize(offset+read);
    }
    require(!result.empty(),"empty GitHub release response");
    return result;
}
std::string as_text(const std::vector<std::uint8_t>& data) {
    return {reinterpret_cast<const char*>(data.data()),data.size()};
}
std::string sha256(const std::vector<std::uint8_t>& payload) {
    BCRYPT_ALG_HANDLE algorithm=nullptr;
    BCRYPT_HASH_HANDLE hash=nullptr;
    try {
        require(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,
                nullptr,0)==0,"SHA-256 provider unavailable");
        DWORD length=0,cb=0;
        require(BCryptGetProperty(algorithm,BCRYPT_HASH_LENGTH,
               reinterpret_cast<PUCHAR>(&length),sizeof(length),&cb,0)==0 &&
               length==32,"invalid SHA-256 configuration");
        require(BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)==0,
                "cannot initialize SHA-256");
        require(BCryptHashData(hash,const_cast<PUCHAR>(payload.data()),
               static_cast<ULONG>(payload.size()),0)==0,
               "cannot hash update archive");
        std::array<unsigned char,32> digest{};
        require(BCryptFinishHash(hash,digest.data(),32,0)==0,
                "cannot finish archive SHA-256");
        BCryptDestroyHash(hash);hash=nullptr;
        BCryptCloseAlgorithmProvider(algorithm,0);algorithm=nullptr;
        constexpr char hex[]="0123456789abcdef";
        std::string result;
        result.reserve(64);
        for(const auto c:digest){result.push_back(hex[c>>4]);result.push_back(hex[c&15]);}
        return result;
    } catch(...) {
        if(hash) BCryptDestroyHash(hash);
        if(algorithm) BCryptCloseAlgorithmProvider(algorithm,0);
        throw;
    }
}
std::wstring latest_release() {
    const auto response=http_get(github_api,max_response);
    const auto tag=update::release_tag(as_text(response));
    require(bool(tag),"GitHub release has no valid stable semantic version");
    return widen(*tag);
}
void write_all(HANDLE handle,const std::vector<std::uint8_t>& data) {
    std::size_t at=0;
    while(at<data.size()) {
        DWORD written=0;
        const DWORD amount=static_cast<DWORD>(std::min<std::size_t>(
            data.size()-at,1024u*1024u));
        require(WriteFile(handle,data.data()+at,amount,&written,nullptr) &&
                written==amount,"cannot write downloaded update");
        at+=written;
    }
}
std::filesystem::path save_verified_archive(const std::wstring& tag) {
    require(bool(update::parse_version(narrow(tag))),
            "invalid update version from GitHub");
    const auto filename=L"ISO-WCX-"+tag+L".zip";
    const auto base=L"https://github.com/sgennadi/ISO-WCX/releases/download/"+
                    tag+L"/";
    const auto manifest=http_get(base+L"SHA256SUMS.txt",16384);
    const auto expected=update::checksum(as_text(manifest),narrow(filename));
    require(bool(expected),"SHA256SUMS does not identify release ZIP");
    const auto zip=http_get(base+filename,max_package);
    require(sha256(zip)==*expected,
            "SHA256 verification failed; refusing untrusted archive");

    std::array<wchar_t,32768> temp{};
    const DWORD count=GetTempPathW(static_cast<DWORD>(temp.size()),temp.data());
    require(count>0 && count<temp.size(),"temporary folder unavailable");
    std::filesystem::path folder;
    for(unsigned retry=0;retry<8;++retry) {
        std::array<std::uint8_t,16> token{};
        require(BCryptGenRandom(nullptr,token.data(),
                static_cast<ULONG>(token.size()),
                BCRYPT_USE_SYSTEM_PREFERRED_RNG)==0,
                "cannot create secure update staging path");
        constexpr wchar_t hex[]=L"0123456789abcdef";
        std::wstring nonce;
        for(const auto b:token){nonce+=hex[b>>4];nonce+=hex[b&15];}
        folder=std::filesystem::path(temp.data())/(L"ISO-WCX-"+nonce);
        if(CreateDirectoryW(folder.c_str(),nullptr))break;
        folder.clear();
    }
    require(!folder.empty(),"cannot create update staging directory");
    const auto file=folder/filename;
    HANDLE raw=CreateFileW(file.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL,nullptr);
    if(raw==INVALID_HANDLE_VALUE) {
        RemoveDirectoryW(folder.c_str());
        throw std::runtime_error("cannot create update archive");
    }
    try {
        write_all(raw,zip);
        require(FlushFileBuffers(raw)!=FALSE,"cannot flush update archive");
        CloseHandle(raw);
    }catch(...) {
        CloseHandle(raw);
        DeleteFileW(file.c_str());
        RemoveDirectoryW(folder.c_str());
        throw;
    }
    return file;
}
bool totalcmd_path(std::wstring_view path) {
    const auto slash=path.find_last_of(L"\\/");
    std::wstring name(path.substr(slash==std::wstring_view::npos?0:slash+1));
    std::transform(name.begin(),name.end(),name.begin(),
                   [](wchar_t c){return (c>=L'A' && c<=L'Z')?wchar_t(c+32):c;});
    return name==L"totalcmd.exe" || name==L"totalcmd64.exe";
}
void open_in_totalcmd(HWND parent,const std::filesystem::path& zip) {
    std::array<wchar_t,32768> exe{};
    DWORD n=GetModuleFileNameW(nullptr,exe.data(),static_cast<DWORD>(exe.size()));
    if(n && n<exe.size() && totalcmd_path(std::wstring_view(exe.data(),n))) {
        const auto args=L"/L=\""+zip.wstring()+L"\"";
        const auto rc=reinterpret_cast<std::intptr_t>(ShellExecuteW(
                  parent,L"open",exe.data(),args.c_str(),nullptr,SW_SHOWNORMAL));
        if(rc>32) return;
    }
    // Outside Total Commander: show the verified ZIP's containing directory.
    (void)ShellExecuteW(parent,L"open",zip.parent_path().c_str(),
                        nullptr,nullptr,SW_SHOWNORMAL);
}
bool russian() {
    return PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN;
}
const wchar_t* tr(bool ru,const wchar_t* en,const wchar_t* localized) {
    return ru?localized:en;
}
void put_status(HWND dlg,const std::wstring& text) {
    SetDlgItemTextW(dlg,id_status,text.c_str());
    if(const auto control=GetDlgItem(dlg,id_status))UpdateWindow(control);
}
RECT rect_units(HWND dlg,int x,int y,int w,int h) {
    RECT rect{x,y,x+w,y+h};
    MapDialogRect(dlg,&rect);
    return {rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top};
}
void add_control(HWND dlg,const wchar_t* cls,const wchar_t* name,
                 DWORD style,int id,int x,int y,int w,int h) {
    const auto r=rect_units(dlg,x,y,w,h);
    const auto child=CreateWindowExW(0,cls,name,style|WS_CHILD|WS_VISIBLE,
        r.left,r.top,r.right,r.bottom,dlg,reinterpret_cast<HMENU>(
        static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
    if(child)SendMessageW(child,WM_SETFONT,
                reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);
}
void check_release(HWND dlg,State& state,bool forced) {
    if(!forced && (!state.automatic || !due_today()))return;
    EnableWindow(GetDlgItem(dlg,id_check),FALSE);
    EnableWindow(GetDlgItem(dlg,id_install),FALSE);
    put_status(dlg,tr(state.russian,L"Checking GitHub releases...",
                              L"Проверяем релизы GitHub..."));
    try {
        state.latest_tag=latest_release();
        const auto parsed=update::parse_version(narrow(state.latest_tag));
        state.newer=parsed && update::newer(*parsed,installed);
        save_last_check();
        if(state.newer) {
            put_status(dlg,std::wstring(tr(state.russian,
                L"New verified Stable release: ",L"Доступна новая Stable-версия: "))+
                state.latest_tag);
            EnableWindow(GetDlgItem(dlg,id_install),TRUE);
        } else put_status(dlg,tr(state.russian,
                L"You have the latest Stable version.",
                L"Установлена последняя Stable-версия."));
    }catch(const std::exception&) {
        state.newer=false;
        put_status(dlg,tr(state.russian,
               L"Could not check releases. Try again later.",
               L"Не удалось проверить релизы. Повторите позже."));
    }
    EnableWindow(GetDlgItem(dlg,id_check),TRUE);
}
void download_and_offer(HWND dlg,State& state) {
    if(!state.newer || state.latest_tag.empty())return;
    EnableWindow(GetDlgItem(dlg,id_install),FALSE);
    put_status(dlg,tr(state.russian,
         L"Downloading release and verifying SHA-256...",
         L"Скачивание релиза и проверка SHA-256..."));
    try {
        const auto verified=save_verified_archive(state.latest_tag);
        const auto instructions=std::wstring(tr(state.russian,
             L"SHA-256 verified. The ZIP will open in Total Commander.\n"
             L"Enter the archive and confirm the plugin installer.\n"
             L"Restart Total Commander afterward; the loaded WCX DLL\n"
             L"will NOT be overwritten while it is in use.",
             L"SHA-256 проверен. ZIP откроется в Total Commander.\n"
             L"Откройте архив и подтвердите установку плагина.\n"
             L"Затем перезапустите Total Commander. Загруженная WCX\n"
             L"не заменяется во время работы."));
        MessageBoxW(dlg,instructions.c_str(),L"ISO-WCX Update",
                    MB_OK|MB_ICONINFORMATION);
        open_in_totalcmd(dlg,verified);
        put_status(dlg,tr(state.russian,
             L"Update ZIP downloaded; complete installation in Total Commander.",
             L"ZIP скачан; завершите установку в Total Commander."));
    }catch(const std::exception&) {
        put_status(dlg,tr(state.russian,
             L"Download or SHA-256 verification failed; nothing was installed.",
             L"Ошибка загрузки или SHA-256; ничего не установлено."));
    }
    EnableWindow(GetDlgItem(dlg,id_install),TRUE);
}
// The WCX host calls GetPackerCaps while loading its packer associations.
// User-invoked, in-process metadata export. Both dialogs are owned by
// ConfigurePacker; neither an EXE, service, mount nor helper DLL is needed.
// No image bytes are ever written; only a user-selected TXT destination.
std::wstring from_utf8(std::string_view value) {
    if(value.empty())return {};
    if(value.size()>static_cast<std::size_t>(INT_MAX))
        throw std::runtime_error("text is too long");
    const int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,
                    value.data(),static_cast<int>(value.size()),nullptr,0);
    if(length<=0)throw std::runtime_error("invalid UTF-8");
    std::wstring result(static_cast<std::size_t>(length),L'\0');
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),
            static_cast<int>(value.size()),result.data(),length))
        throw std::runtime_error("invalid UTF-8 conversion");
    return result;
}
std::optional<std::filesystem::path> choose_structure_image(HWND owner) {
    std::array<wchar_t,32768> name{};
    constexpr wchar_t filter[] =
        L"Disk images (*.iso;*.udf;*.wim;*.esd;*.swm;*.vhd;*.vhdx;*.img;*.ima)\0"
        L"*.iso;*.udf;*.wim;*.esd;*.swm;*.vhd;*.vhdx;*.img;*.ima;*.bif;*.mds;*.ccd;*.nrg;*.cue;*.cso;*.zso;*.chd\0"
        L"All files (*.*)\0*.*\0\0";
    OPENFILENAMEW ofn{};
    ofn.lStructSize=sizeof(ofn);
    ofn.hwndOwner=owner;
    ofn.lpstrFile=name.data();
    ofn.nMaxFile=static_cast<DWORD>(name.size());
    ofn.lpstrFilter=filter;
    ofn.lpstrTitle=L"ISO-WCX - Select image to inspect";
    ofn.Flags=OFN_EXPLORER|OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|
              OFN_NOCHANGEDIR|OFN_HIDEREADONLY;
    if(!GetOpenFileNameW(&ofn)) {
        if(CommDlgExtendedError()!=0)
            throw std::runtime_error("Windows Open File dialog failed");
        return std::nullopt; // User cancelled.
    }
    return std::filesystem::path(name.data());
}
std::optional<std::filesystem::path> choose_structure_output(
        HWND owner,const std::filesystem::path& image) {
    std::array<wchar_t,32768> name{};
    const auto suggested=image.parent_path()/
                         (image.stem().wstring()+L"-ISO-WCX-Structure.txt");
    const auto txt=suggested.wstring();
    if(txt.size()>=name.size()-1)
        throw std::runtime_error("suggested report path is too long");
    std::copy(txt.begin(),txt.end(),name.begin());
    constexpr wchar_t filter[]=L"Text report (*.txt)\0*.txt\0All files (*.*)\0*.*\0\0";
    OPENFILENAMEW ofn{};
    ofn.lStructSize=sizeof(ofn);
    ofn.hwndOwner=owner;
    ofn.lpstrFile=name.data();
    ofn.nMaxFile=static_cast<DWORD>(name.size());
    ofn.lpstrFilter=filter;
    ofn.lpstrDefExt=L"txt";
    ofn.lpstrTitle=L"ISO-WCX - Export Image Structure";
    ofn.Flags=OFN_EXPLORER|OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST|
              OFN_NOCHANGEDIR;
    if(!GetSaveFileNameW(&ofn)) {
        if(CommDlgExtendedError()!=0)
            throw std::runtime_error("Windows Save File dialog failed");
        return std::nullopt;
    }
    // Only text reports may be opened by ShellExecuteW after user approval.
    // Prevent a crafted file name from writing an executable script and
    // launching it from the success prompt.
    const std::filesystem::path output(name.data());
    auto extension=output.extension().wstring();
    for(auto& ch:extension)
        if(ch>=L'A' && ch<=L'Z')ch=static_cast<wchar_t>(ch-L'A'+L'a');
    if(extension!=L".txt")
        throw std::runtime_error("the structure report must be saved as .txt");
    return output;
}
void write_structure_atomically(const std::filesystem::path& image,
                                const std::filesystem::path& dest,
                                const std::string& report) {
    std::error_code ec;
    const bool target_exists=std::filesystem::exists(dest,ec);
    if(ec)throw std::runtime_error("cannot inspect report destination");
    if(target_exists) {
        const bool same=std::filesystem::equivalent(image,dest,ec);
        if(ec)throw std::runtime_error("cannot verify report destination identity");
        if(same)throw std::runtime_error("report destination is the source image");
    }
    const std::string payload=std::string("\xEF\xBB\xBF",3)+report;
    if(payload.size()>static_cast<std::size_t>(MAXDWORD))
        throw std::runtime_error("image structure report too large");
    static std::atomic<unsigned> serial{0};
    std::filesystem::path staged;
    HANDLE file=INVALID_HANDLE_VALUE;
    for(unsigned n=0;n<20;++n) {
        staged=dest;
        staged+=L".iso-wcx-"+
                std::to_wstring(GetCurrentProcessId())+L"-"+
                std::to_wstring(GetTickCount64())+L"-"+
                std::to_wstring(serial.fetch_add(1))+L".tmp";
        file=CreateFileW(staged.c_str(),GENERIC_WRITE,0,nullptr,
                         CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file!=INVALID_HANDLE_VALUE)break;
        const auto code=GetLastError();
        if(code!=ERROR_FILE_EXISTS && code!=ERROR_ALREADY_EXISTS)
            throw std::runtime_error("cannot create report staging file");
    }
    if(file==INVALID_HANDLE_VALUE)
        throw std::runtime_error("cannot reserve a unique report staging file");
    DWORD written=0;
    const bool success=WriteFile(file,payload.data(),
                     static_cast<DWORD>(payload.size()),&written,nullptr)!=FALSE &&
                     written==payload.size() &&
                     FlushFileBuffers(file)!=FALSE;
    CloseHandle(file);
    if(!success) {
        (void)DeleteFileW(staged.c_str());
        throw std::runtime_error("cannot write/flush image structure report");
    }
    if(!MoveFileExW(staged.c_str(),dest.c_str(),
                    MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
        (void)DeleteFileW(staged.c_str());
        throw std::runtime_error("cannot atomically finalize structure report");
    }
}
void export_structure_dialog(HWND dlg,bool ru) {
    try {
        const auto image=choose_structure_image(dlg);
        if(!image)return;
        const auto destination=choose_structure_output(dlg,*image);
        if(!destination)return;
        const auto image_size=std::filesystem::file_size(*image);
        const auto image_modified=std::filesystem::last_write_time(*image);
        put_status(dlg,tr(ru,L"Inspecting image (read-only)...",
                            L"Проверка образа (только чтение)..."));
        const HCURSOR previous=SetCursor(LoadCursorW(nullptr,IDC_WAIT));
        std::string report;
        try {
            report=build_image_structure_report(*image);
        }catch(...) {
            SetCursor(previous);
            throw;
        }
        SetCursor(previous);
        if(std::filesystem::file_size(*image)!=image_size ||
           std::filesystem::last_write_time(*image)!=image_modified)
            throw std::runtime_error("source image changed during export; report discarded");
        write_structure_atomically(*image,*destination,report);
        put_status(dlg,tr(ru,L"Structure report exported.",
                            L"Структура образа сохранена."));
        const std::wstring message=std::wstring(tr(ru,
            L"Structure exported (UTF-8). Image unchanged:\n",
            L"Структура сохранена (UTF-8). Образ не изменён:\n"))+
            destination->wstring()+L"\n\n"+
            tr(ru,L"Open report?",L"Открыть отчёт?");
        if(MessageBoxW(dlg,message.c_str(),L"ISO-WCX Export Image Structure",
                       MB_ICONINFORMATION|MB_YESNO)==IDYES) {
            (void)ShellExecuteW(dlg,L"open",destination->c_str(),nullptr,
                                nullptr,SW_SHOWNORMAL);
        }
    }catch(const std::exception& ex) {
        // Diagnostics must never throw while handling an exception from the
        // file picker or report writer (filesystem what() may be non-UTF-8).
        std::wstring reason;
        try{reason=from_utf8(ex.what());}catch(...){reason=L"Error detail unavailable";}
        const auto message=std::wstring(tr(ru,
                 L"Image structure export failed:\n",
                 L"Не удалось сохранить структуру образа:\n"))+reason;
        put_status(dlg,tr(ru,L"Structure export failed.",
                            L"Ошибка экспорта структуры."));
        MessageBoxW(dlg,message.c_str(),L"ISO-WCX",MB_ICONERROR|MB_OK);
    }catch(...) {
        put_status(dlg,L"Structure export failed.");
        MessageBoxW(dlg,L"Unexpected error while exporting image structure.",
                    L"ISO-WCX",MB_ICONERROR|MB_OK);
    }
}

// Do not block that path on DNS, HTTP, a modal dialog or file downloads.
// Background checks only run in the real Total Commander process.
std::atomic_bool background_started{false};

bool has_notified(const std::wstring& tag) {
    std::array<wchar_t,64> saved{};
    DWORD bytes=static_cast<DWORD>(saved.size()*sizeof(wchar_t));
    return RegGetValueW(HKEY_CURRENT_USER,registry_path,L"LastNotifiedTag",
               RRF_RT_REG_SZ,nullptr,saved.data(),&bytes)==ERROR_SUCCESS &&
           saved.back()==L'\0' && tag==saved.data();
}
void record_notified(const std::wstring& tag) {
    const DWORD bytes=static_cast<DWORD>((tag.size()+1)*sizeof(wchar_t));
    write_reg(L"LastNotifiedTag",REG_SZ,tag.c_str(),bytes);
}

DWORD WINAPI background_worker(LPVOID argument) {
    HMODULE module=static_cast<HMODULE>(argument);
    try {
        const auto tag=latest_release();
        const auto parsed=update::parse_version(narrow(tag));
        if(parsed && update::newer(*parsed,installed) && !has_notified(tag) &&
           read_dword(L"AutoCheck",1)) {
            // Notify only once per version, and only after a complete
            // successful HTTPS check. The dialog is not part of archive I/O.
            record_notified(tag);
            const auto ru=russian();
            const auto prompt=std::wstring(tr(ru,
                L"A newer Stable version of ISO-WCX is available: ",
                L"Доступна новая Stable-версия ISO-WCX: ")) + tag +
                tr(ru,
                L"\nDownload the official ZIP and verify SHA-256?\n"
                L"Installation will require confirmation in Total Commander.",
                L"\nСкачать официальный ZIP с проверкой SHA-256?\n"
                L"Установка потребует подтверждения в Total Commander.");
            if(MessageBoxW(nullptr,prompt.c_str(),L"ISO-WCX Update",
                     MB_YESNO|MB_ICONINFORMATION|MB_SETFOREGROUND)==IDYES) {
                const auto checked=save_verified_archive(tag);
                open_in_totalcmd(nullptr,checked);
            }
        }
    }catch(const std::exception&) {
        // Never affect file browsing or host startup on a network failure.
    }
    FreeLibraryAndExitThread(module,0);
    return 0;
}

INT_PTR CALLBACK dialog_proc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    auto* state=reinterpret_cast<State*>(GetWindowLongPtrW(dlg,DWLP_USER));
    switch(msg) {
    case WM_INITDIALOG: {
        state=reinterpret_cast<State*>(lp);
        SetWindowLongPtrW(dlg,DWLP_USER,lp);
        state->russian=russian();
        state->automatic=read_dword(L"AutoCheck",1);
        state->wim_fast=read_wim_fast_setting();
        add_control(dlg,L"STATIC",L"",SS_LEFT,id_status,12,10,340,38);
        add_control(dlg,L"BUTTON",tr(state->russian,
             L"Do not check for updates automatically",
             L"Не проверять обновления автоматически"),
             BS_AUTOCHECKBOX|WS_TABSTOP,id_never,12,53,330,18);
        add_control(dlg,L"BUTTON",tr(state->russian,L"Check now",
             L"Проверить сейчас"),BS_PUSHBUTTON|WS_TABSTOP,
             id_check,12,81,105,25);
        add_control(dlg,L"BUTTON",tr(state->russian,L"Download update",
             L"Скачать обновление"),BS_PUSHBUTTON|WS_TABSTOP,
             id_install,125,81,117,25);
        add_control(dlg,L"BUTTON",tr(state->russian,L"Close",L"Закрыть"),
             BS_DEFPUSHBUTTON|WS_TABSTOP,IDCANCEL,252,81,100,25);
        add_control(dlg,L"BUTTON",tr(state->russian,
             L"Export Image Structure...",L"Экспорт структуры образа..."),
             BS_PUSHBUTTON|WS_TABSTOP,id_export,12,113,340,26);
        add_control(dlg,L"BUTTON",tr(state->russian,
             L"Fast WIM folders (experimental; administrator required)",
             L"Быстрые папки WIM (экспериментально; нужны права администратора)"),
             BS_AUTOCHECKBOX|WS_TABSTOP,id_wim_fast,12,145,340,24);
        add_control(dlg,L"STATIC",tr(state->russian,
             L"Creates a temporary WIM copy. Keep disabled for production images.",
             L"Создаёт временную копию WIM. Для рабочих образов оставьте выключенным."),
             SS_LEFT,0,12,175,340,31);
        CheckDlgButton(dlg,id_wim_fast,
                       state->wim_fast?BST_CHECKED:BST_UNCHECKED);
        CheckDlgButton(dlg,id_never,
              state->automatic?BST_UNCHECKED:BST_CHECKED);
        EnableWindow(GetDlgItem(dlg,id_install),FALSE);
        put_status(dlg,std::wstring(tr(state->russian,
              L"Installed ISO-WCX ",L"Установлен ISO-WCX "))+
              version_name(installed));
        if(state->automatic && due_today())
            PostMessageW(dlg,check_later,0,0);
        return TRUE;
    }
    case check_later:
        if(state)check_release(dlg,*state,false);
        return TRUE;
    case WM_COMMAND: {
        if(!state)return FALSE;
        const int cmd=LOWORD(wp);
        if(cmd==id_never && HIWORD(wp)==BN_CLICKED) {
            state->automatic=IsDlgButtonChecked(dlg,id_never)!=BST_CHECKED;
            save_automatic(state->automatic);
            return TRUE;
        }
        if(cmd==id_wim_fast && HIWORD(wp)==BN_CLICKED) {
            const bool selected=IsDlgButtonChecked(dlg,id_wim_fast)==BST_CHECKED;
            if(selected) {
                const auto warning=tr(state->russian,
                    L"Experimental WIM mounting requires administrator rights and "
                    L"an NTFS workspace with enough free space for a complete "
                    L"temporary WIM copy. Only use on backed-up images.\n\n"
                    L"Enable fast folder creation?",
                    L"Экспериментальное монтирование WIM требует прав "
                    L"администратора и свободного места на NTFS для полной "
                    L"временной копии WIM. Работайте с резервной копией.\n\n"
                    L"Включить быстрое создание папок?");
                if(MessageBoxW(dlg,warning,L"ISO-WCX WIM Preview",
                    MB_ICONWARNING|MB_YESNO|MB_DEFBUTTON2)!=IDYES) {
                    CheckDlgButton(dlg,id_wim_fast,BST_UNCHECKED);
                    return TRUE;
                }
            }
            if(!save_wim_fast_setting(selected)) {
                MessageBoxW(dlg,tr(state->russian,
                    L"Could not save WIM preference in the current user registry.",
                    L"Не удалось сохранить параметр WIM в реестре пользователя."),
                    L"ISO-WCX",MB_ICONERROR|MB_OK);
                CheckDlgButton(dlg,id_wim_fast,
                    state->wim_fast?BST_CHECKED:BST_UNCHECKED);
                return TRUE;
            }
            state->wim_fast=selected;
            return TRUE;
        }
        if(cmd==id_check) {check_release(dlg,*state,true);return TRUE;}
        if(cmd==id_install) {download_and_offer(dlg,*state);return TRUE;}
        if(cmd==id_export) {export_structure_dialog(dlg,state->russian);return TRUE;}
        if(cmd==IDCANCEL || cmd==IDOK) {EndDialog(dlg,0);return TRUE;}
        break;
    }
    case WM_CLOSE:
        EndDialog(dlg,0);return TRUE;
    }
    return FALSE;
}
} // namespace


void schedule_update_check() {
    // WCX methods are called for every archive and possibly in parallel.
    // Decide once per process before opening the registry or allocating the
    // 64-KiB executable-path buffer, so archive I/O stays fast.
    if(background_started.exchange(true,std::memory_order_relaxed))
        return;
    // With a non-TC host (e.g. the DLL loader smoke test), never start an
    // update worker or show unsolicited user-interface dialogs.
    std::array<wchar_t,32768> executable{};
    const DWORD length=GetModuleFileNameW(nullptr,executable.data(),
                                          static_cast<DWORD>(executable.size()));
    if(!length || length>=executable.size() ||
       !totalcmd_path(std::wstring_view(executable.data(),length)) ||
       !read_dword(L"AutoCheck",1) || !due_today())
        return;

    HMODULE module=nullptr;
    // Keep the WCX DLL resident until its own asynchronous code returns.
    // Otherwise a user could unload the plugin during an HTTP request.
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&background_started),&module))
        return;
    save_last_check(); // Throttle even failed/offline GitHub checks.
    HANDLE thread=CreateThread(nullptr,0,background_worker,module,0,nullptr);
    if(!thread) {
        FreeLibrary(module);
        return;
    }
    CloseHandle(thread);
}

void configure_updates(HWND parent,HINSTANCE instance) {
    // Zero-child dialog template; dynamically created controls scale with the
    // user's dialog font and avoid maintaining a separate .rc resource file.
    alignas(DWORD) std::array<std::uint8_t,256> raw{};
    const auto store32=[&](std::size_t at,DWORD v){std::memcpy(raw.data()+at,&v,sizeof(v));};
    const auto store16=[&](std::size_t at,WORD v){std::memcpy(raw.data()+at,&v,sizeof(v));};
    store32(0,WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME|DS_CENTER);
    store32(4,0);store16(8,0);store16(10,0);store16(12,0);
    store16(14,367);store16(16,214);store16(18,0);store16(20,0);
    constexpr wchar_t caption[]=L"ISO-WCX Settings";
    std::memcpy(raw.data()+22,caption,sizeof(caption));
    State state{};
    (void)DialogBoxIndirectParamW(instance,
        reinterpret_cast<const DLGTEMPLATE*>(raw.data()),
        parent,dialog_proc,reinterpret_cast<LPARAM>(&state));
}
} // namespace iso_wcx
#endif
