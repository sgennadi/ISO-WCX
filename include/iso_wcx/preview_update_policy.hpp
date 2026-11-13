#pragma once
// Parse public GitHub Actions preview metadata without trusting links from JSON.
// Stable releases continue to use a verified SHA256SUMS download; Actions
// previews are opened in the browser and are never silently installed.
#include "iso_wcx/update_policy.hpp"
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace iso_wcx::preview_update {
// GitHub's REST Actions workflow_runs objects include verbose repository,
// actor and commit metadata. Six successful runs exceeded 74 KiB in the real
// repository (2026-11-12), so the old generic 64-KiB HTTP ceiling silently
// hid RC updates. Keep a separate bounded maximum just for this endpoint.
inline constexpr std::size_t max_runs_response_bytes=256u*1024u;
struct Run {std::uint64_t id=0;unsigned number=0;};
struct Build {
    update::Version version{};
    unsigned rc=0,number=0;
    std::uint64_t run_id=0,artifact_id=0;
    std::string name;
};

inline std::string_view trim(std::string_view v) {
    while(!v.empty() && (v.front()==' ' || v.front()=='\r' ||
                          v.front()=='\n' || v.front()=='\t'))v.remove_prefix(1);
    while(!v.empty() && (v.back()==' ' || v.back()=='\r' ||
                          v.back()=='\n' || v.back()=='\t'))v.remove_suffix(1);
    return v;
}
inline void whitespace(std::string_view v,std::size_t& i){
    while(i<v.size() && (v[i]==' '||v[i]=='\t'||v[i]=='\r'||v[i]=='\n'))++i;
}
// Skip an entire scalar, array or object; quoted commas and nested objects
// cannot split the parent JSON field. This is a bounded reader, not JSON
// deserialization into executable updater settings.
inline std::size_t value_end(std::string_view v,std::size_t i){
    int depth=0;
    bool quoted=false,escape=false;
    for(;i<v.size();++i){
        const char c=v[i];
        if(quoted) {
            if(escape)escape=false;
            else if(c=='\\')escape=true;
            else if(c=='"')quoted=false;
        }else if(c=='"')quoted=true;
        else if(c=='{'||c=='[')++depth;
        else if(c=='}'||c==']') {
            if(depth==0)break;
            --depth;
        }else if(c==','&&depth==0)break;
    }
    return i;
}
inline std::optional<std::string_view> member(std::string_view obj,
                                              std::string_view wanted){
    obj=trim(obj);
    if(obj.size()<2 || obj.front()!='{' || obj.back()!='}')return std::nullopt;
    std::size_t i=1;
    while(i<obj.size()-1){
        whitespace(obj,i);
        if(i>=obj.size()-1)break;
        if(obj[i]!='"')return std::nullopt;
        const auto key_start=++i;
        while(i<obj.size() && obj[i]!='"'){
            if(obj[i]=='\\')return std::nullopt; // GitHub keys are plain ASCII
            ++i;
        }
        if(i>=obj.size())return std::nullopt;
        const auto key=obj.substr(key_start,i-key_start);
        ++i;whitespace(obj,i);
        if(i>=obj.size() || obj[i++]!=':')return std::nullopt;
        whitespace(obj,i);
        const auto start=i;
        i=value_end(obj,i);
        if(key==wanted)return trim(obj.substr(start,i-start));
        if(i<obj.size() && obj[i]==',')++i;
        else break;
    }
    return std::nullopt;
}
inline std::optional<std::string> ascii(std::string_view s) {
    s=trim(s);
    if(s.size()<2 || s.front()!='"' || s.back()!='"')return std::nullopt;
    std::string result;
    for(const char c:s.substr(1,s.size()-2)){
        if(c=='\\' || static_cast<unsigned char>(c)<32 ||
           static_cast<unsigned char>(c)>126)return std::nullopt;
        result.push_back(c);
    }
    return result;
}
inline std::optional<std::uint64_t> integer(std::string_view s) {
    s=trim(s);if(s.empty() || s.size()>20)return std::nullopt;
    std::uint64_t n=0;
    const auto r=std::from_chars(s.data(),s.data()+s.size(),n);
    if(r.ec!=std::errc{} || r.ptr!=s.data()+s.size())return std::nullopt;
    return n;
}
inline std::vector<std::string_view> array(std::string_view v) {
    v=trim(v);
    std::vector<std::string_view> out;
    if(v.size()<2 || v.front()!='[' || v.back()!=']')return out;
    std::size_t i=1;
    while(i<v.size()-1 && out.size()<64){
        whitespace(v,i);if(i>=v.size()-1)break;
        const auto begin=i;
        i=value_end(v,i);
        if(i==begin)break;
        out.push_back(trim(v.substr(begin,i-begin)));
        if(i<v.size() && v[i]==',')++i;
        else break;
    }
    return out;
}
inline std::vector<Run> runs(std::string_view response) {
    std::vector<Run> out;
    const auto all=member(response,"workflow_runs");
    if(!all)return out;
    for(auto entry:array(*all)){
        const auto id=member(entry,"id"),n=member(entry,"run_number");
        const auto status=member(entry,"status"),result=member(entry,"conclusion");
        if(!id || !n || !status || !result)continue;
        const auto parsed_id=integer(*id),parsed_number=integer(*n);
        if(!parsed_id || !parsed_number || !*parsed_id ||
           !*parsed_number || *parsed_number>65535u)continue;
        if(ascii(*status)!=std::optional<std::string>("completed") ||
           ascii(*result)!=std::optional<std::string>("success"))continue;
        out.push_back({*parsed_id,static_cast<unsigned>(*parsed_number)});
    }
    return out;
}
inline std::optional<Build> parse_name(std::string_view text) {
    constexpr std::string_view prefix="ISO-WCX-Installable-";
    if(!text.starts_with(prefix))return std::nullopt;
    text.remove_prefix(prefix.size());
    if(text.empty() || text[0]!='v')return std::nullopt;
    const auto dash=text.find("-RC");
    if(dash==text.npos)return std::nullopt;
    const auto ver=update::parse_version(text.substr(0,dash));
    if(!ver)return std::nullopt;
    text.remove_prefix(dash+3);
    const auto marker=text.find("-B");
    if(marker==text.npos)return std::nullopt;
    auto rc=integer(text.substr(0,marker));
    auto build=integer(text.substr(marker+2));
    if(!rc || !build || !*rc || !*build || *rc>99 || *build>65535)
        return std::nullopt;
    Build candidate;
    candidate.version=*ver;
    candidate.rc=static_cast<unsigned>(*rc);
    candidate.number=static_cast<unsigned>(*build);
    return candidate;
}
// Workflow reruns are not necessarily ordered by version and RC.
inline bool preferred(const Build& candidate,const Build& current){
    if(update::newer(candidate.version,current.version))return true;
    if(candidate.version!=current.version)return false;
    if(candidate.rc!=current.rc)return candidate.rc>current.rc;
    return candidate.number>current.number;
}
inline void consider(std::optional<Build>& best,std::optional<Build> candidate){
    if(candidate && (!best || preferred(*candidate,*best)))best=std::move(candidate);
}
inline std::optional<Build> artifact(std::string_view response,Run run){
    const auto all=member(response,"artifacts");
    if(!all)return std::nullopt;
    std::optional<Build> selected;
    for(auto entry:array(*all)){
        const auto id=member(entry,"id"),name=member(entry,"name");
        const auto expired=member(entry,"expired");
        if(!id || !name || !expired || trim(*expired)!="false")continue;
        const auto ident=integer(*id);
        const auto text=ascii(*name);
        if(!ident || !*ident || !text)continue;
        auto candidate=parse_name(*text);
        if(!candidate || candidate->number!=run.number)continue;
        candidate->name=*text;
        candidate->run_id=run.id;
        candidate->artifact_id=*ident;
        consider(selected,std::move(candidate));
    }
    return selected;
}
inline bool newer(const Build& remote,update::Version local,
                  unsigned installed_rc,unsigned installed_build){
    if(update::newer(remote.version,local))return true;
    if(remote.version!=local || installed_rc==0)return false;
    if(remote.rc!=installed_rc)return remote.rc>installed_rc;
    return remote.number>installed_build;
}
} // namespace iso_wcx::preview_update
