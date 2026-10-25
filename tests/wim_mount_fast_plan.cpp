#include "iso_wcx/wim_mount_fast_plan.hpp"
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace iso_wcx;
using namespace iso_wcx::wim_detail;
static void check(bool valid,const char* msg) {
    if(!valid)throw std::runtime_error(msg);
}
static WimTreeItem file(const char* name,std::uint64_t size=17) {
    return WimTreeItem{name,size,0x20,0x526608aa,false};
}
static WimTreeItem dir(const char* name) {
    return WimTreeItem{name,0,0x10,0,true};
}
int main() {
    try {
        constexpr std::uint64_t mib=1024ull*1024ull;
        check(!enough_staging_space(0,1ull<<40),
              "zero-size WIM must be rejected");
        check(!enough_staging_space(64*mib,319*mib),
              "insufficient 256 MiB reserve accepted");
        check(enough_staging_space(64*mib,320*mib),
              "exact 256 MiB workspace reserve rejected");
        // A 16-GiB WIM needs 819.2 MiB, NOT 819.19 MiB. Compute
        // the exact integer threshold to avoid a misleading failed test.
        constexpr std::uint64_t large_wim=16ull*1024ull*mib;
        constexpr std::uint64_t large_reserve=large_wim/20u;
        check(!enough_staging_space(large_wim,large_wim+large_reserve-1),
              "large WIM was accepted one byte below 5% reserve");
        check(enough_staging_space(large_wim,large_wim+large_reserve),
              "large WIM rejected with exact 5% staging reserve");
        check(enough_staging_space(large_wim,large_wim+large_reserve+1),
              "large WIM rejected above 5% staging reserve");
        check(!enough_staging_space(UINT64_MAX-20u,UINT64_MAX),
              "overflow-boundary staging budget accepted");
        ArchiveChange one;
        one.path="New/Child";
        one.directory=true;
        one.source="sample-folder";  // Total Commander provides a source.
        const std::map<unsigned,std::vector<ArchiveChange>> edits{{2,{one}}};
        const auto plan=plan_mounted_folders(edits,{});
        check(plan && plan->index==2 && plan->paths.size()==1,
              "single index nested mkdir should use mount path");
        check(!plan_mounted_folders(edits,{1}),
              "index removal cannot use folder mount path");
        auto mixed=edits;
        mixed[1].push_back(one);
        check(!plan_mounted_folders(mixed,{}),
              "multiple edited indexes must use legacy path");
        mixed=edits;
        mixed.at(2)[0].remove=true;
        check(!plan_mounted_folders(mixed,{}),
              "delete cannot use folder-only mount path");
        mixed=edits;
        mixed.at(2)[0].directory=false;
        check(!plan_mounted_folders(mixed,{}),
              "file replacement cannot use folder-only mount path");
        mixed=edits;
        mixed.at(2).push_back(one);
        check(!plan_mounted_folders(mixed,{}),
              "duplicate paths must not use fast mount path");
        mixed=edits;
        mixed.at(2)[0].path="../escape";
        // The planner requires caller-side edit_path() validation. Such a
        // malformed logical path must never reach this point in production.
        check(!plan_mounted_folders(mixed,{}),
              "unsafe traversal accepted by mount planner");
        mixed.at(2)[0].path="nested/../escape";
        check(!plan_mounted_folders(mixed,{}),
              "nested traversal accepted by mount planner");
        mixed.at(2)[0].path="unsafe:name";
        check(!plan_mounted_folders(mixed,{}),
              "host-invalid name accepted by mount planner");
        for(const auto* value:{"CON","nul.txt","lPt9.log","AUX","COM1",
                               "sub/PRN.md","CONOUT$"}) {
            mixed.at(2)[0].path=value;
            check(!plan_mounted_folders(mixed,{}),
                  "reserved Windows device filename accepted in fast WIM path");
        }
        mixed=edits;
        auto alias=one;
        alias.path="new/CHILD";
        mixed.at(2).push_back(alias);
        check(!plan_mounted_folders(mixed,{}),
              "case-insensitive duplicate directory accepted");

        const std::vector<WimTreeItem> before{
            dir("image1"),file("image1/untouched.bin"),
            dir("image2"),dir("image2/Existing"),file("image2/Existing/keep.dat"),
            dir("image3"),file("image3/preserve.bin")
        };
        auto after=before;
        after.push_back(dir("image2/New"));
        after.push_back(dir("image2/New/Child"));
        std::string why;
        check(verify_mounted_folder_tree(before,after,*plan,why),
              "valid mounted folder edit was refused");
        auto bad=after;
        bad.erase(bad.begin()+1);
        check(!verify_mounted_folder_tree(before,bad,*plan,why),
              "removing a file in another index was accepted");
        bad=after;
        bad[4].size=22;
        check(!verify_mounted_folder_tree(before,bad,*plan,why),
              "changing an untouched file's length was accepted");
        bad=after;
        bad[4].attributes=0;
        check(!verify_mounted_folder_tree(before,bad,*plan,why),
              "changing untouched file flags was accepted");
        bad=after;
        bad.push_back(file("image2/New/unexpected.bin"));
        check(!verify_mounted_folder_tree(before,bad,*plan,why),
              "unexpected new stream accepted");
        bad=after;
        bad.push_back(dir("image2/New"));
        check(!verify_mounted_folder_tree(before,bad,*plan,why),
              "duplicated output path accepted");
        bad=after;
        bad.push_back(dir("image2/new"));
        check(!verify_mounted_folder_tree(before,bad,*plan,why),
              "casefold directory collision accepted");
        bad=before;
        check(!verify_mounted_folder_tree(before,bad,*plan,why),
              "missing requested folder accepted");
        bad=before;
        bad.push_back(file("image2/New"));
        bad.push_back(dir("image2/New/Child"));
        check(!verify_mounted_folder_tree(before,bad,*plan,why),
              "file substituted for expected directory accepted");
        auto broken=before;
        broken.push_back(file("image2/New"));
        check(!verify_mounted_folder_tree(broken,after,*plan,why),
              "creating children under an existing file accepted");
        std::cout<<"PASS: folder-mount plan, exact tree parity and refusal boundaries\n";
    } catch(const std::exception& ex) {
        std::cerr<<"WIM folder mount plan test failed: "<<ex.what()<<'\n';
        return 1;
    }
}
