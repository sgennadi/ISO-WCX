// Synthetic read-only virtual disk partition layout regression tests.
// Creates no physical devices. Fixtures conform to GPT CRC32/MBR layout.
#include "iso_wcx/partition_map.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs=std::filesystem;
using iso_wcx::inspect_partition_table;
using iso_wcx::PartitionTableKind;

namespace {
void check(bool ok,const char* why) {
    if(!ok)throw std::runtime_error(why);
}
void put32(std::vector<std::uint8_t>& b,std::size_t pos,std::uint32_t n) {
    for(unsigned i=0;i<4;++i)b[pos+i]=static_cast<std::uint8_t>(n>>(i*8));
}
void put64(std::vector<std::uint8_t>& b,std::size_t pos,std::uint64_t n) {
    for(unsigned i=0;i<8;++i)b[pos+i]=static_cast<std::uint8_t>(n>>(i*8));
}
// Bitwise reference algorithm rather than using the reader's table implementation.
std::uint32_t crc(const std::uint8_t* data,std::size_t count) {
    std::uint32_t c=~std::uint32_t(0);
    for(std::size_t i=0;i<count;++i) {
        c^=data[i];
        for(unsigned j=0;j<8;++j)c=(c>>1)^((c&1u)?0xedb88320u:0u);
    }
    return ~c;
}
std::vector<std::uint8_t> basic_mbr(bool overlap=false) {
    std::vector<std::uint8_t> d(16u*1024u*1024u);
    d[510]=0x55;d[511]=0xaa;
    d[446+4]=0x07;put32(d,446+8,2048);put32(d,446+12,10000);
    d[462+4]=0x83;put32(d,462+8,overlap?3000:15000);
    put32(d,462+12,1200);
    return d;
}
std::vector<std::uint8_t> gpt(std::uint32_t sector,bool overlapping=false) {
    // 16 MiB @512, 8 MiB @4096; both >128 usable sectors.
    const auto disk_bytes=sector==512?16u*1024u*1024u:8u*1024u*1024u;
    std::vector<std::uint8_t> d(disk_bytes);
    const auto sectors=disk_bytes/sector;
    d[510]=0x55;d[511]=0xaa;
    d[446+4]=0xee;put32(d,446+8,1);put32(d,446+12,sectors-1);
    const auto table=std::size_t(2)*sector;
    d[table]=0xa2; // a synthetic non-zero GPT partition type GUID
    put64(d,table+32,128);put64(d,table+40,255);
    d[table+128]=0xb3;
    put64(d,table+128+32,overlapping?200:256);
    put64(d,table+128+40,400);
    const auto header=std::size_t(sector);
    const std::string sig="EFI PART";
    std::copy(sig.begin(),sig.end(),d.begin()+header);
    put32(d,header+8,0x00010000);
    put32(d,header+12,92);
    put64(d,header+24,1);
    put64(d,header+32,sectors-1);
    put64(d,header+40,34);
    put64(d,header+48,sectors-34);
    put64(d,header+72,2);
    put32(d,header+80,4);
    put32(d,header+84,128);
    put32(d,header+88,crc(d.data()+table,4*128));
    put32(d,header+16,crc(d.data()+header,92));
    return d;
}
void write(const fs::path& path,const std::vector<std::uint8_t>& d) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    check(bool(out),"cannot create synthetic image");
    out.write(reinterpret_cast<const char*>(d.data()),static_cast<std::streamsize>(d.size()));
    check(bool(out),"cannot write synthetic image");
}
void rejects(const fs::path& path,const std::vector<std::uint8_t>& image,
             std::uint32_t sector=512) {
    write(path,image);
    try {
        auto wrong=inspect_partition_table(path,sector);
        (void)wrong;
    } catch(const std::exception&) {return;}
    throw std::runtime_error("invalid virtual disk accepted");
}
}
int main() {
    const auto path=fs::temp_directory_path()/
        ("iso-wcx-partitions-"+std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count())+".img");
    try {
        auto data=basic_mbr();
        write(path,data);
        auto m=inspect_partition_table(path);
        check(m.size()==2 && m[0].scheme==PartitionTableKind::mbr &&
              m[0].mbr_type==0x07 && m[0].first_lba==2048 && m[0].sector_count==10000 &&
              m[1].mbr_type==0x83,"MBR sector mapping wrong");

        rejects(path,basic_mbr(true));
        data=basic_mbr();
        data[510]=0;
        rejects(path,data);
        data=basic_mbr();
        put32(data,446+12,0xffffffffu);
        rejects(path,data);
        data=basic_mbr();
        data[446+4]=0x0f;
        rejects(path,data); // EBR chain is not implemented.

        for(auto sector:{512u,4096u}) {
            auto d=gpt(sector);
            write(path,d);
            const auto parts=inspect_partition_table(path,sector);
            check(parts.size()==2 && parts[0].scheme==PartitionTableKind::gpt &&
                  parts[0].first_lba==128 && parts[0].sector_count==128 &&
                  parts[1].first_lba==256 && parts[1].sector_count==145,
                  "GPT partition mapping wrong");
            rejects(path,gpt(sector,true),sector);
            d=gpt(sector);
            d[sector+16]^=1; // header CRC incorrect
            rejects(path,d,sector);
            d=gpt(sector);
            d[2*sector+100]^=1; // entry array CRC incorrect
            rejects(path,d,sector);
            d=gpt(sector);
            d[sector+8]='X'; // malformed revision (CRC will also fail)
            rejects(path,d,sector);
        }
        fs::remove(path);
        std::cout<<"PASS: read-only MBR/GPT, 512/4096-byte logical sectors, CRC and bounds\n";
    } catch(const std::exception& e) {
        std::error_code ignore;
        fs::remove(path,ignore);
        std::cerr<<"FAIL: "<<e.what()<<"\n";
        return 1;
    }
    return 0;
}
