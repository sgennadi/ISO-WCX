#include "iso_wcx/chd_stream.hpp"

#include <libchdr/chd.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace iso_wcx {
namespace {
void require(bool valid, const char* message) {
    if (!valid) throw std::runtime_error(message);
}
constexpr std::uint64_t max_logical = std::uint64_t(1) << 44;
constexpr std::uint32_t max_hunk = 1024u * 1024u;
constexpr std::uint32_t iso_sector = 2048;
constexpr std::uint32_t cd_frame = 2448;

// Limited, conservative CHD CD track interpretation. Do not guess offsets on
// audio, mixed-mode or disc images with pregap arrangements we do not yet map.
std::string get_field(std::string_view metadata, std::string_view name) {
    const auto pos = metadata.find(name);
    if (pos == std::string_view::npos) return {};
    const auto start = pos + name.size();
    if (start >= metadata.size()) return {};
    const auto end = metadata.find_first_of(" \t\r\n", start);
    return std::string(metadata.substr(start,
        end == std::string_view::npos ? metadata.size() - start : end - start));
}
std::uint64_t parse_count(std::string_view str) {
    require(!str.empty() && str.size() <= 18, "CHD track frame count missing");
    std::uint64_t result = 0;
    for (const auto c : str) {
        require(c >= '0' && c <= '9', "CHD track frame count is invalid");
        require(result <= (max_logical - std::uint64_t(c - '0')) / 10,
                "CHD track frame count overflows");
        result = result * 10 + std::uint64_t(c - '0');
    }
    require(result != 0, "CHD data track is empty");
    return result;
}
std::string metadata_string(chd_file* image, std::uint32_t tag,
                            std::uint32_t index) {
    std::array<char,2048> buffer{};
    std::uint32_t read = 0;
    const chd_error result = chd_get_metadata(image, tag, index, buffer.data(),
                                              static_cast<std::uint32_t>(buffer.size()-1),
                                              &read, nullptr, nullptr);
    if (result == CHDERR_METADATA_NOT_FOUND) return {};
    require(result == CHDERR_NONE && read > 0 && read < buffer.size(),
            "CHD optical track metadata is invalid");
    buffer[read] = '\0';
    require(std::strlen(buffer.data()) <= read,
            "CHD track metadata has invalid terminator");
    return std::string(buffer.data());
}
} // namespace

void ChdStream::FileCloser::operator()(std::FILE* file) const noexcept {
    if (file != nullptr) std::fclose(file);
}
void ChdStream::ChdCloser::operator()(chd_file* chd) const noexcept {
    if (chd != nullptr) chd_close(chd);
}

ChdStream::ChdStream(const std::filesystem::path& path) {
    require(std::filesystem::is_regular_file(path) &&
            !std::filesystem::is_symlink(path),
            "CHD input must be a regular non-symlink file");
#ifdef _WIN32
    std::FILE* raw = nullptr;
    require(_wfopen_s(&raw, path.c_str(), L"rb") == 0 && raw != nullptr,
            "cannot open CHD optical image");
    file_.reset(raw);
#else
    file_.reset(std::fopen(path.c_str(), "rb"));
    require(bool(file_), "cannot open CHD optical image");
#endif
    chd_file* opened = nullptr;
    const chd_error status = chd_open_file(file_.get(), CHD_OPEN_READ,
                                           nullptr, &opened);
    require(status == CHDERR_NONE && opened != nullptr,
            "CHD cannot be opened or its compressed map is invalid");
    chd_.reset(opened);
    const chd_header* header = chd_get_header(chd_.get());
    require(header != nullptr && header->version == 5,
            "only CHD version 5 is supported");
    require(std::all_of(std::begin(header->parentsha1),std::end(header->parentsha1),
                        [](std::uint8_t b){return b == 0;}),
            "CHD parent images are not supported");
    hunk_bytes_ = header->hunkbytes;
    unit_bytes_ = header->unitbytes;
    require(hunk_bytes_ >= iso_sector && hunk_bytes_ <= max_hunk &&
            (unit_bytes_ == iso_sector || unit_bytes_ == cd_frame) &&
            hunk_bytes_ % unit_bytes_ == 0 &&
            header->logicalbytes > 0 && header->logicalbytes <= max_logical &&
            header->logicalbytes % unit_bytes_ == 0 &&
            header->totalhunks > 0,
            "CHD logical hunk or optical sector geometry is unsupported");

    hunk_.resize(hunk_bytes_);
    const std::uint64_t frames = header->logicalbytes / unit_bytes_;
    if (unit_bytes_ == iso_sector) {
        // CHD image with a straight 2048-byte cooked optical stream.
        logical_size_ = header->logicalbytes;
        label_ = "CHD v5 / indexed compressed 2048-byte optical blocks";
    } else {
        // chdman-style CD images have 2448-byte frames: 2352 bytes raw CD
        // sector plus 96-byte subcode. Only expose Mode 1 / Mode 2 Form 1
        // logical user data, not the sync, ECC or subchannel bytes.
        auto meta = metadata_string(chd_.get(),CDROM_TRACK_METADATA2_TAG,0);
        std::uint32_t tag = CDROM_TRACK_METADATA2_TAG;
        if (meta.empty()) {
            tag = CDROM_TRACK_METADATA_TAG;
            meta = metadata_string(chd_.get(),tag,0);
        }
        require(!meta.empty(), "CHD CD track metadata is required");
        require(metadata_string(chd_.get(),tag,1).empty(),
                "CHD multi-track CD mapping is not supported yet");
        const auto type = get_field(meta,"TYPE:");
        const auto track = get_field(meta,"TRACK:");
        require(track == "1", "CHD CD must contain one data track");
        if (type == "MODE1" || type == "MODE1_RAW") {
            data_offset_ = 16;
        } else if (type == "MODE2_FORM1" || type == "MODE2_FORM1_RAW") {
            data_offset_ = 24;
        } else {
            throw std::runtime_error("CHD CD track is not Mode 1/Mode 2 Form 1 data");
        }
        require(get_field(meta,"PREGAP:").empty() ||
                get_field(meta,"PREGAP:") == "0",
                "CHD CD pregap mapping is not supported");
        require(get_field(meta,"POSTGAP:").empty() ||
                get_field(meta,"POSTGAP:") == "0",
                "CHD CD postgap mapping is not supported");
        const auto used_frames = parse_count(get_field(meta,"FRAMES:"));
        require(used_frames <= frames &&
                used_frames <= max_logical / iso_sector,
                "CHD track exceeds logical frame count");
        logical_size_ = used_frames * iso_sector;
        label_ = "CHD v5 / compressed single-track CD Mode 1/2 Form 1";

        // Some CHD CD containers represent a 150-sector CD lead-in and
        // cooked vs. raw track offsets differently. Trust a validated ISO
        // PVD or UDF anchor instead of guessing a fixed frame offset.
        auto probe = [&](std::uint64_t frame, std::uint32_t offset,
                         bool udf) -> bool {
            if (frame >= used_frames || offset > unit_bytes_ - 16)
                return false;
            const auto pos = frame * unit_bytes_ + offset;
            if (pos > header->logicalbytes ||
                16 > header->logicalbytes - pos) return false;
            const auto idx = pos / hunk_bytes_;
            if (cached_hunk_ != idx) load_hunk(idx);
            const auto within = static_cast<std::size_t>(pos % hunk_bytes_);
            if (within > hunk_.size() || 16 > hunk_.size() - within)
                return false;
            const auto* p = hunk_.data() + within;
            if (!udf)
                return p[0] == 1 && std::memcmp(p+1,"CD001",5) == 0 &&
                       p[6] == 1;
            if (p[0] != 2 || p[1] != 0) return false;
            unsigned sum = 0;
            for (int i=0; i<16; ++i) if (i!=4) sum += p[i];
            return (sum & 255u) == p[4];
        };
        bool found = false;
        const auto try_leading = [&](std::uint32_t lead, bool udf) -> bool {
            if (used_frames <= std::uint64_t(lead) + (udf ? 256u : 16u))
                return false;
            for (const auto offset : {data_offset_,0u,16u,24u}) {
                if (probe(std::uint64_t(lead) + (udf ? 256u : 16u),
                          offset,udf)) {
                    data_offset_ = offset;
                    leading_frames_ = lead;
                    logical_size_ = (used_frames - lead) * iso_sector;
                    return true;
                }
            }
            return false;
        };
        // Standard layouts are the fast path. Some chdman-produced and
        // vendor CD images place a data-track lead-in at other frame offsets.
        // Allow up to 512 frames, but only after observing a valid ISO PVD
        // or UDF anchor. Never guess and expose unknown audio as file bytes.
        for (const auto lead : {0u,150u}) {
            if (try_leading(lead,false) || try_leading(lead,true)) {
                found = true;
                break;
            }
        }
        if (!found) {
            const auto limit = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(512,used_frames > 16 ?
                    used_frames-17 : 0));
            for (std::uint32_t lead=1; lead<=limit; ++lead) {
                if (lead==150) continue;
                if (try_leading(lead,false) || try_leading(lead,true)) {
                    found = true;
                    break;
                }
            }
        }
        require(found, "CHD CD track has no validated ISO/UDF data anchor");

    }
}

ChdStream::~ChdStream() = default;

void ChdStream::load_hunk(std::uint64_t index) {
    require(index <= (std::numeric_limits<std::uint32_t>::max)(),
            "CHD hunk index exceeds supported range");
    const chd_error status = chd_read(chd_.get(),
        static_cast<std::uint32_t>(index), hunk_.data());
    require(status == CHDERR_NONE,
            "CHD hunk decompression or CRC verification failed");
    cached_hunk_ = index;
}

std::vector<std::uint8_t> ChdStream::read(
        std::uint64_t start, std::size_t length) {
    require(length <= 32u * 1024u * 1024u &&
            start <= logical_size_ && length <= logical_size_ - start,
            "CHD optical read outside logical data track");
    std::vector<std::uint8_t> result(length);
    std::size_t done = 0;
    while (done < length) {
        const std::uint64_t position = start + done;
        const std::uint64_t sector = position / iso_sector;
        const auto in_sector = static_cast<std::uint32_t>(position % iso_sector);
        const auto physical =
            (sector + leading_frames_) * unit_bytes_ + data_offset_ + in_sector;
        const std::uint64_t index = physical / hunk_bytes_;
        if (cached_hunk_ != index) load_hunk(index);
        const auto within_hunk = static_cast<std::size_t>(physical % hunk_bytes_);
        const auto capacity = std::min<std::size_t>(
            hunk_bytes_ - within_hunk, iso_sector - in_sector);
        const auto count = std::min<std::size_t>(capacity,length - done);
        require(count > 0 && count <= hunk_.size() - within_hunk,
                "invalid CHD hunk copy extent");
        std::copy_n(hunk_.data() + within_hunk, count, result.data() + done);
        done += count;
    }
    return result;
}

} // namespace iso_wcx
