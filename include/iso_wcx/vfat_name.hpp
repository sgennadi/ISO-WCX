#pragma once
// VFAT long filename codec for native FAT12/16/32 writers.
// Independent implementation of the on-disk specification; no external code.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace iso_wcx::vfat {

inline void verify(bool condition, const char* why) {
    if (!condition) throw std::runtime_error(why);
}
inline std::u16string utf16(std::string_view name) {
    verify(!name.empty(), "empty VFAT name");
    std::u16string out;
    for (std::size_t i=0; i<name.size();) {
        const auto first = static_cast<unsigned char>(name[i++]);
        std::uint32_t cp = 0;
        unsigned continuation = 0;
        std::uint32_t minimum = 0;
        if (first < 0x80) cp = first;
        else if (first >= 0xc2 && first <= 0xdf) {
            cp = first & 0x1fu; continuation = 1; minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            cp = first & 0x0fu; continuation = 2; minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            cp = first & 0x07u; continuation = 3; minimum = 0x10000;
        } else throw std::runtime_error("invalid VFAT UTF-8 leading byte");
        verify(continuation <= name.size()-i, "truncated VFAT UTF-8");
        for (unsigned k=0; k<continuation; ++k) {
            const auto part = static_cast<unsigned char>(name[i++]);
            verify((part & 0xc0u) == 0x80u, "invalid VFAT UTF-8 continuation");
            cp = (cp<<6) | (part & 0x3fu);
        }
        verify((continuation == 0 || cp >= minimum) &&
               cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff) &&
               cp >= 32 && cp != 0x7f && cp != 0xfffe && cp != 0xffff,
               "invalid VFAT Unicode scalar");
        if (cp < 0x10000) out.push_back(static_cast<char16_t>(cp));
        else {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xd800 + (cp>>10)));
            out.push_back(static_cast<char16_t>(0xdc00 + (cp & 0x3ff)));
        }
        verify(out.size() <= 255, "VFAT name exceeds 255 UTF-16 units");
    }
    return out;
}
inline void put_scalar(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) out.push_back(static_cast<char>(cp));
    else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xc0 | (cp>>6)));
        out.push_back(static_cast<char>(0x80 | (cp & 63)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xe0 | (cp>>12)));
        out.push_back(static_cast<char>(0x80 | ((cp>>6) & 63)));
        out.push_back(static_cast<char>(0x80 | (cp & 63)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (cp>>18)));
        out.push_back(static_cast<char>(0x80 | ((cp>>12) & 63)));
        out.push_back(static_cast<char>(0x80 | ((cp>>6) & 63)));
        out.push_back(static_cast<char>(0x80 | (cp & 63)));
    }
}
inline std::string utf8(std::span<const char16_t> units) {
    std::string out;
    bool ended = false;
    for (std::size_t i=0; i<units.size(); ++i) {
        const auto cp = static_cast<std::uint32_t>(units[i]);
        if (ended) {
            verify(cp == 0 || cp == 0xffff, "invalid VFAT UTF-16 tail");
            continue;
        }
        if (cp == 0 || cp == 0xffff) { ended = true; continue; }
        if (cp >= 0xd800 && cp <= 0xdbff) {
            verify(++i < units.size(), "truncated VFAT surrogate pair");
            const auto lo = static_cast<std::uint32_t>(units[i]);
            verify(lo >= 0xdc00 && lo <= 0xdfff, "invalid VFAT surrogate pair");
            put_scalar(out, 0x10000 + ((cp - 0xd800) << 10) + lo - 0xdc00);
        } else {
            verify(cp < 0xdc00 || cp > 0xdfff, "orphan VFAT low surrogate");
            verify(cp >= 32 && cp != 0x7f, "VFAT control character in name");
            put_scalar(out, cp);
        }
        verify(out.size() <= 1024, "VFAT name exceeds decoded limit");
    }
    verify(!out.empty(), "empty VFAT long name");
    return out;
}
inline constexpr std::array<std::size_t,13> positions{
    1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30
};
inline std::uint8_t checksum(const std::uint8_t* short_name) {
    std::uint8_t value = 0;
    for (unsigned i=0; i<11; ++i)
        value = static_cast<std::uint8_t>(((value & 1u)<<7) +
                                           (value>>1) + short_name[i]);
    return value;
}
inline std::size_t required_slots(std::string_view name) {
    return (utf16(name).size()+12)/13;
}
inline std::string decode(std::span<const std::uint8_t> disk,
                          const std::vector<std::size_t>& offsets,
                          std::size_t short_at) {
    verify(!offsets.empty() && offsets.size() <= 20, "invalid VFAT slot count");
    verify(short_at <= disk.size() && disk.size()-short_at >= 32,
           "VFAT short entry outside image");
    const auto sum = checksum(disk.data()+short_at);
    std::vector<char16_t> units(offsets.size()*13, static_cast<char16_t>(0xffff));
    for (std::size_t i=0; i<offsets.size(); ++i) {
        const auto at = offsets[i];
        verify(at <= disk.size() && disk.size()-at >= 32, "VFAT slot outside image");
        const auto ordinal = static_cast<unsigned>(offsets.size()-i);
        verify((disk[at] & 0x1fu) == ordinal &&
               ((disk[at] & 0x40u)!=0) == (i==0) &&
               (disk[at] & 0xa0u) == 0 &&
               disk[at+11] == 0x0f && disk[at+12] == 0 &&
               disk[at+13] == sum && disk[at+26] == 0 && disk[at+27] == 0,
               "damaged VFAT ordinal or checksum");
        for (std::size_t j=0; j<13; ++j) {
            const auto p = at + positions[j];
            const char16_t u = static_cast<char16_t>(
                std::uint16_t(disk[p]) | (std::uint16_t(disk[p+1])<<8));
            units[(ordinal-1)*13 + j] = u;
        }
    }
    return utf8(units);
}
inline void encode(std::span<std::uint8_t> disk,
                   const std::vector<std::size_t>& slots,
                   std::string_view name,
                   const std::array<std::uint8_t,11>& alias) {
    const auto units = utf16(name);
    verify(!slots.empty() && slots.size() == (units.size()+12)/13 &&
           slots.size() <= 20, "VFAT output slot count mismatch");
    const auto sum = checksum(alias.data());
    for (std::size_t i=0; i<slots.size(); ++i) {
        const auto at = slots[i];
        verify(at <= disk.size() && disk.size()-at >= 32, "VFAT write outside image");
        const std::size_t ordinal = slots.size()-i;
        for (std::size_t j=0; j<32; ++j) disk[at+j] = 0xff;
        disk[at] = static_cast<std::uint8_t>(ordinal | (i == 0 ? 0x40u : 0u));
        disk[at+11] = 0x0f;
        disk[at+12] = 0;
        disk[at+13] = sum;
        disk[at+26] = 0;
        disk[at+27] = 0;
        for (std::size_t j=0; j<13; ++j) {
            const auto index = (ordinal-1)*13 + j;
            const auto value = index < units.size() ?
                static_cast<std::uint16_t>(units[index]) :
                static_cast<std::uint16_t>(index == units.size() ? 0 : 0xffff);
            disk[at+positions[j]] = static_cast<std::uint8_t>(value);
            disk[at+positions[j]+1] = static_cast<std::uint8_t>(value>>8);
        }
    }
}
} // namespace iso_wcx::vfat
