#pragma once
// Independent bounded SHA-1 digest for READ-ONLY WIM blob verification.
// SHA-1 is required by the WIM on-disk format. It is NOT used here as a
// security authentication primitive and is never used to authorize writes.
// Standalone C++20 code: no external DLL, executable or third-party source.
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>

namespace iso_wcx::wim_detail {
using WimSha1Digest=std::array<std::uint8_t,20>;

[[nodiscard]] inline bool wim_sha1_is_zero(
        const WimSha1Digest& digest) noexcept {
    for(const auto byte:digest)if(byte!=0)return false;
    return true;
}

[[nodiscard]] inline WimSha1Digest wim_sha1(
        std::span<const std::uint8_t> bytes) {
    if(bytes.size() > std::numeric_limits<std::uint64_t>::max()/8u)
        throw std::runtime_error("SHA-1 input length overflow");
    std::array<std::uint32_t,5> state{
        0x67452301u,0xefcdab89u,0x98badcfeu,0x10325476u,0xc3d2e1f0u
    };
    const auto block=[&](const std::uint8_t* input) {
        std::array<std::uint32_t,80> words{};
        for(std::size_t i=0;i<16;++i) {
            const auto k=i*4;
            words[i]=(std::uint32_t(input[k])<<24)|
                     (std::uint32_t(input[k+1])<<16)|
                     (std::uint32_t(input[k+2])<<8)|
                     std::uint32_t(input[k+3]);
        }
        for(std::size_t i=16;i<80;++i)
            words[i]=std::rotl(words[i-3]^words[i-8]^
                               words[i-14]^words[i-16],1);
        std::uint32_t a=state[0],b=state[1],c=state[2],
                      d=state[3],e=state[4];
        for(std::size_t i=0;i<80;++i) {
            std::uint32_t f=0,k=0;
            if(i<20) {
                f=(b&c)|((~b)&d);
                k=0x5a827999u;
            }else if(i<40) {
                f=b^c^d;
                k=0x6ed9eba1u;
            }else if(i<60) {
                f=(b&c)|(b&d)|(c&d);
                k=0x8f1bbcdcu;
            }else{
                f=b^c^d;
                k=0xca62c1d6u;
            }
            const auto temp=std::rotl(a,5)+f+e+k+words[i];
            e=d;d=c;c=std::rotl(b,30);b=a;a=temp;
        }
        state[0]+=a;state[1]+=b;state[2]+=c;
        state[3]+=d;state[4]+=e;
    };
    std::size_t offset=0;
    for(;bytes.size()-offset>=64;offset+=64)
        block(bytes.data()+offset);
    std::array<std::uint8_t,128> final{};
    const auto rest=bytes.size()-offset;
    for(std::size_t i=0;i<rest;++i)final[i]=bytes[offset+i];
    final[rest]=0x80u;
    const auto final_length=rest<56?64u:128u;
    const auto bits=static_cast<std::uint64_t>(bytes.size())*8u;
    for(unsigned i=0;i<8;++i)
        final[final_length-8u+i]=
            static_cast<std::uint8_t>(bits>>(56u-8u*i));
    block(final.data());
    if(final_length==128u)block(final.data()+64);
    WimSha1Digest digest{};
    for(std::size_t i=0;i<state.size();++i) {
        for(unsigned j=0;j<4;++j)
            digest[i*4+j]=static_cast<std::uint8_t>(
                state[i]>>(24u-8u*j));
    }
    return digest;
}
} // namespace iso_wcx::wim_detail
