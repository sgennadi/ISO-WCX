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
            change.path.find("//")!=std::string::npos) return std::nullopt;
        if (!paths.insert(change.path).second) return std::nullopt;
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
    for(const auto& item:before)
        if(!old.emplace(item.path,&item).second)
            return fail("duplicate original WIM path");
    for(const auto& item:after)
        if(!fresh.emplace(item.path,&item).second)
            return fail("duplicate edited WIM path");
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
