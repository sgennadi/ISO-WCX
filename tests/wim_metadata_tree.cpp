#include "iso_wcx/wim_metadata_tree_probe.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace iso_wcx::wim_detail;
namespace fs=std::filesystem;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void put16(std::vector<std::uint8_t>& b,std::size_t pos,std::uint16_t x){
    for(unsigned i=0;i<2;++i)b[pos+i]=static_cast<std::uint8_t>(x>>(i*8));
}
void put32(std::vector<std::uint8_t>& b,std::size_t pos,std::uint32_t x){
    for(unsigned i=0;i<4;++i)b[pos+i]=static_cast<std::uint8_t>(x>>(i*8));
}
void put64(std::vector<std::uint8_t>& b,std::size_t pos,std::uint64_t x){
    for(unsigned i=0;i<8;++i)b[pos+i]=static_cast<std::uint8_t>(x>>(i*8));
}
void descriptor(std::vector<std::uint8_t>& b,std::size_t pos,
                std::uint64_t size,std::uint8_t flags,
                std::uint64_t off){
    for(unsigned i=0;i<7;++i)b[pos+i]=static_cast<std::uint8_t>(size>>(i*8));
    b[pos+7]=flags;
    put64(b,pos+8,off);
    put64(b,pos+16,size);
}
std::vector<std::uint8_t> metadata(){
    std::vector<std::uint8_t> b(256);
    put32(b,0,8); // zero security descriptors; root starts at 8
    put32(b,4,0);
    put64(b,8,104); // unnamed root record
    put32(b,16,0x10);
    put32(b,20,0xffffffffu);
    put64(b,24,120); // root children directory-list offset
    // root record ends at 112, followed by its zero terminator
    put64(b,120,112); // child record
    put32(b,128,0x10);
    put32(b,132,0xffffffffu);
    put64(b,136,240); // empty child directory list
    put16(b,220,6); // child filename length at child+100
    put16(b,222,'N');put16(b,224,'E');put16(b,226,'W');
    // zero child-list terminators at 232 and 240
    return b;
}
std::vector<std::uint8_t> image(const std::vector<std::uint8_t>& m) {
    const std::size_t header_size=208;
    const std::size_t lookup_offset=header_size+m.size();
    std::vector<std::uint8_t> b(lookup_offset+50);
    for(std::size_t i=0;i<8;++i)
        b[i]=static_cast<std::uint8_t>(std::array<char,8>{'M','S','W','I','M',0,0,0}[i]);
    put32(b,8,208);
    put32(b,12,0x00010d00u);
    put16(b,40,1);put16(b,42,1);put32(b,44,1);
    descriptor(b,48,50,0,lookup_offset);
    std::copy(m.begin(),m.end(),b.begin()+static_cast<std::ptrdiff_t>(header_size));
    descriptor(b,lookup_offset,m.size(),2,header_size);
    put16(b,lookup_offset+24,1);
    return b;
}
int main(){
    fs::path path=fs::temp_directory_path()/"iso-wcx-wim-metadata-readonly-fixture.wim";
    try {
        auto b=metadata();
        auto ok=scan_uncompressed_metadata(b);
        check(ok.valid && ok.entries==2 && ok.directories==2 &&
              ok.streams==0 && ok.security_descriptors==0,
              "valid root and empty subdirectory not decoded");
        auto damaged=b;
        put64(damaged,120,64);
        check(!scan_uncompressed_metadata(damaged).valid,
              "too-short directory entry accepted");
        damaged=b;
        put64(damaged,136,120);
        check(!scan_uncompressed_metadata(damaged).valid,
              "cycle in child offsets accepted");
        damaged=b;
        put32(damaged,132,0);
        check(!scan_uncompressed_metadata(damaged).valid,
              "invalid security ID accepted");
        damaged=b;
        put16(damaged,220,5);
        check(!scan_uncompressed_metadata(damaged).valid,
              "odd UTF-16 byte length accepted");
        damaged=b;
        put16(damaged,222,0xd800);
        check(!scan_uncompressed_metadata(damaged).valid,
              "broken UTF-16 surrogate accepted");
        damaged=b;
        put16(damaged,104,1); // root stream count at root+96
        check(!scan_uncompressed_metadata(damaged).valid,
              "invalid alternate-stream length accepted");
        damaged=b;
        put32(damaged,0,1000);
        check(!scan_uncompressed_metadata(damaged).valid,
              "oversized security table accepted");
        damaged=b;
        damaged.resize(100);
        check(!scan_uncompressed_metadata(damaged).valid,
              "truncated metadata accepted");

        const auto raw=image(b);
        {
            std::ofstream out(path,std::ios::binary);
            out.write(reinterpret_cast<const char*>(raw.data()),
                      static_cast<std::streamsize>(raw.size()));
            check(bool(out),"cannot write WIM fixture");
        }
        auto result=scan_wim_metadata_tree(path);
        check(result.valid && result.images==1 && result.entries==2 &&
              result.uncompressed_bytes==b.size(),
              "WIM metadata lookup to directory tree validation failed");
        {
            std::ifstream in(path,std::ios::binary);
            const std::vector<std::uint8_t> unchanged(
                (std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>{});
            check(unchanged==raw,"read-only scanner modified the WIM");
        }
        auto reject=image(b);
        put32(reject,16,0x4); // read-only WIM
        {
            std::ofstream out(path,std::ios::binary|std::ios::trunc);
            out.write(reinterpret_cast<const char*>(reject.data()),
                      static_cast<std::streamsize>(reject.size()));
        }
        check(!scan_wim_metadata_tree(path).valid,"read-only WIM writer candidate accepted");
        fs::remove(path);
        std::cout<<"PASS: bounded read-only WIM security/directory/stream tree validation\n";
    }catch(const std::exception& ex){
        std::error_code ec;fs::remove(path,ec);
        std::cerr<<"FAIL: "<<ex.what()<<'\n';
        return 1;
    }
    return 0;
}
