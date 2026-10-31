// Integration tests for transactional extraction, cancellation and read errors.
// Deliberately uses the public Disc API instead of mocking the disk reader.
#include "iso_wcx/disc.hpp"
#ifdef _WIN32
#include "iso_wcx/ntfs_preallocation.hpp"
#endif
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs=std::filesystem;
namespace {
constexpr std::size_t kSector=2048;
constexpr std::size_t kPayload=3*1024*1024;
void insist(bool ok,const char* msg) {if(!ok)throw std::runtime_error(msg);}
void le32(std::vector<std::uint8_t>& b,std::size_t pos,std::uint32_t value) {
    for(unsigned i=0;i<4;i++)b[pos+i]=static_cast<std::uint8_t>(value>>(8*i));
}
void be32(std::vector<std::uint8_t>& b,std::size_t pos,std::uint32_t value) {
    for(unsigned i=0;i<4;i++)b[pos+i]=static_cast<std::uint8_t>(value>>(8*(3-i)));
}
std::vector<std::uint8_t> rec(std::uint32_t lba,std::uint32_t size,
                              std::uint8_t flags,const std::string& name) {
    const auto len=33+name.size()+(name.size()%2==0?1:0);
    std::vector<std::uint8_t> out(len,0);
    out[0]=static_cast<std::uint8_t>(len);
    le32(out,2,lba);be32(out,6,lba);
    le32(out,10,size);be32(out,14,size);
    out[25]=flags;out[28]=1;out[31]=1;
    out[32]=static_cast<std::uint8_t>(name.size());
    std::copy(name.begin(),name.end(),out.begin()+33);
    return out;
}
std::vector<std::uint8_t> make_image() {
    std::vector<std::uint8_t> data((21+(kPayload+kSector-1)/kSector)*kSector,0);
    const auto root=rec(20,kSector,2,std::string(1,'\0'));
    auto pvd=16*kSector;
    data[pvd]=1;
    for(int i=0;i<5;++i)data[pvd+1+i]="CD001"[i];
    data[pvd+6]=1;
    std::copy(root.begin(),root.end(),data.begin()+static_cast<std::ptrdiff_t>(pvd+156));
    pvd=17*kSector;data[pvd]=255;
    for(int i=0;i<5;++i)data[pvd+1+i]="CD001"[i];
    data[pvd+6]=1;
    std::size_t pos=20*kSector;
    for(const auto& r:{root,rec(20,kSector,2,std::string(1,'\1')),
                      rec(21,kPayload,0,"BIG.BIN;1")}) {
        std::copy(r.begin(),r.end(),data.begin()+static_cast<std::ptrdiff_t>(pos));
        pos+=r.size();
    }
    for(std::size_t i=0;i<kPayload;++i)data[21*kSector+i]=static_cast<std::uint8_t>(i%251);
    return data;
}
void write_string(const fs::path& p,const std::string& s) {
    std::ofstream f(p,std::ios::binary|std::ios::trunc);
    insist(bool(f),"cannot write test destination");f<<s;
}
std::string content(const fs::path& p) {
    std::ifstream f(p,std::ios::binary);
    insist(bool(f),"cannot open expected result");
    return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};
}
void no_staging_files(const fs::path& p) {
    for(const auto& entry:fs::directory_iterator(p)) {
        const auto name=entry.path().filename().string();
        insist(name.find(".iso-wcx-")==std::string::npos,"staging file leaked");
    }
}
}
int main() {
    const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
    const auto folder=fs::temp_directory_path()/("iso-wcx-copy-"+std::to_string(stamp));
    fs::create_directories(folder);
    try {
        const auto image=folder/"input.iso",result=folder/"result.bin";
        auto b=make_image();
        {std::ofstream f(image,std::ios::binary);insist(bool(f),"cannot create test image");
         f.write(reinterpret_cast<const char*>(b.data()),static_cast<std::streamsize>(b.size()));}
        iso_wcx::Disc disc(image);
        insist(disc.items().size()==1 && disc.items()[0].size==kPayload,"bad test image catalog");
        write_string(result,"ORIGINAL FILE - MUST SURVIVE CANCELLATION");
        std::size_t callback_calls=0;
#ifdef _WIN32
        bool saw_full_ntfs_reservation=false;
        const bool expect_ntfs_reservation=
            iso_wcx::ntfs_preallocation::enabled() &&
            iso_wcx::ntfs_preallocation::is_ntfs(folder);
#endif
        bool completed=disc.copy(0,result,[&](std::uint64_t bytes) {
            ++callback_calls;
#ifdef _WIN32
            // Inspect the live sibling .part BEFORE the first 1 MiB callback
            // aborts. Under NTFS the temporary 3 MiB output must already
            // have its full logical size, even though it is being written
            // in many smaller extents/chunks. This tests Disc::copy rather
            // than only the low-level Win32 preparation helper.
            for(const auto& stage:fs::directory_iterator(folder)) {
                const auto name=stage.path().filename().string();
                if(name.find(".iso-wcx-")==std::string::npos ||
                   !name.ends_with(".part"))continue;
                if(fs::file_size(stage.path())==kPayload)
                    saw_full_ntfs_reservation=true;
            }
#endif
            return bytes<1024*1024; // cancel after first chunk is written to staging
        });
        insist(!completed && callback_calls==1,"cancel callback did not abort copy");
#ifdef _WIN32
        if(expect_ntfs_reservation)
            insist(saw_full_ntfs_reservation,
                   "Disc::copy did not reserve the full NTFS output before streaming");
#endif
        insist(content(result)=="ORIGINAL FILE - MUST SURVIVE CANCELLATION",
               "original destination truncated after cancellation");
        no_staging_files(folder);
        const auto fresh=folder/"cancel-new.bin";
        completed=disc.copy(0,fresh,[](std::uint64_t){return false;});
        insist(!completed && !fs::exists(fresh),"cancel left incomplete extracted file");
        no_staging_files(folder);
        // TEST operation must validate bytes without touching the requested filename.
        insist(disc.copy(0,result,[](std::uint64_t){return true;},true),"test-only read failed");
        insist(content(result)=="ORIGINAL FILE - MUST SURVIVE CANCELLATION","test modified output");
        completed=disc.copy(0,result);
        insist(completed,"successful extraction failed");
        insist(fs::file_size(result)==kPayload,"incorrect extracted size");
        {std::ifstream f(result,std::ios::binary);insist(bool(f),"cannot open committed result");
         std::array<unsigned char,4096> first{};
         f.read(reinterpret_cast<char*>(first.data()),static_cast<std::streamsize>(first.size()));
         insist(bool(f),"short committed read");
         for(std::size_t i=0;i<first.size();++i)
             insist(first[i]==i%251,"incorrect extracted data");}
        no_staging_files(folder);
        write_string(result,"KEEP THIS ON SOURCE READ ERROR");
        fs::resize_file(image,23*kSector);  // Leaves ISO volume and directory readable.
        bool threw=false;
        try {disc.copy(0,result);}catch(const std::exception&){threw=true;}
        insist(threw,"truncated image did not fail extraction");
        insist(content(result)=="KEEP THIS ON SOURCE READ ERROR",
               "source error destroyed pre-existing file");
        no_staging_files(folder);
        fs::remove_all(folder);
        std::cout<<"PASS: cancellation, rollback, replacement, test-only and source error\n";
        return 0;
    }catch(const std::exception& ex){
        std::cerr<<"copy_transaction test failed: "<<ex.what()<<"\n";
        std::error_code ignored;fs::remove_all(folder,ignored);
        return 1;
    }
}
