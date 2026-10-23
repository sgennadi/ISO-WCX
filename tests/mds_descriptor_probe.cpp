// Independent metadata-only MDSv1, MDSv2 and MDX regression tests.
// Test images deliberately contain NO encryption keys, passwords or real
// copyrighted disc sectors. No decryption/extraction is claimed.
#include "iso_wcx/mds_probe.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs=std::filesystem;
namespace {
using Bytes=std::vector<std::uint8_t>;

void check(bool valid, const char* message) {
    if(!valid)throw std::runtime_error(message);
}
void le32(Bytes& data,std::size_t at,std::uint32_t v) {
    check(at<=data.size() && data.size()-at>=4,"bad synthetic u32");
    for(unsigned k=0;k<4;++k)data[at+k]=static_cast<std::uint8_t>(v>>(8*k));
}
void le64(Bytes& data,std::size_t at,std::uint64_t v) {
    le32(data,at,static_cast<std::uint32_t>(v));
    le32(data,at+4,static_cast<std::uint32_t>(v>>32));
}
Bytes descriptor(std::size_t length, std::uint8_t major) {
    Bytes data(length);
    const std::string signature="MEDIA DESCRIPTOR";
    check(length>=96 && signature.size()==16,"bad synthetic descriptor");
    for(std::size_t i=0;i<signature.size();++i)
        data[i]=static_cast<std::uint8_t>(signature[i]);
    data[16]=major;
    data[17]=0;
    return data;
}
void save(const fs::path& path,const Bytes& data) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    check(bool(out),"failed to open temporary MDS/MDX test file");
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    check(bool(out),"failed to write MDS/MDX test file");
}
void invalid(const fs::path& path,const Bytes& data) {
    save(path,data);
    try {
        (void)iso_wcx::probe_mds_descriptor(path);
    }catch(const std::exception&) {
        return;
    }
    throw std::runtime_error("corrupt or unsupported MDS/MDX descriptor accepted");
}
}

int main() {
    const auto dir=fs::temp_directory_path()/
        ("iso-wcx-mds-probe-"+std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto mds=dir/"test.mds";
    const auto mdx=dir/"test.mdx";
    try {
        fs::create_directories(dir);
        auto v1=descriptor(128,1);
        save(mds,v1);
        auto r=iso_wcx::probe_mds_descriptor(mds);
        check(r.kind==iso_wcx::MdsDescriptorKind::alcohol_v1 &&
              r.major==1 && r.file_bytes==128,
              "MDS version 1 recognition regression");
        invalid(mdx,v1);

        auto v2=descriptor(1024,2);
        le32(v2,0x2c,0x200);
        save(mds,v2);
        r=iso_wcx::probe_mds_descriptor(mds);
        check(r.kind==iso_wcx::MdsDescriptorKind::daemon_mds_v2 &&
              r.encrypted_header_offset==512 &&
              r.packed_metadata_offset==0x30 &&
              r.packed_metadata_length==0x1d0,
              "MDS v2 encrypted header and metadata regions not identified");
        invalid(mdx,v2);
        auto damaged=v2;
        le32(damaged,0x2c,0x2ff);
        invalid(mds,damaged); // 0x200-byte encrypted header would exceed EOF
        damaged=v2;
        le32(damaged,0x2c,0x20);
        invalid(mds,damaged); // compressed metadata starts at 0x30
        damaged=v2;
        le32(damaged,0x2c,0x30);
        invalid(mds,damaged); // no compressed metadata
        damaged=v2;
        damaged[3]='?';
        invalid(mds,damaged);
        damaged=v2;
        damaged[16]=3;
        invalid(mds,damaged);
        damaged=v2;
        damaged.resize(100);
        invalid(mds,damaged);

        auto embedded=descriptor(4096,2);
        le32(embedded,0x2c,UINT32_MAX);
        le64(embedded,0x30,0x600);
        le64(embedded,0x38,0x300);
        save(mdx,embedded);
        r=iso_wcx::probe_mds_descriptor(mdx);
        check(r.kind==iso_wcx::MdsDescriptorKind::daemon_mdx &&
              r.major==2 && r.packed_metadata_offset==0x600 &&
              r.packed_metadata_length==0x2c0 &&
              r.encrypted_header_offset==0x8c0,
              "MDX embedded metadata boundaries not identified");
        invalid(mds,embedded);
        damaged=embedded;
        le64(damaged,0x30,4090);
        invalid(mdx,damaged);
        damaged=embedded;
        le64(damaged,0x38,UINT64_MAX);
        invalid(mdx,damaged);
        damaged=embedded;
        le64(damaged,0x38,0x3f);
        invalid(mdx,damaged);
        damaged=embedded;
        le64(damaged,0x30,0);
        invalid(mdx,damaged);
        damaged=embedded;
        le64(damaged,0x30,0xf00);
        le64(damaged,0x38,0x80);
        invalid(mdx,damaged);
        damaged=embedded;
        damaged.resize(0x100);
        invalid(mdx,damaged);

        std::error_code ec;
        fs::remove_all(dir,ec);
        std::cout<<"PASS: MDSv1, MDSv2, MDX probe, encrypted header boundaries, malformed metadata\n";
        return 0;
    } catch(const std::exception& e) {
        std::error_code ec;
        fs::remove_all(dir,ec);
        std::cerr<<"FAIL: "<<e.what()<<'\n';
        return 1;
    }
}
