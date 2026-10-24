#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace iso_wcx {
namespace wim_detail {
struct Header {
    std::array<unsigned char, 208> bytes{};
    unsigned number = 0, total = 0;
};
inline unsigned u16(const Header& h, unsigned p) {
    return unsigned(h.bytes[p]) | (unsigned(h.bytes[p+1]) << 8);
}
inline std::uint32_t u32(const Header& h, unsigned p) {
    return u16(h,p) | (std::uint32_t(u16(h,p+2)) << 16);
}
inline bool read_header(const std::filesystem::path& path, Header& h) {
    std::ifstream file(path, std::ios::binary);
    file.read(reinterpret_cast<char*>(h.bytes.data()), h.bytes.size());
    constexpr std::array<unsigned char,8> magic{'M','S','W','I','M',0,0,0};
    if (!file || !std::equal(magic.begin(),magic.end(),h.bytes.begin())) return false;
    h.number = u16(h,40); h.total = u16(h,42);
    return true;
}
inline void validate(const Header& h) {
    if (u32(h,8) < h.bytes.size() || !h.number || !h.total ||
        h.number > h.total || h.total > 4096 || (u32(h,16) & 0x40))
        throw std::runtime_error("invalid, incomplete or oversized WIM part header");
}
}
// Discover by set GUID and header part number, never by filename order.
// No recursive search, wildcard reference registration, or concatenation.
inline std::vector<std::filesystem::path> resolve_wim_parts(const std::filesystem::path& input) {
    namespace fs = std::filesystem;
    using namespace wim_detail;
    const auto image = fs::absolute(input).lexically_normal();
    if (!fs::is_regular_file(fs::symlink_status(image)))
        throw std::runtime_error("WIM input must be a regular non-symlink file");
    Header selected;
    if (!read_header(image,selected)) throw std::runtime_error("invalid or truncated WIM header");
    validate(selected);
    if (selected.total == 1) return {image};
    std::vector<fs::path> parts(selected.total);
    parts[selected.number-1] = image;
    std::size_t inspected = 0;
    for (const auto& entry : fs::directory_iterator(image.parent_path())) {
        if (++inspected > 10000) throw std::runtime_error("SWM directory exceeds search limit");
        if (entry.path() == image) continue;
        auto extension = entry.path().extension().wstring();
        std::transform(extension.begin(),extension.end(),extension.begin(),
                       [](wchar_t c){ return c >= L'A' && c <= L'Z' ? c + (L'a'-L'A') : c; });
        if (extension != L".swm" || !fs::is_regular_file(entry.symlink_status())) continue;
        Header candidate;
        if (!read_header(entry.path(),candidate)) continue;
        if (!std::equal(selected.bytes.begin()+24,selected.bytes.begin()+40,
                        candidate.bytes.begin()+24)) continue;
        validate(candidate);
        if (candidate.total != selected.total || u32(candidate,12) != u32(selected,12) ||
            u32(candidate,20) != u32(selected,20) ||
            (u32(candidate,16) & 0xffff0002u) != (u32(selected,16) & 0xffff0002u))
            throw std::runtime_error("inconsistent SWM set headers");
        auto& slot = parts[candidate.number-1];
        if (!slot.empty()) throw std::runtime_error("duplicate SWM part number");
        slot = entry.path();
    }
    for (std::size_t i=0; i<parts.size(); ++i)
        if (parts[i].empty()) throw std::runtime_error("missing SWM part " + std::to_string(i+1));
    return parts;
}
}
