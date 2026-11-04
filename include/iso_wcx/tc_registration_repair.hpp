#pragma once
// Pure, source-format-preserving migration of ISO-WCX's own WCX registrations.
// No executable, PowerShell script or Total Commander global reset required.
// Disk I/O and explicit user consent live in wcx_registration_repair.cpp.
#include <algorithm>
#include <array>
#include <cwctype>
#include <cstddef>
#include <map>
#include <set>
#include <stdexcept>
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
// The running Total Commander instance may use /i=<custom.ini> which takes
// precedence over portable settings, COMMANDER_INI and the registry. The
// existing helper previously inspected the wrong file in that case, so an
// apparently correct wim=158 association could be unrelated to the active
// process. Parse an already tokenized Windows command line without opening
// or modifying any INI; reject ambiguous switches instead of guessing.
inline std::wstring command_line_ini_override(
        const std::vector<std::wstring>& args) {
    std::wstring chosen;
    for(std::size_t i=1;i<args.size();++i) {
        const auto& arg=args[i];
        const bool slash=arg.size()>=3 &&
            (arg[0]==L'/' || arg[0]==L'-') &&
            std::towlower(arg[1])==L'i' && arg[2]==L'=';
        const bool bare=arg.size()>=2 &&
            std::towlower(arg[0])==L'i' && arg[1]==L'=';
        if(!slash && !bare)continue;
        const auto offset=slash?3u:2u;
        auto value=arg.substr(offset);
        if(value.empty() && i+1<args.size())value=args[++i];
        if(value.empty())throw std::runtime_error(
            "Total Commander /i= must include an INI path");
        if(!chosen.empty() && fold(chosen)!=fold(value))
            throw std::runtime_error(
                "Conflicting Total Commander /i= arguments");
        chosen=std::move(value);
    }
    return chosen;
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
// A correct portable TC registration should stay portable; merely opening
// a WIM must not turn %COMMANDER_PATH% into a machine-specific absolute path.
// The pure implementation uses Windows separators on Linux CI as well.
inline std::wstring windows_path(std::wstring path) {
    std::replace(path.begin(),path.end(),L'/',L'\\');
    return fold(std::move(path));
}
// Editable formats require mask 159 so TC routes F5 into existing images.
// Read-only container types remain at 158 in per-extension INI entries.
inline constexpr std::array<std::wstring_view,11> writable_image_extensions{
    L"iso",L"udf",L"wim",L"img",L"raw",L"vhd",
    L"vhdx",L"ima",L"vfd",L"flp",L"bif"
};
inline constexpr std::wstring_view expected_wcx_mask(
        std::wstring_view ext) {
    for(const auto editable:writable_image_extensions)
        if(ext==editable)return L"159";
    return L"158";
}
inline bool is_current_wcx_registration(
        std::wstring_view entry,std::wstring_view installed_wcx,
        std::wstring_view ext) {
    const auto field=trim(entry);
    const auto expected=expected_wcx_mask(ext);
    if(field.size()<expected.size()+1 ||
       field.substr(0,expected.size())!=expected ||
       field[expected.size()]!=L',')
        return false;
    auto candidate=windows_path(unpack_plugin_path(field));
    const auto installed=windows_path(std::wstring(installed_wcx));
    if(candidate==installed)return true;
    constexpr std::wstring_view token=L"%commander_path%";
    constexpr std::wstring_view plugins=L"\\plugins\\wcx\\";
    if(candidate.rfind(token,0)!=0)return false;
    const auto offset=installed.rfind(plugins);
    if(offset==std::wstring::npos)return false;
    candidate=installed.substr(0,offset)+
              candidate.substr(token.size());
    return candidate==installed;
}
inline constexpr std::array<std::wstring_view,23> supported_image_extensions{
    L"iso",L"udf",L"cue",L"ccd",L"nrg",L"mds",L"mdx",
    L"cso",L"zso",L"isz",L"daa",L"img",L"raw",L"vhd",
    L"vhdx",L"wim",L"esd",L"swm",L"ima",L"vfd",L"flp",
    L"bif",L"imz"
};
inline bool supported(std::wstring_view ext) {
    return std::find(supported_image_extensions.begin(),
                     supported_image_extensions.end(),ext)!=
           supported_image_extensions.end();
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
        // Prefer a correct existing line, including a portable
        // %COMMANDER_PATH% binding, instead of destroying its layout and
        // moving it to the first duplicate's line.
        std::size_t keep=0;
        for(std::size_t n=0;n<entries.size();++n)
            if(is_current_wcx_registration(entries[n].val,installed_wcx,ext)) {
                keep=n;
                break;
            }
        bool changed=false;
        if(!is_current_wcx_registration(entries[keep].val,installed_wcx,ext)) {
            lines[entries[keep].line].body=
                ext+L"="+std::wstring(expected_wcx_mask(ext))+L","+
                std::wstring(installed_wcx);
            patch.changed=true;
            changed=true;
        }
        for(std::size_t n=0;n<entries.size();++n) {
            if(n==keep)continue;
            auto& line=lines[entries[n].line];
            line.body.clear();
            line.ending.clear();
            patch.changed=true;
            changed=true;
            ++patch.duplicates_removed;
        }
        if(changed)patch.extensions.push_back(ext);
    }
    if(patch.changed)patch.text=join(lines);
    return patch;
}
// First TC installation exposes only ISO/WIM through pluginst.inf (two
// native dialogs, not 23). After loading an image, an explicitly confirmed
// operation can add the still-UNASSIGNED image formats in one INI update.
// Existing foreign handlers, intentionally empty entries, duplicate keys
// and every unrelated INI section are preserved verbatim.
inline Patch register_missing_packer_plugins(
        std::wstring_view input,std::wstring_view installed_wcx) {
    Patch patch{};
    patch.text=std::wstring(input);
    auto lines=split(input);
    std::set<std::wstring> registered;
    std::wstring current;
    std::size_t end=lines.size();
    unsigned sections=0;
    bool inside=false;
    for(std::size_t i=0;i<lines.size();++i) {
        const auto sec=section(lines[i].body);
        if(!sec.empty()) {
            if(inside) {end=i;inside=false;}
            if(sec==L"packerplugins") {
                if(++sections>1)return patch; // Ambiguous INI: fail closed.
                inside=true;
                end=lines.size();
            }
            current=sec;
            continue;
        }
        if(current==L"packerplugins") {
            const auto [key,val]=property(lines[i].body);
            if(supported(key))registered.insert(key);
        }
    }
    if(sections!=1)return patch;
    for(const auto ext:supported_image_extensions) {
        if(!registered.contains(std::wstring(ext)))
            patch.extensions.emplace_back(ext);
    }
    if(patch.extensions.empty())return patch;
    std::wstring ending=L"\r\n";
    for(const auto& line:lines) {
        if(!line.ending.empty()) {ending=line.ending;break;}
    }
    // When PackerPlugins is the last section and its final property
    // lacks a trailing newline, add exactly one separator first.
    if(end>0 && lines[end-1].ending.empty())
        lines[end-1].ending=ending;
    std::vector<Line> additions;
    additions.reserve(patch.extensions.size());
    for(const auto& ext:patch.extensions)
        additions.push_back({ext+L"="+
                             std::wstring(expected_wcx_mask(ext))+L","+
                             std::wstring(installed_wcx),ending});
    lines.insert(lines.begin()+static_cast<std::ptrdiff_t>(end),
                 additions.begin(),additions.end());
    patch.text=join(lines);
    patch.changed=true;
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
