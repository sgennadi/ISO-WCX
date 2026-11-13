// Format-agnostic protected byte-range regression (Windows/Linux, no
// external tools, no Total Commander). No source files are modified.
#include "iso_wcx/optical_edit_guard.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs=std::filesystem;

void require(bool good,const char* reason) {
    if(!good)throw std::runtime_error(reason);
}
void put(const fs::path& path,const std::string& content) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    out.write(content.data(),static_cast<std::streamsize>(content.size()));
    require(bool(out),"cannot write optical fixture");
}
int main() {
    const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
    const auto folder=fs::temp_directory_path()/
        ("iso-wcx-optical-guard-"+std::to_string(stamp));
    fs::create_directories(folder);
    try {
        const auto original=folder/"original.iso";
        const auto staged=folder/"stage.iso";
        const std::string bytes(128*1024,'A');
        put(original,bytes);
        auto altered=bytes;
        altered[4500]='X'; // Unprotected UDF-only data
        put(staged,altered);
        iso_wcx::OpticalEditGuard guard(bytes.size());
        guard.protect(0,4096);
        guard.protect(8192,2048);
        guard.protect(10240,1024); // Adjacent regions merge
        guard.verify(original,staged);

        altered[8192]='B'; // First byte of an immutable boot image
        put(staged,altered);
        bool refused=false;
        try{guard.verify(original,staged);}
        catch(const iso_wcx::EditUnsupported&){refused=true;}
        require(refused,"protected boot-region mutation was accepted");

        altered=bytes;
        put(staged,altered);
        refused=false;
        try{guard.protect(bytes.size()-2,4);}
        catch(const iso_wcx::EditUnsupported&){refused=true;}
        require(refused,"out-of-bounds protected region accepted");

        refused=false;
        try{guard.verify(original,staged,[]{return false;});}
        catch(const iso_wcx::EditCancelled&){refused=true;}
        require(refused,"cancellation ignored");

        altered.pop_back();
        put(staged,altered);
        refused=false;
        try{guard.verify(original,staged);}
        catch(const iso_wcx::EditUnsupported&){refused=true;}
        require(refused,"truncated stage accepted");

        require(fs::file_size(original)==bytes.size(),
                "original fixture was damaged");
        fs::remove_all(folder);
        std::cout<<"PASS: universal optical immutable spans, rollback, bounds, cancel\n";
        return 0;
    }catch(const std::exception& e){
        fs::remove_all(folder);
        std::cerr<<"FAIL: "<<e.what()<<"\n";
        return 1;
    }
}
