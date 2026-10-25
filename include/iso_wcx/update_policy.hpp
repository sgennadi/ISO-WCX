#pragma once
// Pure updater policy. The GUI/download implementation is Windows-only;
// these validators can be unit-tested on all native build platforms.
#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace iso_wcx::update {
struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;
    friend bool operator==(const Version&, const Version&) = default;
};
inline bool newer(Version remote, Version installed) {
    if (remote.major != installed.major) return remote.major > installed.major;
    if (remote.minor != installed.minor) return remote.minor > installed.minor;
    return remote.patch > installed.patch;
}
inline std::optional<Version> parse_version(std::string_view s) {
    if (s.size() < 6 || s.size() > 28 || s[0] != 'v') return std::nullopt;
    Version v{};
    std::array<int*, 3> parts{&v.major, &v.minor, &v.patch};
    std::size_t at = 1;
    for (unsigned index = 0; index < parts.size(); ++index) {
        int count = 0;
        int value = 0;
        while (at < s.size() && s[at] >= '0' && s[at] <= '9') {
            if (++count > 7) return std::nullopt;
            value = value * 10 + (s[at++] - '0');
        }
        if (count == 0) return std::nullopt;
        *parts[index] = value;
        if (index < 2) {
            if (at >= s.size() || s[at++] != '.') return std::nullopt;
        }
    }
    if (at != s.size()) return std::nullopt;
    return v;
}
inline std::optional<std::string> release_tag(std::string_view json) {
    constexpr std::string_view needle = "\"tag_name\"";
    const auto pos = json.find(needle);
    if (pos == std::string_view::npos) return std::nullopt;
    auto at = pos + needle.size();
    while (at < json.size() && (json[at] == ' ' || json[at] == '\t' ||
                                json[at] == '\n' || json[at] == '\r')) ++at;
    if (at >= json.size() || json[at++] != ':') return std::nullopt;
    while (at < json.size() && (json[at] == ' ' || json[at] == '\t' ||
                                json[at] == '\n' || json[at] == '\r')) ++at;
    if (at >= json.size() || json[at++] != '"') return std::nullopt;
    const auto begin = at;
    while (at < json.size() && json[at] != '"' && at-begin <= 28) ++at;
    if (at >= json.size() || json[at] != '"') return std::nullopt;
    const auto candidate = std::string(json.substr(begin, at-begin));
    return parse_version(candidate) ? std::optional(candidate) : std::nullopt;
}
inline bool hex_character(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}
inline std::optional<std::string> checksum(std::string_view manifest,
                                           std::string_view exact_filename) {
    if (exact_filename.empty() || exact_filename.size() > 128 ||
        exact_filename.find('/') != std::string_view::npos ||
        exact_filename.find('\\') != std::string_view::npos)
        return std::nullopt;
    std::optional<std::string> value;
    std::size_t offset = 0;
    while (offset < manifest.size()) {
        auto end = manifest.find('\n', offset);
        if (end == std::string_view::npos) end = manifest.size();
        auto line = manifest.substr(offset, end-offset);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.size() == exact_filename.size()+66 &&
            line[64] == ' ' && (line[65] == ' ' || line[65] == '*') &&
            line.substr(66) == exact_filename) {
            std::string found;
            found.reserve(64);
            for (unsigned i = 0; i < 64; ++i) {
                if (!hex_character(line[i])) return std::nullopt;
                const auto c = line[i];
                found.push_back(c >= 'A' && c <= 'F' ? char(c-'A'+'a') : c);
            }
            if (value) return std::nullopt; // ambiguous duplicate entry
            value = std::move(found);
        }
        offset = end+1;
    }
    return value;
}
} // namespace iso_wcx::update
