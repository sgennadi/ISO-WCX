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
// Two independent linked EBRs plus a primary MBR partition, safe for 512/4K.
std::vector<std::uint8_t> linked_ebr(std::uint32_t sector) {
    std::vector<std::uint8_t> bytes(16u*1024u*1024u);
    bytes[510]=0x55;bytes[511]=0xaa;
    bytes[446+4]=0x07;
    put32(bytes,446+8,1500);
    put32(bytes,446+12,200);
    bytes[462+4]=0x0f;
    put32(bytes,462+8,64);
    put32(bytes,462+12,1200);
    for(std::uint32_t i=0;i<3;++i) {
        const std::uint32_t ebr=64+400*i;
        const auto pos=std::size_t(ebr)*sector;
        bytes[pos+510]=0x55;bytes[pos+511]=0xaa;
        bytes[pos+446+4]=i==1?0x83:0x07;
        put32(bytes,pos+446+8,1);
        put32(bytes,pos+446+12,100);
        if(i<2) {
            bytes[pos+462+4]=0x0f;
            put32(bytes,pos+462+8,(i+1)*400);
            put32(bytes,pos+462+12,1200-(i+1)*400);
        }
    }
    return bytes;
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
std::vector<std::uint8_t> gpt_with_backup(std::uint32_t sector) {
    auto bytes=gpt(sector);
    const auto sectors=bytes.size()/sector;
    const auto primary_header=std::size_t(sector);
    const auto backup_header=std::size_t(sectors-1)*sector;
    const auto backup_table=std::size_t(sectors-2)*sector;
    // Independent entry-array copy, not an alias of the primary array.
    std::copy_n(bytes.begin()+2u*sector,512,bytes.begin()+backup_table);
    std::copy_n(bytes.begin()+primary_header,sector,bytes.begin()+backup_header);
    put64(bytes,backup_header+24,sectors-1); // current backup GPT LBA
    put64(bytes,backup_header+32,1); // backlink to primary
    put64(bytes,backup_header+72,sectors-2); // backup table immediately before header
    put32(bytes,backup_header+16,0);
    put32(bytes,backup_header+16,crc(bytes.data()+backup_header,92));
    return bytes;
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
        rejects(path,data); // Garbage at declared extended partition start.

        for(auto logical_sector:{512u,4096u}) {
            auto d=linked_ebr(logical_sector);
            write(path,d);
            auto partitions=inspect_partition_table(path,logical_sector);
            check(partitions.size()==4 &&
                  partitions[0].first_lba==65 && partitions[0].sector_count==100 &&
                  partitions[1].first_lba==465 && partitions[1].mbr_type==0x83 &&
                  partitions[2].first_lba==865 &&
                  partitions[3].first_lba==1500 && partitions[3].mbr_type==0x07,
                  "valid linked EBR logical/primary partitions not discovered");

            d=linked_ebr(logical_sector);
            d[std::size_t(464)*logical_sector+510]=0; // 2nd EBR signature invalid.
            rejects(path,d,logical_sector);
            d=linked_ebr(logical_sector);
            put32(d,std::size_t(64)*logical_sector+462+8,0); // EBR self-link.
            rejects(path,d,logical_sector);
            d=linked_ebr(logical_sector);
            put32(d,std::size_t(464)*logical_sector+462+8,1); // Backward link.
            rejects(path,d,logical_sector);
            d=linked_ebr(logical_sector);
            put32(d,std::size_t(64)*logical_sector+446+12,500); // Covers next EBR.
            rejects(path,d,logical_sector);
            d=linked_ebr(logical_sector);
            put32(d,std::size_t(864)*logical_sector+446+12,600); // Outside extended range.
            rejects(path,d,logical_sector);
            d=linked_ebr(logical_sector);
            d[std::size_t(64)*logical_sector+478+4]=0x07; // Third EBR entry.
            rejects(path,d,logical_sector);
            d=linked_ebr(logical_sector);
            d[462+4]=0xee; // Hybrid protective GPT with legacy primary.
            rejects(path,d,logical_sector);
            d=linked_ebr(logical_sector);
            put32(d,446+8,100); // Primary partition overlaps extended container.
            rejects(path,d,logical_sector);
        }

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

            // A corrupt primary GPT must be recovered only from a valid
            // secondary header AND its own entry-array checksum.
            d=gpt_with_backup(sector);
            write(path,d);
            auto healthy=inspect_partition_table(path,sector);
            check(healthy.size()==2 &&
                  healthy[0].first_lba==128 && healthy[1].first_lba==256,
                  "dual GPT header image not recognized");
            d=gpt_with_backup(sector);
            d[sector+16]^=1; // damaged primary header CRC
            write(path,d);
            auto backup_recovered=inspect_partition_table(path,sector);
            check(backup_recovered.size()==2 &&
                  backup_recovered[0].first_lba==128 &&
                  backup_recovered[1].first_lba==256,
                  "valid backup GPT did not recover corrupted primary");
            d=gpt_with_backup(sector);
            d[2*sector+100]^=1; // primary array corrupted
            write(path,d);
            backup_recovered=inspect_partition_table(path,sector);
            check(backup_recovered.size()==2 &&
                  backup_recovered[1].first_lba==256,
                  "backup GPT did not recover corrupted primary entry array");
            d=gpt_with_backup(sector);
            d[sector+16]^=1;
            d[(d.size()/sector-1)*sector+16]^=1; // both headers CRC damaged
            rejects(path,d,sector);
            d=gpt_with_backup(sector);
            d[sector+16]^=1;
            d[(d.size()/sector-2)*sector+100]^=1; // backup entries damaged
            rejects(path,d,sector);
            d=gpt_with_backup(sector);
            d[sector+16]^=1;
            const auto backup_offset=std::size_t(d.size()-sector);
            put64(d,backup_offset+32,2); // backup points to wrong primary LBA
            put32(d,backup_offset+16,0);
            put32(d,backup_offset+16,crc(d.data()+backup_offset,92));
            rejects(path,d,sector);
        }
        fs::remove(path);
        std::cout<<"PASS: read-only MBR/EBR/GPT primary+backup, 512/4096 logical sectors, CRC and bounds\n";
    } catch(const std::exception& e) {
        std::error_code ignore;
        fs::remove(path,ignore);
        std::cerr<<"FAIL: "<<e.what()<<"\n";
        return 1;
    }
    return 0;
}
