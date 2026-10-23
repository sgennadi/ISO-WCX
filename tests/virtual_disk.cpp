// Bounded fixed VHD vs raw hard disk regression tests.
// No physical disks, Windows mounts, or dependency on libvhdi are used.
#include "iso_wcx/partition_map.hpp"
#include "iso_wcx/virtual_disk.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {

void check(bool condition, const char* reason) {
    if(!condition) throw std::runtime_error(reason);
}
void put_be32(std::array<std::uint8_t,512>& b,std::size_t p,std::uint32_t value) {
    for(unsigned i=0;i<4;++i)b[p+i]=static_cast<std::uint8_t>(value>>(24-i*8));
}
void put_be64(std::array<std::uint8_t,512>& b,std::size_t p,std::uint64_t value) {
    for(unsigned i=0;i<8;++i)b[p+i]=static_cast<std::uint8_t>(value>>(56-i*8));
}
void put_le32(std::vector<std::uint8_t>& b,std::size_t p,std::uint32_t value) {
    for(unsigned i=0;i<4;++i)b[p+i]=static_cast<std::uint8_t>(value>>(i*8));
}
void checksum(std::array<std::uint8_t,512>& b) {
    put_be32(b,64,0);
    std::uint32_t sum=0;
    for(auto c:b)sum+=c;
    put_be32(b,64,~sum);
}
std::array<std::uint8_t,512> vhd_footer(std::uint64_t capacity) {
    std::array<std::uint8_t,512> b{};
    std::copy_n("conectix",8,b.begin());
    put_be32(b,8,2);
    put_be32(b,12,0x00010000);
    put_be64(b,16,UINT64_MAX); // Fixed VHD has no dynamic metadata header.
    put_be64(b,40,capacity);
    put_be64(b,48,capacity);
    put_be32(b,60,2); // Fixed disk.
    checksum(b);
    return b;
}
std::vector<std::uint8_t> raw_disk() {
    std::vector<std::uint8_t> data(4u*1024u*1024u);
    data[510]=0x55;data[511]=0xaa;
    data[446+4]=0x07; // NTFS partition type in primary MBR.
    put_le32(data,446+8,2048);
    put_le32(data,446+12,2048);
    data[2048u*512u]=0x83;
    data[2048u*512u+1]=0x42;
    return data;
}
std::vector<std::uint8_t> fixed_vhd(
    const std::vector<std::uint8_t>& raw,
    const std::array<std::uint8_t,512>& footer) {
    std::vector<std::uint8_t> result=raw;
    result.insert(result.end(),footer.begin(),footer.end());
    return result;
}
void save(const fs::path& file, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(file,std::ios::binary|std::ios::trunc);
    check(bool(out),"cannot create synthetic VHD image");
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    check(bool(out),"cannot write synthetic VHD image");
}
void expect_failure(const fs::path& file,
                    const std::vector<std::uint8_t>& bytes) {
    save(file,bytes);
    try {
        const iso_wcx::VirtualDiskReader disk(file);
        (void)disk;
    }catch(const std::exception&) {
        return;
    }
    throw std::runtime_error("unsafe or unsupported VHD image accepted");
}

} // namespace

int main() {
    const auto stem = "iso-wcx-fixed-vhd-"+std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto dir=fs::temp_directory_path() / stem;
    const auto vhd=dir/"test.vhd";
    const auto raw=dir/"test.img";
    const auto vhdx=dir/"test.vhdx";
    try {
        fs::create_directories(dir);
        const auto bytes=raw_disk();
        save(raw,bytes);
        {
            iso_wcx::VirtualDiskReader reader(raw);
            check(reader.format()==iso_wcx::VirtualDiskFormat::raw_image,
                  "raw IMG format incorrectly detected");
            check(reader.size()==bytes.size(),"raw IMG capacity mismatch");
        }
        const auto footer=vhd_footer(bytes.size());
        save(vhd,fixed_vhd(bytes,footer));
        {
            iso_wcx::VirtualDiskReader reader(vhd);
            check(reader.format()==iso_wcx::VirtualDiskFormat::fixed_vhd,
                  "fixed VHD format incorrectly detected");
            check(reader.size()==bytes.size(),"fixed VHD capacity mismatch");
            check(reader.read(2048u*512u,2)==std::vector<std::uint8_t>({0x83,0x42}),
                  "VHD data offset incorrectly mapped");
            bool bounded=false;
            try { (void)reader.read(reader.size(),1); }
            catch(const std::exception&) {bounded=true;}
            check(bounded,"VHD footer exposed as partition data");
        }
        const auto entries=iso_wcx::inspect_partition_table(vhd);
        check(entries.size()==1 && entries[0].first_lba==2048 &&
              entries[0].sector_count==2048 && entries[0].mbr_type==0x07,
              "MBR partition inside fixed VHD not recognized");

        auto damaged=footer;
        damaged[0]='X';
        checksum(damaged);
        expect_failure(vhd,fixed_vhd(bytes,damaged));
        damaged=footer;
        damaged[32]=1; // Intentionally fails checksum.
        expect_failure(vhd,fixed_vhd(bytes,damaged));
        damaged=footer;
        put_be32(damaged,60,3); // Dynamic VHD intentionally unsupported.
        checksum(damaged);
        expect_failure(vhd,fixed_vhd(bytes,damaged));
        damaged=footer;
        put_be64(damaged,16,512); // Non-fixed next metadata offset.
        checksum(damaged);
        expect_failure(vhd,fixed_vhd(bytes,damaged));
        damaged=footer;
        put_be64(damaged,48,bytes.size()+512);
        checksum(damaged);
        expect_failure(vhd,fixed_vhd(bytes,damaged));
        damaged=footer;
        damaged[84]=1; // Saved-state VHD must not be read as consistent.
        checksum(damaged);
        expect_failure(vhd,fixed_vhd(bytes,damaged));
        auto extra=fixed_vhd(bytes,footer);
        extra.push_back(0);
        expect_failure(vhd,extra);
        auto partial=fixed_vhd(bytes,footer);
        partial.pop_back();
        expect_failure(vhd,partial);

        auto vhdx_bytes=bytes;
        std::copy_n("vhdxfile",8,vhdx_bytes.begin());
        save(vhdx,vhdx_bytes);
        expect_failure(vhdx,vhdx_bytes);

        std::error_code ec;
        fs::remove_all(dir,ec);
        std::cout<<"PASS: fixed VHD logical bytes, partition table, footer checksum, bounds, and negative cases\n";
    }catch(const std::exception& e) {
        std::error_code ec;
        fs::remove_all(dir,ec);
        std::cerr<<"FAIL: "<<e.what()<<"\n";
        return 1;
    }
    return 0;
}
