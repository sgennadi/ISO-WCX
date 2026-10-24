#include "iso_wcx/mds_v2.hpp"
#include "iso_wcx/mds_probe.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <zlib.h>

extern "C" {
#define AES256 1
#define ECB 1
#define CBC 0
#define CTR 0
#include "../third_party/tiny_aes/aes.h"
}

namespace iso_wcx {
namespace {
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

void require(bool valid, const char* message) {
    if (!valid) throw std::runtime_error(message);
}
constexpr std::size_t max_descriptor = 16u * 1024u * 1024u;
constexpr std::array<std::uint8_t, 64> empty_salt{};

// Message orders and rotations defined in the public RIPEMD-160 standard.
// The algorithm is implemented in-source so no crypto DLL is distributed.
constexpr std::array<int,80> R{
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
    7,4,13,1,10,6,15,3,12,0,9,5,2,14,11,8,
    3,10,14,4,9,15,8,1,2,7,0,6,13,11,5,12,
    1,9,11,10,0,8,12,4,13,3,7,15,14,5,6,2,
    4,0,5,9,7,12,2,10,14,1,3,8,11,6,15,13
};
constexpr std::array<int,80> Rp{
    5,14,7,0,9,2,11,4,13,6,15,8,1,10,3,12,
    6,11,3,7,0,13,5,10,14,15,8,12,4,9,1,2,
    15,5,1,3,7,14,6,9,11,8,12,2,10,0,4,13,
    8,6,4,1,3,11,15,0,5,12,2,13,9,7,10,14,
    12,15,10,4,1,5,8,7,6,2,13,14,0,3,9,11
};
constexpr std::array<int,80> S{
    11,14,15,12,5,8,7,9,11,13,14,15,6,7,9,8,
    7,6,8,13,11,9,7,15,7,12,15,9,11,7,13,12,
    11,13,6,7,14,9,13,15,14,8,13,6,5,12,7,5,
    11,12,14,15,14,15,9,8,9,14,5,6,8,6,5,12,
    9,15,5,11,6,8,13,12,5,12,13,14,11,8,5,6
};
constexpr std::array<int,80> Sp{
    8,9,9,11,13,15,15,5,7,7,8,11,14,14,12,6,
    9,13,15,7,12,8,9,11,7,7,12,7,6,15,13,11,
    9,7,15,11,8,6,6,14,12,13,5,14,13,13,7,5,
    15,5,8,11,14,14,6,14,6,9,12,9,12,5,15,8,
    8,5,12,9,12,5,14,6,8,13,6,5,15,13,11,11
};
constexpr std::array<u32,5> K{
    0x00000000u,0x5A827999u,0x6ED9EBA1u,0x8F1BBCDCu,0xA953FD4Eu
};
constexpr std::array<u32,5> Kp{
    0x50A28BE6u,0x5C4DD124u,0x6D703EF3u,0x7A6D76E9u,0x00000000u
};
u32 f(int step,u32 x,u32 y,u32 z) noexcept {
    if(step<16)return x^y^z;
    if(step<32)return (x&y)|(~x&z);
    if(step<48)return (x|~y)^z;
    if(step<64)return (x&z)|(y&~z);
    return x^(y|~z);
}
u32 little32(const u8* p) noexcept {
    return u32(p[0])|(u32(p[1])<<8)|(u32(p[2])<<16)|(u32(p[3])<<24);
}
u64 little64(const u8* p) noexcept {
    return u64(little32(p))|(u64(little32(p+4))<<32);
}
void put32(u8* p,u32 x) noexcept {
    for (int i=0;i<4;i++) p[i]=u8(x>>(i*8));
}
void put64(u8* p,u64 x) noexcept {
    for (int i=0;i<8;i++) p[i]=u8(x>>(i*8));
}
std::array<u8,20> hmac_ripemd160(std::span<const u8> key,
                               std::span<const u8> message) {
    require(key.size()<=64,"MDS PBKDF2 password limit exceeded");
    std::array<u8,64> ipad{},opad{};
    ipad.fill(0x36);
    opad.fill(0x5c);
    for(std::size_t i=0;i<key.size();++i) {
        ipad[i]^=key[i];
        opad[i]^=key[i];
    }
    std::vector<u8> inner(ipad.begin(),ipad.end());
    inner.insert(inner.end(),message.begin(),message.end());
    const auto a=mds_ripemd160(inner);
    std::vector<u8> outer(opad.begin(),opad.end());
    outer.insert(outer.end(),a.begin(),a.end());
    return mds_ripemd160(outer);
}
std::array<u8,152> pbkdf2(std::span<const u8> password,
                          std::span<const u8> salt) {
    std::array<u8,152> result{};
    std::vector<u8> block(salt.begin(),salt.end());
    block.resize(salt.size()+4,0);
    for(u32 index=1;index<=8;++index) {
        block[block.size()-4] = u8(index>>24);
        block[block.size()-3] = u8(index>>16);
        block[block.size()-2] = u8(index>>8);
        block[block.size()-1] = u8(index);
        auto u = hmac_ripemd160(password,block);
        auto x = u;
        for(int round=1;round<2000;++round) {
            u=hmac_ripemd160(password,u);
            for(std::size_t i=0;i<u.size();++i)x[i]^=u[i];
        }
        const std::size_t out=(index-1)*20u;
        std::copy_n(x.begin(),std::min<std::size_t>(20,result.size()-out),
                    result.begin()+static_cast<std::ptrdiff_t>(out));
    }
    return result;
}
u32 crc_edc(std::span<const u8> data) {
    u32 value=0;
    for(const auto octet:data) {
        value ^= octet;
        for(int i=0;i<8;++i)
            value = (value>>1) ^ ((value&1u)?0xD8018001u:0u);
    }
    return value;
}
std::array<u8,64> unsalted_password(std::span<const u8> salt) {
    require(salt.size()==64,"MDS descriptor salt must be 64 bytes");
    std::array<u8,64> result{};
    std::copy(salt.begin(),salt.end(),result.begin());
    u32 modifier=crc_edc(result)^0x567372ffu;
    for(std::size_t i=0;i<16;i++) {
        u32 word=little32(result.data()+i*4);
        modifier=modifier*0x35e85a6du+0x1548dce9u;
        word ^= modifier ^ 0xec564717u;
        for(int octet=0;octet<4;++octet)
            if(((word>>(octet*8))&255u)==0)word |= 0x5fu<<(octet*8);
        put32(result.data()+i*4,word);
    }
    return result;
}
void decipher_cbc(std::span<u8> data,std::span<const u8> key,
                  std::span<const u8> iv) {
    require(key.size()==32 && iv.size()==16 &&
            (data.size()%16)==0,
            "MDS descriptor AES-256 CBC block geometry invalid");
    AES_ctx aes{};
    AES_init_ctx(&aes,key.data());
    const auto init0=little64(iv.data());
    const auto init1=little64(iv.data()+8);
    for(std::size_t group=0;group<data.size();group+=512) {
        const auto group_end=std::min(data.size(),group+512);
        u64 chain0=init0,chain1=init1;
        for(std::size_t at=group;at<group_end;at+=16) {
            auto* p=data.data()+at;
            const u64 cipher0=little64(p)^init1;
            const u64 cipher1=little64(p+8)^init1;
            put64(p,cipher0);
            put64(p+8,cipher1);
            AES_ECB_decrypt(&aes,p);
            const u64 clear0=little64(p)^chain0;
            const u64 clear1=little64(p+8)^chain1;
            put64(p,clear0);
            put64(p+8,clear1);
            chain0=cipher0;
            chain1=cipher1;
        }
    }
}
std::vector<u8> read_at(std::ifstream& in,u64 at,std::size_t length) {
    require(at<=static_cast<u64>(std::numeric_limits<std::streamoff>::max()),
            "MDS metadata file offset is beyond stream range");
    std::vector<u8> bytes(length);
    in.clear();
    in.seekg(static_cast<std::streamoff>(at),std::ios::beg);
    require(bool(in),"MDS metadata seek failed");
    in.read(reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(length));
    require(static_cast<std::size_t>(in.gcount())==length,
            "MDS descriptor is truncated");
    return bytes;
}
} // namespace

std::array<std::uint8_t,20> mds_ripemd160(
    std::span<const std::uint8_t> input) {
    require(input.size()<=max_descriptor,
            "RIPEMD160 input exceeds descriptor safety limit");
    const auto padded_size=((input.size()+9+63)/64)*64;
    std::vector<u8> padded(padded_size,0);
    std::copy(input.begin(),input.end(),padded.begin());
    padded[input.size()]=0x80;
    put64(padded.data()+padded.size()-8,u64(input.size())*8);
    u32 h0=0x67452301u,h1=0xEFCDAB89u,h2=0x98BADCFEu,
        h3=0x10325476u,h4=0xC3D2E1F0u;
    for(std::size_t at=0;at<padded.size();at+=64) {
        std::array<u32,16> x{};
        for(int i=0;i<16;++i)x[i]=little32(padded.data()+at+i*4);
        u32 a=h0,b=h1,c=h2,d=h3,e=h4;
        u32 ap=h0,bp=h1,cp=h2,dp=h3,ep=h4;
        for(int j=0;j<80;++j) {
            const u32 t=std::rotl(a+f(j,b,c,d)+x[R[j]]+K[j/16],S[j])+e;
            a=e;e=d;d=std::rotl(c,10);c=b;b=t;
            const u32 tp=std::rotl(ap+f(79-j,bp,cp,dp)+x[Rp[j]]+Kp[j/16],Sp[j])+ep;
            ap=ep;ep=dp;dp=std::rotl(cp,10);cp=bp;bp=tp;
        }
        const u32 next0=h1+c+dp;
        h1=h2+d+ep;
        h2=h3+e+ap;
        h3=h4+a+bp;
        h4=h0+b+cp;
        h0=next0;
    }
    std::array<u8,20> output{};
    put32(output.data(),h0);put32(output.data()+4,h1);
    put32(output.data()+8,h2);put32(output.data()+12,h3);
    put32(output.data()+16,h4);
    return output;
}

std::vector<std::uint8_t> decode_mds_v2_descriptor(
    const std::filesystem::path& image) {
    const auto probe=probe_mds_descriptor(image);
    require(probe.kind==MdsDescriptorKind::daemon_mds_v2 ||
            probe.kind==MdsDescriptorKind::daemon_mdx,
            "native MDS v2 decoder requires an encrypted MDSv2/MDX descriptor");
    require(probe.packed_metadata_length>0 &&
            probe.packed_metadata_length<=max_descriptor+15 &&
            probe.encrypted_header_offset+512<=probe.file_bytes,
            "MDS v2 encrypted metadata extent exceeds safety bounds");
    std::ifstream file(image,std::ios::binary);
    require(bool(file),"cannot open MDS v2 descriptor");
    auto header=read_at(file,probe.encrypted_header_offset,512);
    const auto password=unsalted_password(
        std::span<const u8>(header.data(),64));
    const auto key=pbkdf2(password,std::span<const u8>(header.data(),64));
    decipher_cbc(std::span<u8>(header.data()+64,header.size()-64),
                 std::span<const u8>(key.data()+32,32),
                 std::span<const u8>(key.data(),16));
    require(little32(header.data()+68)==0x54525545u &&
            header[74]==0 && header[75]==1,
            "MDS v2 descriptor AES-256 encryption header is invalid");
    const auto crc=::crc32(0u,header.data()+80,256);
    require(crc==little32(header.data()+64),
            "MDS v2 decrypted descriptor key checksum mismatch");
    const auto packed=little32(header.data()+336);
    const auto unpacked=little32(header.data()+340);
    const u64 aligned=(u64(packed)+15u)&~u64(15);
    require(packed>0 && aligned==probe.packed_metadata_length &&
            unpacked>0 && unpacked<=max_descriptor,
            "MDS v2 compressed/decompressed descriptor length is invalid");

    auto payload=read_at(file,probe.packed_metadata_offset,
                         static_cast<std::size_t>(aligned));
    decipher_cbc(payload,
        std::span<const u8>(header.data()+80+32,32),
        std::span<const u8>(header.data()+80,16));
    std::vector<u8> result(std::size_t(unpacked)+18u,0);
    const auto first=read_at(file,0,18);
    std::copy(first.begin(),first.end(),result.begin());
    uLongf produced=unpacked;
    const int zstatus=::uncompress(result.data()+18,&produced,
                 payload.data(),static_cast<uLong>(packed));
    require(zstatus==Z_OK && produced==unpacked,
            "MDS v2 encrypted descriptor zlib decompression failed");
    require(result.size()>=0x54 &&
            std::memcmp(result.data(),"MEDIA DESCRIPTOR",16)==0 &&
            result[16]==2,
            "MDS v2 decrypted descriptor signature is invalid");
    return result;
}
} // namespace iso_wcx
