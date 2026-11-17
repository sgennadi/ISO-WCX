#include "iso_wcx/wim_resource_audit.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace iso_wcx::wim_detail;
namespace fs=std::filesystem;

static void require(bool cond,const char* error) {
    if(!cond)throw std::runtime_error(error);
}
static void put16(std::vector<std::uint8_t>& a,std::size_t p,std::uint16_t n) {
    for(unsigned i=0;i<2;++i)a[p+i]=static_cast<std::uint8_t>(n>>(i*8u));
}
static void put32(std::vector<std::uint8_t>& a,std::size_t p,std::uint32_t n) {
    for(unsigned i=0;i<4;++i)a[p+i]=static_cast<std::uint8_t>(n>>(i*8u));
}
static void put64(std::vector<std::uint8_t>& a,std::size_t p,std::uint64_t n) {
    for(unsigned i=0;i<8;++i)a[p+i]=static_cast<std::uint8_t>(n>>(i*8u));
}
static void store_digest(std::vector<std::uint8_t>& a,std::size_t p,
                         const WimSha1Digest& digest) {
    std::copy(digest.begin(),digest.end(),a.begin()+static_cast<std::ptrdiff_t>(p));
}
static std::vector<std::uint8_t> bytes(std::string_view v) {
    return {v.begin(),v.end()};
}
static std::string hex(const WimSha1Digest& hash) {
    constexpr char alphabet[]="0123456789abcdef";
    std::string s;
    for(auto byte:hash) {
        s.push_back(alphabet[byte>>4]);
        s.push_back(alphabet[byte&15u]);
    }
    return s;
}
static void descriptor(std::vector<std::uint8_t>& a,
                       std::size_t p,std::uint64_t stored,
                       std::uint8_t flags,std::uint64_t off,
                       const WimSha1Digest& digest) {
    for(unsigned i=0;i<7;++i)
        a[p+i]=static_cast<std::uint8_t>(stored>>(i*8u));
    a[p+7]=flags;
    put64(a,p+8,off);
    put64(a,p+16,stored);
    put16(a,p+24,1u);
    put32(a,p+26,1u);
    store_digest(a,p+30,digest);
}
static std::vector<std::uint8_t> metadata(
        const WimSha1Digest& file_hash,const WimSha1Digest& ads_hash) {
    // Security table, root, one file and one alternate stream. All sizes
    // and offsets follow the uncompressed Windows Imaging Format layout.
    std::vector<std::uint8_t> b(320);
    put32(b,0,8);
    put32(b,4,0);
    put64(b,8,104);
    put32(b,16,0x10u);
    put32(b,20,0xffffffffu);
    put64(b,24,120); // root's children at 120
    // Root sibling terminator at 112
    put64(b,120,112);
    put32(b,128,0x80u); // regular file
    put32(b,132,0xffffffffu);
    store_digest(b,184,file_hash); // child+64
    put16(b,216,1u); // child+96 has one ADS
    put16(b,220,6u);
    put16(b,222,'N');put16(b,224,'E');put16(b,226,'W');
    // child ends at 232, first stream is at 232
    put64(b,232,48u);
    store_digest(b,248,ads_hash); // stream+16
    put16(b,268,6u); // stream+36; name length 6 bytes
    put16(b,270,'A');put16(b,272,'D');put16(b,274,'S');
    // Terminate directory list at offset 280
    return b;
}
static std::vector<std::uint8_t> image(
        std::uint8_t payload_flags=0,std::uint8_t metadata_flags=2,
        bool omit_ads_resource=false) {
    const auto data=bytes("HELLO-WIM-RESOURCE");
    const auto ads=bytes("ALTERNATE-STREAM");
    const auto fh=wim_sha1(data);
    const auto sh=wim_sha1(ads);
    const auto meta=metadata(fh,sh);
    constexpr std::size_t header=208;
    const std::size_t file_at=header+meta.size();
    const std::size_t ads_at=file_at+data.size();
    const std::size_t lookup_at=ads_at+ads.size();
    const std::size_t descriptors=omit_ads_resource?2u:3u;
    std::vector<std::uint8_t> out(lookup_at+50u*descriptors);
    const std::array<std::uint8_t,8> magic{'M','S','W','I','M',0,0,0};
    std::copy(magic.begin(),magic.end(),out.begin());
    put32(out,8,208);
    put32(out,12,0x00010d00u);
    put16(out,40,1u);put16(out,42,1u);put32(out,44,1u);
    for(std::size_t i=0;i<7;++i)
        out[48+i]=static_cast<std::uint8_t>(
            (50u*descriptors)>>(8u*i));
    put64(out,56,lookup_at);
    put64(out,64,50u*descriptors);
    std::copy(meta.begin(),meta.end(),
              out.begin()+static_cast<std::ptrdiff_t>(header));
    std::copy(data.begin(),data.end(),
              out.begin()+static_cast<std::ptrdiff_t>(file_at));
    std::copy(ads.begin(),ads.end(),
              out.begin()+static_cast<std::ptrdiff_t>(ads_at));
    descriptor(out,lookup_at,meta.size(),metadata_flags,header,
               wim_sha1(meta));
    descriptor(out,lookup_at+50,data.size(),payload_flags,file_at,fh);
    if(!omit_ads_resource)
        descriptor(out,lookup_at+100,ads.size(),0,ads_at,sh);
    return out;
}
static void write(const fs::path& path,
                  const std::vector<std::uint8_t>& input) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    require(bool(out),"cannot write disposable WIM fixture");
    out.write(reinterpret_cast<const char*>(input.data()),
              static_cast<std::streamsize>(input.size()));
    require(bool(out),"cannot finish disposable WIM fixture");
}
int main(){
    const auto path=fs::temp_directory_path()/
        "iso-wcx-wim-resource-audit-readonly-fixture.wim";
    try {
        const auto empty=wim_sha1(std::span<const std::uint8_t>{});
        require(hex(empty)=="da39a3ee5e6b4b0d3255bfef95601890afd80709",
                "SHA-1 empty vector mismatch");
        require(hex(wim_sha1(bytes("abc")))==
                "a9993e364706816aba3e25717850c26c9cd0d89d",
                "SHA-1 abc vector mismatch");
        std::vector<std::uint8_t> million(1000000u,'a');
        require(hex(wim_sha1(million))==
                "34aa973cd4c4daa4f61eeb2bdbad27316534016f",
                "SHA-1 multi-block vector mismatch");
        auto original=image();
        write(path,original);
        auto good=audit_wim_resource_links(path);
        if(!good.limited_audit_complete)
            std::cerr<<"Unexpected incomplete audit: "<<good.reason<<"\n";
        require(good.metadata_verified && good.references_resolved &&
                good.limited_audit_complete && good.images==1u &&
                good.referenced_hashes==2u &&
                good.metadata_resources_verified==1u &&
                good.uncompressed_payloads_verified==2u,
                "valid metadata, normal file or ADS hashes not verified");
        {
            std::ifstream in(path,std::ios::binary);
            const std::vector<std::uint8_t> unchanged(
                (std::istreambuf_iterator<char>(in)),
                std::istreambuf_iterator<char>{});
            require(unchanged==original,"read-only audit modified WIM bytes");
        }
        auto broken=original;
        broken[208+184]^=0x80u; // metadata child file stream hash
        write(path,broken);
        require(!audit_wim_resource_links(path).metadata_verified,
                "metadata resource SHA-1 corruption accepted");
        broken=image(0,2,true);
        write(path,broken);
        const auto missing=audit_wim_resource_links(path);
        require(missing.metadata_verified && !missing.references_resolved &&
                !missing.limited_audit_complete,
                "missing ADS lookup resource accepted");
        broken=original;
        broken[208+320]^=0x01u; // corrupt plain file payload, not lookup
        write(path,broken);
        const auto payload=audit_wim_resource_links(path);
        require(payload.metadata_verified && payload.references_resolved &&
                !payload.limited_audit_complete &&
                payload.reason.find("SHA-1 mismatch")!=std::string::npos,
                "payload SHA-1 corruption accepted");
        broken=image(4); // content is marked compressed: refuse full hash claim
        write(path,broken);
        const auto compressed=audit_wim_resource_links(path);
        require(compressed.metadata_verified && compressed.references_resolved &&
                compressed.requires_decompression &&
                compressed.unverified_payloads==1u &&
                !compressed.limited_audit_complete,
                "compressed payload falsely reported verified");
        broken=image(0,6); // metadata marked compressed: no uncompressed parse
        write(path,broken);
        const auto compressed_metadata=audit_wim_resource_links(path);
        require(compressed_metadata.requires_decompression &&
                !compressed_metadata.metadata_verified &&
                !compressed_metadata.limited_audit_complete,
                "compressed metadata falsely reported verified");
        broken=original;
        // Truncated lookup entry must be rejected before metadata traversal.
        broken.resize(broken.size()-1);
        write(path,broken);
        require(!audit_wim_resource_links(path).limited_audit_complete,
                "truncated lookup entry accepted");
        fs::remove(path);
        std::cout<<"PASS: WIM SHA-1, normal streams, ADS, missing links, "
                    "truncation and compressed-resource refusal\n";
    }catch(const std::exception& e) {
        std::error_code ec;fs::remove(path,ec);
        std::cerr<<"FAIL WIM resource audit: "<<e.what()<<'\n';
        return 1;
    }
    return 0;
}
