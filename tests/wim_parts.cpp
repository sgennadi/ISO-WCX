#include "iso_wcx/wim_parts.hpp"
#include <chrono>
#include <iostream>
namespace fs = std::filesystem;
using iso_wcx::resolve_wim_parts;
void check(bool condition) { if (!condition) throw std::runtime_error("SWM assertion failed"); }
void part(const fs::path& file, unsigned number, unsigned total, unsigned guid=1) {
    std::array<char,208> h{};
    std::copy_n("MSWIM\0\0\0",8,h.begin());
    h[8]=static_cast<char>(208); h[14]=1; h[24]=static_cast<char>(guid);
    h[40]=static_cast<char>(number);h[41]=static_cast<char>(number>>8);
    h[42]=static_cast<char>(total);h[43]=static_cast<char>(total>>8);
    std::ofstream out(file,std::ios::binary);out.write(h.data(),h.size());
}
void rejected(const fs::path& p) {
    bool failed=false;
    try { (void)resolve_wim_parts(p); } catch(const std::runtime_error&) { failed=true; }
    check(failed);
}
int main() {
    const auto root=fs::temp_directory_path()/ ("iso-wcx-parts-"+std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    try {
        const auto first=root/"arbitrary.SWM", second=root/"another.swm";
        part(first,1,2); rejected(first);
        part(second,2,2);
        const auto set=resolve_wim_parts(second);
        check(set.size()==2 && set[0]==first && set[1]==second);
        part(root/"unrelated.swm",1,2,2); check(resolve_wim_parts(first)==set);
        part(root/"duplicate.swm",2,2);rejected(first);fs::remove(root/"duplicate.swm");
        part(second,2,3);rejected(first);
        part(second,0,2);rejected(first);
        part(second,3,2);rejected(first);
        part(second,2,2);fs::resize_file(second,40);rejected(first);
        fs::remove(second);part(second,2,2,2);rejected(first);
        part(first,1,5000);rejected(first);
        part(first,1,1);check(resolve_wim_parts(first).size()==1);
        fs::resize_file(first,7);rejected(first);
        fs::remove_all(root);
        std::cout<<"PASS: SWM GUID, ordering, missing, duplicate, inconsistent and truncated parts\n";
        return 0;
    } catch(const std::exception& e) { fs::remove_all(root);std::cerr<<e.what()<<'\n';return 1; }
}
