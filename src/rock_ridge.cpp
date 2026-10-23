#include "iso_wcx/rock_ridge.hpp"
#include "iso_wcx/legacy_formats.hpp"
#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace iso_wcx {
namespace {
constexpr std::uint64_t sector_size = 2048;
constexpr std::size_t max_ce_area = 65536;
constexpr unsigned max_ce_hops = 8;

void check(bool valid, const char* message) {
    if (!valid) throw std::runtime_error(message);
}
std::uint32_t little32(std::span<const std::uint8_t> b, std::size_t at) {
    check(at <= b.size() && 4 <= b.size() - at, "truncated SUSP number");
    return static_cast<std::uint32_t>(b[at]) |
           (static_cast<std::uint32_t>(b[at+1]) << 8) |
           (static_cast<std::uint32_t>(b[at+2]) << 16) |
           (static_cast<std::uint32_t>(b[at+3]) << 24);
}
std::size_t su_start(std::span<const std::uint8_t> record) {
    check(record.size() >= 34 && record[0] == record.size(), "invalid ISO record for SUSP");
    const auto len = static_cast<std::size_t>(record[32]);
    const auto at = 33 + len + (len % 2 == 0 ? 1 : 0);
    check(at <= record.size(), "invalid ISO System Use offset");
    return at;
}
void utf8_valid(const std::string& name) {
    // Rock Ridge names are byte strings; for the Windows Unicode WCX API only
    // well-formed UTF-8 is accepted (no surrogate or overlong encodings).
    for (std::size_t p = 0; p < name.size();) {
        auto a = static_cast<unsigned char>(name[p]);
        if (a < 0x80) { ++p; continue; }
        std::size_t n = 0;
        std::uint32_t cp = 0, minimum = 0;
        if (a >= 0xc2 && a <= 0xdf) { n = 2; cp = a & 0x1f; minimum = 0x80; }
        else if (a >= 0xe0 && a <= 0xef) { n = 3; cp = a & 0x0f; minimum = 0x800; }
        else if (a >= 0xf0 && a <= 0xf4) { n = 4; cp = a & 0x07; minimum = 0x10000; }
        else throw std::runtime_error("invalid Rock Ridge UTF-8");
        check(n <= name.size()-p, "truncated Rock Ridge UTF-8");
        for (std::size_t j = 1; j < n; ++j) {
            const auto next = static_cast<unsigned char>(name[p+j]);
            check((next & 0xc0) == 0x80, "invalid Rock Ridge UTF-8 continuation");
            cp = (cp << 6) | (next & 0x3f);
        }
        check(cp >= minimum && cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff),
              "invalid Rock Ridge Unicode codepoint");
        p += n;
    }
}
// RRIP PX fields use both-endian 32-bit integers (ECMA-119 7.3.3).
std::uint32_t mirrored32(std::span<const std::uint8_t> source,std::size_t at) {
    check(at<=source.size() && source.size()-at>=8,"truncated RRIP PX integer");
    const auto le=little32(source,at);
    const auto be=(std::uint32_t(source[at+4])<<24)|
                  (std::uint32_t(source[at+5])<<16)|
                  (std::uint32_t(source[at+6])<<8)|source[at+7];
    check(le==be,"RRIP PX integer endian mismatch");
    return le;
}
struct Continuation {std::uint32_t block{},offset{},length{};};
}

std::optional<std::uint8_t> rock_ridge_skip(std::span<const std::uint8_t> root_dot) {
    if (root_dot.size() < 34 || root_dot[0] != root_dot.size() ||
        root_dot[32] != 1 || root_dot[33] != 0) return std::nullopt;
    const auto beginning = su_start(root_dot);
    // SP is the first SUSP entry, preceded by LEN_SKP bytes if nonzero.
    for (std::size_t skip = 0; skip <= 32 && beginning+skip <= root_dot.size(); ++skip) {
        const auto p = beginning + skip;
        if (root_dot.size()-p < 7) continue;
        if (root_dot[p]=='S' && root_dot[p+1]=='P' && root_dot[p+2]==7 &&
            root_dot[p+3]==1 && root_dot[p+4]==0xbe && root_dot[p+5]==0xef &&
            root_dot[p+6]==skip) return static_cast<std::uint8_t>(skip);
    }
    return std::nullopt;
}

RockRidgeEntry rock_ridge_entry(std::span<const std::uint8_t> record,
                                std::uint8_t susp_skip,
                                const RockRidgeReader& read) {
    const auto beginning = su_start(record);
    check(susp_skip <= record.size()-beginning, "invalid Rock Ridge SUSP skip");
    RockRidgeEntry result;
    std::string name;
    bool saw_nm = false, continuing_nm = false, name_finished = false;
    bool saw_px = false, saw_tf = false;
    std::set<std::tuple<std::uint32_t,std::uint32_t,std::uint32_t>> visited;
    std::vector<std::uint8_t> continuation_data;
    std::span<const std::uint8_t> current = record.subspan(beginning+susp_skip);
    std::size_t total_ce_bytes = 0;
    for (unsigned hop = 0;; ++hop) {
        std::optional<Continuation> next;
        for (std::size_t p = 0; p+4 <= current.size();) {
            if (current[p] == 0) break; // zero padding before end of record
            auto length = static_cast<std::size_t>(current[p+2]);
            check(length >= 4 && length <= current.size()-p && current[p+3] == 1,
                  "invalid SUSP entry size or version");
            const auto a = current[p], b = current[p+1];
            if (a=='S' && b=='T') break;
            if (a=='E' && b=='R') {
                check(length>=8,"truncated SUSP ER entry");
                auto id_length=current[p+4];
                auto description_length=current[p+5];
                auto source_length=current[p+6];
                check(8u+id_length+description_length+source_length<=length,
                      "invalid SUSP ER identifier lengths");
                const std::string id(reinterpret_cast<const char*>(current.data()+p+8),id_length);
                if(id=="RRIP_1991A" || id=="IEEE_P1282")result.extension=true;
            } else if (a=='N' && b=='M') {
                check(length>=5, "truncated Rock Ridge NM record");
                const auto flags = current[p+4];
                check((flags & ~std::uint8_t{1}) == 0, "unsupported Rock Ridge NM flags");
                check(!name_finished, "extra Rock Ridge NM after final segment");
                saw_nm = true;
                check(length-5 <= 250-name.size(), "Rock Ridge filename too long");
                name.append(reinterpret_cast<const char*>(current.data()+p+5),length-5);
                continuing_nm = (flags & 1) != 0;
                if (!continuing_nm) name_finished = true;
            } else if (a=='P' && b=='X') {
                check(!saw_px && length>=12 && (length-4)%8==0,
                      "duplicate or truncated Rock Ridge PX record");
                saw_px=true;
                // Reject contradictory LE/BE copies even in unused PX fields.
                for(std::size_t at=4;at+8<=length;at+=8)
                    (void)mirrored32(current.subspan(p,length),at);
                result.posix_mode=mirrored32(current,p+4);
            } else if (a=='T' && b=='F') {
                check(!saw_tf && length>=5,"duplicate or truncated Rock Ridge TF record");
                saw_tf=true;
                const auto flags=current[p+4];
                const auto width=(flags&0x80u)?17u:7u;
                unsigned count=0;
                for(unsigned bit=0;bit<7;++bit)if(flags&(1u<<bit))++count;
                check(count>0 && length==5u+count*width,
                      "Rock Ridge TF timestamp list has invalid length");
                std::size_t at=p+5;
                for(unsigned bit=0;bit<7;++bit) {
                    if(!(flags&(1u<<bit)))continue;
                    if(bit==1) { // modification timestamp only
                        const auto stamp=(width==7)?
                            iso_dos_timestamp(current.subspan(at,width)):
                            iso_long_dos_timestamp(current.subspan(at,width));
                        if(stamp)result.modification_dos_time=stamp;
                    }
                    at+=width;
                }
            } else if (a=='S' && b=='L') {
                check(length>=5,"truncated Rock Ridge SL record");
                result.symlink = true;
            } else if (a=='R' && b=='E') {
                result.relocated = true;
            } else if (a=='C' && b=='L') {
                check(length>=12,"truncated Rock Ridge CL record");
                check(!result.child_lba,"duplicate Rock Ridge CL record");
                auto lba=little32(current,p+4);
                auto big=(std::uint32_t(current[p+8])<<24) |
                         (std::uint32_t(current[p+9])<<16) |
                         (std::uint32_t(current[p+10])<<8) | current[p+11];
                check(lba==big,"Rock Ridge CL both-endian LBA mismatch");
                result.child_link = true;
                result.child_lba = lba;
            } else if (a=='C' && b=='E') {
                check(length>=28 && !next,"invalid Rock Ridge CE entry");
                Continuation ce{little32(current,p+4),little32(current,p+12),little32(current,p+20)};
                check(ce.offset < sector_size && ce.length > 0 && ce.length <= max_ce_area,
                      "invalid Rock Ridge CE extent");
                next = ce;
            }
            p += length;
        }
        if (!next) break;
        check(hop < max_ce_hops && total_ce_bytes <= max_ce_area-next->length,
              "Rock Ridge continuation limit exceeded");
        check(visited.insert({next->block,next->offset,next->length}).second,
              "cyclic Rock Ridge CE continuation");
        total_ce_bytes += next->length;
        continuation_data = read(static_cast<std::uint64_t>(next->block)*sector_size + next->offset,
                                 next->length);
        check(continuation_data.size() == next->length,"truncated Rock Ridge continuation");
        current = continuation_data;
    }
    if (saw_nm) {
        check(name_finished && !continuing_nm && !name.empty(), "incomplete Rock Ridge NM name");
        utf8_valid(name);
        result.alternate_name = std::move(name);
    }
    return result;
}
} // namespace iso_wcx
