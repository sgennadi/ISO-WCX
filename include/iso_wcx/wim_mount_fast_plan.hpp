#pragma once
// Pure, portable safety decisions for the optional WIMGAPI mounted-folder edit.
// No filesystem write or Windows image mount occurs in this header.
#include "iso_wcx/archive_edit.hpp"
#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace iso_wcx::wim_detail {

// Avoid ambiguous aliases on case-insensitive NTFS when creating
// directories through WIMGAPI. Leave non-ASCII UTF-8 bytes unchanged;
// the Windows system APIs remain authoritative for Unicode casing.
inline std::string ascii_case_key(std::string value) {
    for(auto& ch:value)
        if(ch>='A' && ch<='Z')
            ch=static_cast<char>(ch-'A'+'a');
    return value;
}
inline bool windows_device_name(const std::string& segment) {
    const auto base=ascii_case_key(segment.substr(0,segment.find('.')));
    if(base=="con" || base=="prn" || base=="aux" || base=="nul" ||
       base=="conin$" || base=="conout$")
        return true;
    if(base.size()==4 && (base.starts_with("com") || base.starts_with("lpt"))
       && base[3]>='1' && base[3]<='9')
        return true;
    return false;
}

struct MountFolderPlan {
    unsigned index = 0;
    std::vector<std::string> paths;
};

inline std::optional<MountFolderPlan> plan_mounted_folders(
    const std::map<unsigned,std::vector<ArchiveChange>>& edits,
    const std::set<unsigned>& deleted) {
    if (!deleted.empty() || edits.size()!=1) return std::nullopt;
    auto it=edits.begin();
    if (it->first==0 || it->second.empty() || it->second.size()>64)
        return std::nullopt;
    MountFolderPlan plan;
    plan.index=it->first;
    std::set<std::string> paths;
    for (const auto& change:it->second) {
        // The caller already validated change.path with edit_path(); source
        // is allowed because Total Commander supplies it for directories.
        if (!change.directory || change.remove || change.path.empty() ||
            change.path.front()=='/' || change.path.back()=='/' ||
            change.path.size()>2048 ||
            change.path.find("//")!=std::string::npos ||
            change.path.find('\\')!=std::string::npos)
            return std::nullopt;
        // Double-check traversal and invalid host-path tokens before entering
        // the Windows mount. Upstream edit_path() remains authoritative.
        std::size_t offset=0;
        while(offset<change.path.size()) {
            const auto end=change.path.find('/',offset);
            const auto stop=end==std::string::npos?change.path.size():end;
            const auto segment=change.path.substr(offset,stop-offset);
            if(segment.empty() || segment=="." || segment==".." ||
               segment.back()=='.' || segment.back()==' ' ||
               windows_device_name(segment))
                return std::nullopt;
            for(const unsigned char ch:segment)
                if(ch<32 || ch==127 || ch==':' || ch=='*' || ch=='?' ||
                   ch=='"' || ch=='<' || ch=='>' || ch=='|')
                    return std::nullopt;
            if(end==std::string::npos) break;
            offset=end+1;
        }
        if (!paths.insert(ascii_case_key(change.path)).second)
            return std::nullopt;
        plan.paths.push_back(change.path);
    }
    return plan;
}

struct WimTreeItem {
    std::string path;
    std::uint64_t size = 0;
    std::uint32_t attributes = 0;
    std::uint32_t dos_time = 0;
    bool directory = false;
};

inline bool verify_mounted_folder_tree(
    const std::vector<WimTreeItem>& before,
    const std::vector<WimTreeItem>& after,
    const MountFolderPlan& plan,
    std::string& failure) {
    auto fail=[&](const char* msg) {
        failure=msg;
        return false;
    };
    if (plan.index==0 || plan.paths.empty())
        return fail("no mounted-folder edit plan");
    std::unordered_map<std::string,const WimTreeItem*> old, fresh;
    old.reserve(before.size());
    fresh.reserve(after.size());
    std::set<std::string> old_ci,fresh_ci;
    for(const auto& item:before) {
        if(!old.emplace(item.path,&item).second ||
           !old_ci.insert(ascii_case_key(item.path)).second)
            return fail("ambiguous original WIM path/case alias");
    }
    for(const auto& item:after) {
        if(!fresh.emplace(item.path,&item).second ||
           !fresh_ci.insert(ascii_case_key(item.path)).second)
            return fail("ambiguous edited WIM path/case alias");
    }
    const std::string prefix="image"+std::to_string(plan.index);
    const auto image=old.find(prefix);
    if(image==old.end() || !image->second->directory)
        return fail("selected original WIM image is missing");
    std::set<std::string> permitted;
    for(const auto& relative:plan.paths) {
        std::size_t offset=0;
        while(offset<relative.size()) {
            const auto slash=relative.find('/',offset);
            const auto count=slash==std::string::npos?relative.size():slash;
            const auto path=prefix+"/"+relative.substr(0,count);
            const auto existing=old.find(path);
            if(existing!=old.end() && !existing->second->directory)
                return fail("WIM directory path intersects an existing file");
            permitted.insert(path);
            if(slash==std::string::npos) break;
            offset=slash+1;
        }
    }
    for(const auto& [key,item]:old) {
        const auto updated=fresh.find(key);
        if(updated==fresh.end())
            return fail("an unchanged WIM item disappeared");
        const auto& value=*updated->second;
        if(value.directory!=item->directory || value.size!=item->size)
            return fail("an existing WIM item changed type or size");
        // Directory timestamps may change when adding children. Existing
        // regular file attributes/timestamps must remain exact.
        if(!item->directory &&
           (value.attributes!=item->attributes ||
            value.dos_time!=item->dos_time))
            return fail("an unchanged WIM file changed attributes or time");
    }
    for(const auto& [key,item]:fresh) {
        if(old.contains(key))continue;
        if(!permitted.contains(key) || !item->directory || item->size!=0)
            return fail("WIM mount unexpectedly created or modified an item");
    }
    for(const auto& key:permitted) {
        const auto found=fresh.find(key);
        if(found==fresh.end() || !found->second->directory)
            return fail("requested WIM folder was not created");
    }
    failure.clear();
    return true;
}
} // namespace iso_wcx::wim_detail
