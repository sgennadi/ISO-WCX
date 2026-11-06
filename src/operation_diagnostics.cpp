// Per-operation ISO-WCX diagnostics. No paths, machine IDs, credentials,
// filenames or file payloads. Network transfer is never automatic.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "iso_wcx/operation_diagnostics.hpp"
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#ifndef ISO_WCX_VERSION_MAJOR
#define ISO_WCX_VERSION_MAJOR 0
#define ISO_WCX_VERSION_MINOR 0
#define ISO_WCX_VERSION_PATCH 0
#define ISO_WCX_RC_NUMBER 0
#define ISO_WCX_BUILD_NUMBER 0
#endif

namespace iso_wcx::diagnostics {
namespace {
std::mutex journal_mutex;
std::atomic<std::uint64_t> next_id{1};
constexpr std::uintmax_t max_log_size=4u*1024u*1024u;
constexpr std::size_t max_read_lines=200000;

std::string build_name() {
    std::string result=std::to_string(ISO_WCX_VERSION_MAJOR)+"."+
        std::to_string(ISO_WCX_VERSION_MINOR)+"."+
        std::to_string(ISO_WCX_VERSION_PATCH);
    if(ISO_WCX_RC_NUMBER)
        result+=" RC"+std::to_string(ISO_WCX_RC_NUMBER)+
                " B"+std::to_string(ISO_WCX_BUILD_NUMBER);
    return result;
}
std::string format_name(const std::filesystem::path& image) {
    auto ext=image.extension().wstring();
    std::transform(ext.begin(),ext.end(),ext.begin(),
                   [](wchar_t c){return wchar_t(towlower(c));});
    if(ext==L".iso")return "iso";
    if(ext==L".udf")return "udf";
    if(ext==L".wim")return "wim";
    if(ext==L".esd")return "esd";
    if(ext==L".swm")return "swm";
    if(ext==L".vhd")return "vhd";
    if(ext==L".vhdx")return "vhdx";
    if(ext==L".img"||ext==L".ima"||ext==L".raw")return "img";
    return "other";
}
std::string classify(std::string_view reason, int code) {
    if(code==0)return "none";
    if(code==21)return "cancelled";
    std::string lower;
    lower.reserve(std::min<std::size_t>(reason.size(),500));
    for(char c:reason.substr(0,500))
        lower.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    auto has=[&](const char* token){return lower.find(token)!=std::string::npos;};
    if(has("checksum")||has("hash mismatch")||has("crc")||has("corrupt"))
        return "integrity_failed";
    if(has("verify")||has("validation failed")||has("readback"))
        return "verification_failed";
    if(has("space")||has("disk full")||has("not enough room"))
        return "out_of_space";
    if(has("access")||has("permission")||has("denied"))
        return "access_denied";
    if(has("boot")||has("protected")||has("el torito"))
        return "protected_layout";
    if(has("missing")||has("not found"))return "missing_input";
    if(code==24)return "unsupported_layout";
    if(code==16)return "io_error";
    return "unexpected_exception";
}
std::string stage_name(std::wstring_view description) {
    std::wstring s(description.substr(0,160));
    std::transform(s.begin(),s.end(),s.begin(),[](wchar_t c){return wchar_t(towlower(c));});
    if(s.find(L"copy")!=s.npos)return "copy";
    if(s.find(L"export")!=s.npos)return "export";
    if(s.find(L"apply")!=s.npos)return "apply";
    if(s.find(L"capture")!=s.npos)return "capture";
    if(s.find(L"verify")!=s.npos||s.find(L"validat")!=s.npos)return "verify";
    if(s.find(L"commit")!=s.npos)return "commit";
    if(s.find(L"delete")!=s.npos||s.find(L"remove")!=s.npos)return "delete";
    if(s.find(L"open")!=s.npos)return "open";
    if(s.find(L"write")!=s.npos||s.find(L"pack")!=s.npos)return "write";
    return "processing";
}
std::string timestamp() {
    SYSTEMTIME value{};
    GetSystemTime(&value);
    char b[40]{};
    sprintf_s(b,"%04u-%02u-%02uT%02u:%02u:%02uZ",
              value.wYear,value.wMonth,value.wDay,
              value.wHour,value.wMinute,value.wSecond);
    return b;
}
std::filesystem::path logs() {
    wchar_t local[32768]{};
    const auto n=GetEnvironmentVariableW(L"LOCALAPPDATA",local,32768);
    if(n==0||n>=32768)return {};
    return std::filesystem::path(local)/L"ISO-WCX"/L"Logs";
}
void append(const std::string& line) noexcept {
    try {
        std::lock_guard guard(journal_mutex);
        auto folder=logs();
        if(folder.empty())return;
        std::filesystem::create_directories(folder);
        auto log=folder/L"operations.jsonl";
        std::error_code ec;
        if(std::filesystem::exists(log,ec) &&
           std::filesystem::file_size(log,ec)>=max_log_size) {
            auto rotated=folder/L"operations.previous.jsonl";
            std::filesystem::remove(rotated,ec);
            ec.clear();
            std::filesystem::rename(log,rotated,ec);
        }
        std::ofstream stream(log,std::ios::binary|std::ios::app);
        if(stream)stream<<line<<'\n';
    }catch(...) {
        // Never make Total Commander operations fail due to logging.
    }
}
std::string base_json(const char* event,const std::string& action,
                      const std::string& format,std::uint64_t id) {
    return "{\"schema\":1,\"event\":\""+std::string(event)+
           "\",\"utc\":\""+timestamp()+"\",\"build\":\""+build_name()+
           "\",\"op_id\":"+std::to_string(id)+
           ",\"action\":\""+action+"\",\"format\":\""+format+"\"";
}
std::string url_escape(std::string_view s) {
    const char* digits="0123456789ABCDEF";
    std::string out;
    for(unsigned char c:s) {
        if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||
           (c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~')
            out.push_back(char(c));
        else {
            out.push_back('%');
            out.push_back(digits[c>>4]);
            out.push_back(digits[c&15]);
        }
    }
    return out;
}
struct Group {
    std::string build,format,action,reason;
    int code=0;
    bool operator<(const Group& rhs) const {
        return std::tie(build,format,action,code,reason)<
               std::tie(rhs.build,rhs.format,rhs.action,rhs.code,rhs.reason);
    }
};
bool field(std::string_view line,std::string_view field_name,std::string& result) {
    std::string needle="\""+std::string(field_name)+"\":\"";
    auto at=line.find(needle);
    if(at==line.npos)return false;
    at+=needle.size();
    auto end=line.find('"',at);
    if(end==line.npos||end-at>100)return false;
    result=line.substr(at,end-at);
    return true;
}
bool number(std::string_view line,std::string_view field_name,int& result) {
    std::string needle="\""+std::string(field_name)+"\":";
    auto at=line.find(needle);
    if(at==line.npos)return false;
    at+=needle.size();
    auto end=at;
    if(end<line.size()&&line[end]=='-')++end;
    while(end<line.size()&&line[end]>='0'&&line[end]<='9')++end;
    if(end==at||end-at>8)return false;
    try {result=std::stoi(std::string(line.substr(at,end-at)));return true;}
    catch(...){return false;}
}
std::map<Group,std::uint64_t> summary() {
    std::map<Group,std::uint64_t> counts;
    const auto folder=logs();
    if(folder.empty())return counts;
    for(const wchar_t* name:{L"operations.previous.jsonl",L"operations.jsonl"}) {
        std::ifstream source(folder/name,std::ios::binary);
        std::string line;
        std::size_t lines=0;
        while(lines++<max_read_lines&&std::getline(source,line)) {
            if(line.size()>2000||line.find("\"event\":\"complete\"")==std::string::npos)
                continue;
            Group key;
            if(!field(line,"build",key.build)||
               !field(line,"format",key.format)||
               !field(line,"action",key.action)||
               !field(line,"reason",key.reason)||
               !number(line,"code",key.code))continue;
            // All fields are controlled by our journal, but apply
            // strict allowlists before creating a public report.
            auto allowed=[](const std::string& s) {
                if(s.size()>64)return false;
                for(unsigned char c:s)
                    if(!std::isalnum(c)&&c!=' '&&c!='.'&&c!='_'&&c!='-'&&
                       c!='('&&c!=')')return false;
                return true;
            };
            if(allowed(key.build)&&allowed(key.format)&&allowed(key.action)&&
               allowed(key.reason))++counts[key];
        }
    }
    return counts;
}
std::string report_text() {
    const auto groups=summary();
    std::ostringstream stream;
    stream<<"# ISO-WCX Diagnostic Summary\n\n"
          <<"Build: "<<build_name()<<"\n"
          <<"Generated UTC: "<<timestamp()<<"\n"
          <<"Local operation records are summarized without paths or filenames.\n"
          <<"No image contents, user credentials, usernames or device IDs are included.\n\n"
          <<"| Build | Format | Action | WCX code | Category | Count |\n"
          <<"|---|---|---|---:|---|---:|\n";
    unsigned rows=0;
    for(const auto& [group,count]:groups) {
        if(rows++>=80)break;
        stream<<"| "<<group.build<<" | "<<group.format<<" | "<<group.action
              <<" | "<<group.code<<" | "<<group.reason<<" | "<<count<<" |\n";
    }
    if(groups.empty())stream<<"\nNo operations have been logged yet.\n";
    return stream.str();
}
} // namespace

Operation::Operation(const char* action,const std::filesystem::path& image,
                     unsigned file_count,unsigned image_index) noexcept {
    try {
        action_=action?action:"other";
        format_=format_name(image);
        id_=next_id.fetch_add(1,std::memory_order_relaxed);
        start_=GetTickCount64();
        count_=file_count;
        image_index_=image_index;
        append(base_json("start",action_,format_,id_)+
               ",\"file_count\":"+std::to_string(count_)+
               ",\"image_index\":"+std::to_string(image_index_)+"}");
    }catch(...) {}
}
Operation::~Operation() noexcept {
    if(!completed_)complete(13,"interrupted");
}
void Operation::stage(std::wstring_view value,unsigned pct) noexcept {
    try {
        const auto kind=stage_name(value);
        pct=std::min(pct,100u);
        if(stage_==kind&&pct<last_stage_pct_+10&&pct!=100)return;
        stage_=kind;
        last_stage_pct_=pct;
        append(base_json("stage",action_,format_,id_)+
               ",\"stage\":\""+stage_+"\",\"pct\":"+std::to_string(pct)+"}");
    }catch(...) {}
}
void Operation::complete(int code,std::string_view reason) noexcept {
    if(completed_)return;
    completed_=true;
    try {
        append(base_json("complete",action_,format_,id_)+
               ",\"code\":"+std::to_string(code)+
               ",\"reason\":\""+classify(reason,code)+"\""+
               ",\"file_count\":"+std::to_string(count_)+
               ",\"image_index\":"+std::to_string(image_index_)+
               ",\"elapsed_ms\":"+std::to_string(GetTickCount64()-start_)+"}");
    }catch(...) {}
}
std::filesystem::path log_directory() noexcept {
    try{return logs();}catch(...){return {};}
}
std::filesystem::path export_report() noexcept {
    try {
        auto folder=logs();
        if(folder.empty())return {};
        folder=folder.parent_path()/L"Reports";
        std::filesystem::create_directories(folder);
        auto target=folder/L"ISO-WCX-Diagnostics-Summary.md";
        std::ofstream out(target,std::ios::binary|std::ios::trunc);
        if(!out)return {};
        out<<report_text();
        out.close();
        return out?target:std::filesystem::path{};
    }catch(...){return {};}
}
bool open_github_issue() noexcept {
    try {
        const auto exported=export_report();
        if(exported.empty())return false;
        // Prefill a limited, aggregate-only issue. GitHub itself authenticates
        // the user and asks for a visible confirmation before publication.
        auto body=report_text();
        if(body.size()>2600)body.resize(2600);
        const auto url=std::string(
            "https://github.com/sgennadi/ISO-WCX/issues/new?title=")+
            url_escape("ISO-WCX Diagnostics: "+build_name())+
            "&body="+url_escape(body);
        if(url.size()>7500)return false;
        std::wstring wide(url.begin(),url.end()); // URL is ASCII percent encoded.
        auto r=ShellExecuteW(nullptr,L"open",wide.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
        return reinterpret_cast<std::intptr_t>(r)>32;
    }catch(...){return false;}
}
std::wstring status() noexcept {
    try {
        const auto dir=logs();
        return L"Local operation logging enabled. No automatic network upload. "+
               (dir.empty()?std::wstring(L"No LOCALAPPDATA.") : dir.wstring());
    }catch(...){return L"Diagnostic status unavailable.";}
}
} // namespace iso_wcx::diagnostics
#endif
