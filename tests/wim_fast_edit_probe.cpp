#include "iso_wcx/wim_fast_edit_probe.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using iso_wcx::wim_detail::probe_fast_edit_metadata;

static void require(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
}
static void le16(std::uint8_t* p, std::uint16_t v) {
    for(unsigned i=0;i<2;i++)p[i]=static_cast<std::uint8_t>(v>>(8*i));
}
static void le32(std::uint8_t* p, std::uint32_t v) {
    for(unsigned i=0;i<4;i++)p[i]=static_cast<std::uint8_t>(v>>(8*i));
}
static void le64(std::uint8_t* p, std::uint64_t v) {
    for(unsigned i=0;i<8;i++)p[i]=static_cast<std::uint8_t>(v>>(8*i));
}
static void res(std::uint8_t* p, std::uint64_t stored, std::uint8_t flags,
                std::uint64_t offset, std::uint64_t original) {
    for(unsigned i=0;i<7;i++)p[i]=static_cast<std::uint8_t>(stored>>(8*i));
    p[7]=flags;
    le64(p+8,offset);
    le64(p+16,original);
}
static fs::path make_fixture(const fs::path& where, bool compressed=false) {
    std::array<std::uint8_t,386> bytes{};
    const char magic[8]={'M','S','W','I','M',0,0,0};
    for(unsigned i=0;i<8;i++)bytes[i]=static_cast<std::uint8_t>(magic[i]);
    le32(bytes.data()+8,208);
    le32(bytes.data()+12,0x00010d00);
    le16(bytes.data()+40,1);
    le16(bytes.data()+42,1);
    le32(bytes.data()+44,1);
    res(bytes.data()+48,50,0,336,50);
    res(bytes.data()+336,128,compressed?6:2,208,128);
    le16(bytes.data()+336+24,1);
    std::ofstream out(where,std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    require(static_cast<bool>(out),"cannot write fixture");
    return where;
}
int main() {
    const auto file=fs::temp_directory_path()/"iso-wcx-wim-fast-probe-test.wim";
    try {
        make_fixture(file);
        auto a=probe_fast_edit_metadata(file);
        require(a.header_and_lookup_valid && a.metadata_layout_candidate,
                "uncompressed metadata layout not recognized");
        require(a.images==1 && a.lookup_entries==1 &&
                a.metadata_resources==1 && a.metadata_uncompressed_bytes==128,
                "lookup/metadata count mismatch");
        make_fixture(file,true);
        auto b=probe_fast_edit_metadata(file);
        require(b.header_and_lookup_valid && !b.metadata_layout_candidate &&
                b.compressed_metadata_resources==1,
                "compressed metadata must not qualify for uncompressed candidate");
        make_fixture(file);
        {
            std::fstream io(file,std::ios::binary|std::ios::in|std::ios::out);
            io.seekp(336+7);
            const char flags=static_cast<char>(0x0a); // metadata + spanned
            io.write(&flags,1);
        }
        require(!probe_fast_edit_metadata(file).header_and_lookup_valid,
                "spanned/unknown metadata resource flags accepted");
        make_fixture(file);
        {
            std::fstream io(file,std::ios::binary|std::ios::in|std::ios::out);
            io.seekp(48+8);
            std::array<std::uint8_t,8> corrupt{};
            le64(corrupt.data(),0xfffffffffffffff0ull);
            io.write(reinterpret_cast<const char*>(corrupt.data()),corrupt.size());
        }
        require(!probe_fast_edit_metadata(file).header_and_lookup_valid,
                "out-of-bounds lookup offset accepted");
        make_fixture(file);
        {
            std::fstream io(file,std::ios::binary|std::ios::in|std::ios::out);
            io.seekp(336+8);
            std::array<std::uint8_t,8> corrupt{};
            le64(corrupt.data(),340);
            io.write(reinterpret_cast<const char*>(corrupt.data()),corrupt.size());
        }
        require(!probe_fast_edit_metadata(file).header_and_lookup_valid,
                "metadata overlapping lookup table accepted");
        make_fixture(file);
        {
            std::fstream io(file,std::ios::binary|std::ios::in|std::ios::out);
            io.seekp(16);
            std::array<std::uint8_t,4> readonly{};
            le32(readonly.data(),0x04);
            io.write(reinterpret_cast<const char*>(readonly.data()),readonly.size());
        }
        auto c=probe_fast_edit_metadata(file);
        require(c.header_and_lookup_valid && !c.metadata_layout_candidate,
                "read-only WIM must not qualify");
        fs::remove(file);
        std::cout<<"PASS: read-only WIM lookup/metadata preflight and bounds\n";
        return 0;
    }catch(const std::exception& e){
        std::error_code ec;fs::remove(file,ec);
        std::cerr<<"FAIL: "<<e.what()<<"\n";
        return 1;
    }
}
