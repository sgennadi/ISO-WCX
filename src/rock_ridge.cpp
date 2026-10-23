#include "iso_wcx/rock_ridge.hpp"
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
            } else if (a=='S' && b=='L') {
                check(length>=5,"truncated Rock Ridge SL record");
                result.symlink = true;
            } else if (a=='R' && b=='E') {
                result.relocated = true;
            } else if (a=='C' && b=='L') {
                check(length>=12,"truncated Rock Ridge CL record");
                result.child_link = true;
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
