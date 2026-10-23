#include "iso_wcx/subq.hpp"

#include <array>
#include <cstddef>
#include <fstream>
#include <map>
#include <stdexcept>
#include <tuple>

namespace iso_wcx {
namespace {
[[nodiscard]] bool bcd(std::uint8_t b) noexcept {
    return (b & 15u) <= 9u && (b >> 4u) <= 9u;
}
[[nodiscard]] std::uint8_t from_bcd(std::uint8_t b) noexcept {
    return static_cast<std::uint8_t>(10u*(b >> 4u) + (b & 15u));
}
[[nodiscard]] bool msf(std::span<const std::uint8_t,3> s) noexcept {
    return bcd(s[0]) && bcd(s[1]) && bcd(s[2]) &&
           from_bcd(s[1]) < 60u && from_bcd(s[2]) < 75u;
}
[[nodiscard]] std::uint32_t to_frames(std::span<const std::uint8_t,3> s) noexcept {
    return (static_cast<std::uint32_t>(from_bcd(s[0]))*60u + from_bcd(s[1]))*75u + from_bcd(s[2]);
}
struct Range {
    std::uint64_t first{}, last{}, count{};
};
}

std::array<std::uint8_t,12> extract_subq(
    std::span<const std::uint8_t,96> packet, SubQLayout layout) noexcept {
    std::array<std::uint8_t,12> q{};
    if(layout==SubQLayout::deinterleaved) {
        for(std::size_t i=0;i<12;++i)q[i]=packet[12+i];
    } else {
        // Raw interleaved P-W bits: Q lives in bit 6 of every byte.
        for(std::size_t i=0;i<96;++i) {
            if(packet[i] & 0x40u)q[i/8] |= static_cast<std::uint8_t>(0x80u >> (i%8));
        }
    }
    return q;
}

bool subq_crc_valid(std::span<const std::uint8_t,12> q) noexcept {
    // ECMA-130: CCITT (x^16 + x^12 + x^5 + 1), initial 0, final complement.
    std::uint16_t crc=0;
    for(std::size_t i=0;i<10;++i) {
        crc ^= static_cast<std::uint16_t>(q[i]) << 8;
        for(unsigned b=0;b<8;++b)
            crc=static_cast<std::uint16_t>((crc<<1) ^ ((crc & 0x8000u) ? 0x1021u : 0u));
    }
    crc ^= 0xffffu;
    return q[10]==static_cast<std::uint8_t>(crc>>8) &&
           q[11]==static_cast<std::uint8_t>(crc & 0xffu);
}

std::optional<SubQPosition> decode_subq_position(
    std::span<const std::uint8_t,12> q) noexcept {
    if(!subq_crc_valid(q) || (q[0]&15u)!=1u || !bcd(q[1]) || !bcd(q[2]) ||
       q[6]!=0u || !msf(q.subspan<3,3>()) || !msf(q.subspan<7,3>()))
        return std::nullopt;
    const auto track=from_bcd(q[1]);
    if(track==0 || track>99)return std::nullopt; // lead-in/out are excluded
    return SubQPosition{static_cast<std::uint8_t>(q[0]>>4), track, from_bcd(q[2]),
                        to_frames(q.subspan<3,3>()), to_frames(q.subspan<7,3>())};
}

std::string subq_index_report(const std::filesystem::path& sub_file) {
    constexpr std::uint64_t kSectorBytes=96, kMaxPackets=1000000;
    if(!std::filesystem::is_regular_file(sub_file) || std::filesystem::is_symlink(sub_file))
        throw std::runtime_error("unsafe CloneCD SUB path");
    const auto bytes=std::filesystem::file_size(sub_file);
    if(bytes%kSectorBytes || bytes/kSectorBytes>kMaxPackets)
        throw std::runtime_error("invalid or oversized CloneCD SUB file");
    std::ifstream input(sub_file,std::ios::binary);
    if(!input)throw std::runtime_error("cannot open CloneCD SUB file");
    std::array<std::uint8_t,96> packet{};
    std::map<std::tuple<SubQLayout,std::uint8_t,std::uint8_t>,Range> ranges;
    std::uint64_t valid=0, bad=0, unrecognized=0;
    for(std::uint64_t sector=0;sector<bytes/kSectorBytes;++sector) {
        if(!input.read(reinterpret_cast<char*>(packet.data()),packet.size()))
            throw std::runtime_error("truncated CloneCD SUB read");
        const auto direct=extract_subq(packet,SubQLayout::deinterleaved);
        const auto packed=extract_subq(packet,SubQLayout::interleaved);
        // Prefer CloneCD's deinterleaved P12/Q12 representation if both
        // independently pass CRC; using the fallback permits variant dumps.
        const bool direct_crc=subq_crc_valid(direct);
        const bool packed_crc=subq_crc_valid(packed);
        if(!direct_crc && !packed_crc){++bad;continue;}
        ++valid;
        const auto layout=direct_crc?SubQLayout::deinterleaved:SubQLayout::interleaved;
        const auto pos=decode_subq_position(direct_crc?direct:packed);
        if(!pos){++unrecognized;continue;}
        auto& row=ranges[{layout,pos->track,pos->index}];
        if(!row.count)row.first=sector;
        row.last=sector;
        ++row.count;
    }
    std::string result="# CloneCD SUB Q-channel summary; only CRC-valid frames are interpreted\n";
    result+="# physical_sectors,"+std::to_string(bytes/kSectorBytes)+"\n";
    result+="# crc_valid,"+std::to_string(valid)+"\n";
    result+="# crc_failed,"+std::to_string(bad)+"\n";
    result+="# crc_valid_other_adr_or_invalid_position,"+std::to_string(unrecognized)+"\n";
    result+="layout,track,index,first_img_sector,last_img_sector,valid_frames\n";
    for(const auto& [key,row]:ranges) {
        const auto [layout,track,index]=key;
        result+=(layout==SubQLayout::deinterleaved?"deinterleaved,":"interleaved,");
        result+=std::to_string(track)+","+std::to_string(index)+","+
                std::to_string(row.first)+","+std::to_string(row.last)+","+
                std::to_string(row.count)+"\n";
    }
    return result;
}
} // namespace iso_wcx
