#pragma once
// Pure, source-format-preserving migration of ISO-WCX's own WCX registrations.
// No executable, PowerShell script or Total Commander global reset required.
// Disk I/O and explicit user consent live in wcx_registration_repair.cpp.
#include <algorithm>
#include <array>
#include <cwctype>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace iso_wcx::tc_registration {
struct Patch {
    std::wstring text;
    std::vector<std::wstring> extensions;
    std::vector<std::wstring> conflicts;
    unsigned duplicates_removed=0;
    bool changed=false;
};
struct Line {
    std::wstring body;
    std::wstring ending;
};
inline std::wstring fold(std::wstring value) {
    std::transform(value.begin(),value.end(),value.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}
inline std::wstring trim(std::wstring_view input) {
    const auto first=input.find_first_not_of(L" \t");
    if(first==std::wstring_view::npos)return {};
    const auto last=input.find_last_not_of(L" \t");
    return std::wstring(input.substr(first,last-first+1));
}
inline std::vector<Line> split(std::wstring_view text) {
    std::vector<Line> lines;
    std::size_t offset=0;
    while(offset<text.size()) {
        const auto end=text.find(L'\n',offset);
        if(end==std::wstring_view::npos) {
            lines.push_back({std::wstring(text.substr(offset)),{}});
            break;
        }
        const bool cr=end>offset && text[end-1]==L'\r';
        lines.push_back({std::wstring(text.substr(offset,end-offset-(cr?1:0))),
                         cr?L"\r\n":L"\n"});
        offset=end+1;
    }
    return lines;
}
inline std::wstring join(const std::vector<Line>& lines) {
    std::wstring out;
    for(const auto& line:lines)out+=line.body+line.ending;
    return out;
}
inline std::wstring section(std::wstring_view line) {
    const auto name=trim(line);
    if(name.size()<3 || name.front()!=L'[' || name.back()!=L']')
        return {};
    return fold(trim(std::wstring_view(name).substr(1,name.size()-2)));
}
inline std::pair<std::wstring,std::wstring> property(
        std::wstring_view line) {
    const auto at=line.find(L'=');
    if(at==std::wstring_view::npos || line.empty() ||
       line.front()==L';' || line.front()==L'#')
        return {};
    return {fold(trim(line.substr(0,at))),trim(line.substr(at+1))};
}
inline std::wstring value(std::wstring_view text,
                          std::wstring_view group,
                          std::wstring_view name) {
    std::wstring current;
    const auto wanted_group=fold(std::wstring(group));
    const auto wanted_name=fold(std::wstring(name));
    for(const auto& line:split(text)) {
        const auto sec=section(line.body);
        if(!sec.empty()) {current=sec;continue;}
        if(current!=wanted_group)continue;
        const auto [key,val]=property(line.body);
        if(key==wanted_name)return val;
    }
    return {};
}
inline std::wstring unpack_plugin_path(std::wstring_view field) {
    auto result=trim(field);
    const auto comma=result.find(L',');
    if(comma!=std::wstring::npos && comma>0 && comma<6 &&
       std::all_of(result.begin(),result.begin()+static_cast<std::ptrdiff_t>(comma),
                   [](wchar_t c){return c>=L'0' && c<=L'9';}))
        result=trim(std::wstring_view(result).substr(comma+1));
    if(result.size()>1 && result.front()==L'"' && result.back()==L'"')
        result=result.substr(1,result.size()-2);
    return result;
}
inline bool owned_plugin(std::wstring_view entry) {
    const auto path=fold(unpack_plugin_path(entry));
    const auto at=path.find_last_of(L"\\/");
    const auto file=path.substr(at==std::wstring::npos?0:at+1);
    return file==L"iso-wcx.wcx" || file==L"iso-wcx.wcx64";
}
inline bool supported(std::wstring_view ext) {
    constexpr std::array<std::wstring_view,23> extensions{
        L"iso",L"udf",L"cue",L"ccd",L"nrg",L"mds",L"mdx",
        L"cso",L"zso",L"isz",L"daa",L"img",L"raw",L"vhd",
        L"vhdx",L"wim",L"esd",L"swm",L"ima",L"vfd",L"flp",
        L"bif",L"imz"};
    return std::find(extensions.begin(),extensions.end(),ext)!=extensions.end();
}
// Replaces stale 0/192/222/bare-path flags, moves previous ISO-WCX installs
// to the currently loaded DLL, and removes same-extension duplicates.
// Foreign plugin bindings ALWAYS win: ambiguous extensions are not changed.
inline Patch repair_packer_plugins(std::wstring_view input,
                                   std::wstring_view installed_wcx) {
    Patch patch{};
    patch.text=std::wstring(input);
    auto lines=split(input);
    struct Entry {std::size_t line;std::wstring val;};
    std::map<std::wstring,std::vector<Entry>> owned;
    std::map<std::wstring,std::vector<Entry>> foreign;
    std::wstring current;
    for(std::size_t i=0;i<lines.size();++i) {
        const auto sec=section(lines[i].body);
        if(!sec.empty()) {current=sec;continue;}
        if(current!=L"packerplugins")continue;
        const auto [key,val]=property(lines[i].body);
        if(key.empty() || !supported(key))continue;
        (owned_plugin(val)?owned:foreign)[key].push_back({i,val});
    }
    for(const auto& [ext,entries]:owned) {
        if(!foreign[ext].empty()) {
            patch.conflicts.push_back(ext);
            continue; // Preserve unrelated working plugins and user intent.
        }
        const auto normalized=ext+L"=158,"+std::wstring(installed_wcx);
        if(lines[entries[0].line].body!=normalized) {
            lines[entries[0].line].body=normalized;
            patch.changed=true;
        }
        for(std::size_t n=1;n<entries.size();++n) {
            auto& line=lines[entries[n].line];
            line.body.clear();
            line.ending.clear();
            patch.changed=true;
            ++patch.duplicates_removed;
        }
        if(lines[entries[0].line].body==normalized &&
           (entries.size()>1 || entries[0].val!=L"158,"+std::wstring(installed_wcx)))
            patch.extensions.push_back(ext);
    }
    if(patch.changed)patch.text=join(lines);
    return patch;
}
inline Patch invalidate_cache(std::wstring_view input) {
    Patch patch{};
    patch.text=std::wstring(input);
    auto lines=split(input);
    std::wstring current;
    for(auto& line:lines) {
        const auto sec=section(line.body);
        if(!sec.empty()) {current=sec;continue;}
        if(current!=L"packerplugins64")continue;
        const auto [key,val]=property(line.body);
        (void)val;
        if(key==L"$checksum$") {
            line.body.clear();line.ending.clear();patch.changed=true;
        }
    }
    if(patch.changed)patch.text=join(lines);
    return patch;
}
} // namespace iso_wcx::tc_registration
