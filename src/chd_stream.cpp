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
#include <vector>

namespace iso_wcx {
namespace {
void require(bool valid, const char* message) {
    if (!valid) throw std::runtime_error(message);
}
constexpr std::uint64_t max_logical = std::uint64_t(1) << 44;
constexpr std::uint32_t max_hunk = 1024u * 1024u;
constexpr std::uint32_t iso_sector = 2048;
constexpr std::uint32_t cd_frame = 2448;
constexpr std::uint64_t max_hunks = 4'000'000;

std::uint32_t header_be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | p[3];
}
std::uint64_t header_be64(const std::uint8_t* p) {
    return (std::uint64_t(header_be32(p)) << 32) | header_be32(p+4);
}

// Interpret native CHD CD track geometry, including MAME's four-frame padding.
// Audio and unsupported sector modes consume storage but are never exposed as
// ISO/UDF user data. No external chdman utility is needed at runtime.
std::string get_field(std::string_view metadata, std::string_view name) {
    // Match a complete metadata token, not a substring of another key.
    std::size_t at = 0;
    while (at < metadata.size()) {
        const auto start = metadata.find_first_not_of(" \t\r\n", at);
        if (start == std::string_view::npos) break;
        const auto end = metadata.find_first_of(" \t\r\n", start);
        const auto token = metadata.substr(start,
            end == std::string_view::npos ? metadata.size() - start : end - start);
        if (token.starts_with(name))
            return std::string(token.substr(name.size()));
        if (end == std::string_view::npos) break;
        at = end;
    }
    return {};
}
std::uint64_t parse_count(std::string_view str, bool allow_zero = false) {
    require(!str.empty() && str.size() <= 18, "CHD track frame count missing");
    std::uint64_t result = 0;
    for (const auto c : str) {
        require(c >= '0' && c <= '9', "CHD track frame count is invalid");
        require(result <= (max_logical - std::uint64_t(c - '0')) / 10,
                "CHD track frame count overflows");
        result = result * 10 + std::uint64_t(c - '0');
    }
    require(allow_zero || result != 0, "CHD data track is empty");
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
// cdrom_file in MAME and libchdr align each track independently to four
// 2448-byte frames. FRAMES excludes those artificial padding frames.
struct OpticalTrack {
    std::uint64_t first_frame = 0;
    std::uint64_t frames = 0;
    std::uint32_t number = 0;
    std::uint32_t preferred_offset = 0;
    bool readable_data = false;
    bool is_audio = false;
};

std::vector<OpticalTrack> optical_track_map(chd_file* image,
                                           std::uint64_t total_frames) {
    std::vector<OpticalTrack> tracks;
    std::uint64_t cursor = 0;
    constexpr std::uint32_t max_tracks = 99;
    for (std::uint32_t i = 0; i < max_tracks; ++i) {
        // New CHT2 metadata is preferred over the original CHTR tag.
        auto meta = metadata_string(image, CDROM_TRACK_METADATA2_TAG, i);
        if (meta.empty())
            meta = metadata_string(image, CDROM_TRACK_METADATA_TAG, i);
        if (meta.empty()) break;

        const auto number = parse_count(get_field(meta, "TRACK:"));
        require(number == std::uint64_t(i) + 1,
                "CHD track numbers are missing or not sequential");
        const auto count = parse_count(get_field(meta, "FRAMES:"));
        require(count <= total_frames - cursor,
                "CHD track frame count exceeds compressed image");
        const auto pad = (4u - (count % 4u)) % 4u;
        require(pad <= total_frames - cursor - count,
                "CHD CD four-frame alignment exceeds compressed image");

        const auto pregap = get_field(meta, "PREGAP:");
        const auto postgap = get_field(meta, "POSTGAP:");
        if (!pregap.empty())
            require(parse_count(pregap, true) <= total_frames,
                    "CHD CD pregap is invalid");
        if (!postgap.empty())
            require(parse_count(postgap, true) <= total_frames,
                    "CHD CD postgap is invalid");

        OpticalTrack track{};
        track.first_frame = cursor;
        track.frames = count;
        track.number = static_cast<std::uint32_t>(number);
        const auto type = get_field(meta, "TYPE:");
        require(!type.empty(), "CHD CD track type is missing");
        if (type == "AUDIO") {
            track.is_audio = true;
        } else if (type == "MODE1" || type == "MODE1_RAW") {
            track.preferred_offset = 16;
            track.readable_data = true;
        } else if (type == "MODE2_FORM1" || type == "MODE2_FORM1_RAW" ||
                   type == "MODE2_RAW") {
            // MODE2_RAW is included only when the ISO/UDF anchor validates
            // the actual sector payload. Form 2 files are not exposed.
            track.preferred_offset = 24;
            track.readable_data = true;
        }
        tracks.push_back(track);
        cursor += count + pad;
    }
    require(!tracks.empty(), "CHD CD track metadata is required");
    require(metadata_string(image, CDROM_TRACK_METADATA2_TAG, max_tracks).empty() &&
            metadata_string(image, CDROM_TRACK_METADATA_TAG, max_tracks).empty(),
            "CHD CD has more than 99 tracks");
    return tracks;
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
    // Fail before libchdr allocates/expands an untrusted compressed hunk map.
    // A crafted header can otherwise request millions of 12-byte entries or
    // huge buffers before the caller has a chance to inspect the CHD header.
    const auto on_disk_bytes = std::filesystem::file_size(path);
    require(on_disk_bytes >= 124, "CHD v5 header is truncated");
    std::array<std::uint8_t,124> header_bytes{};
    require(std::fread(header_bytes.data(),1,header_bytes.size(),file_.get())
                == header_bytes.size(),
            "CHD v5 header could not be read");
    const auto* raw_header = header_bytes.data();
    require(std::memcmp(raw_header,"MComprHD",8) == 0 &&
            header_be32(raw_header+8) == 124 &&
            header_be32(raw_header+12) == 5,
            "CHD v5 header signature, version or length is invalid");
    const auto bytes = header_be64(raw_header+32);
    const auto map_at = header_be64(raw_header+40);
    const auto metadata_at = header_be64(raw_header+48);
    const auto hunk_bytes = header_be32(raw_header+56);
    const auto unit_bytes = header_be32(raw_header+60);
    require(bytes > 0 && bytes <= max_logical &&
            (unit_bytes == iso_sector || unit_bytes == cd_frame) &&
            hunk_bytes >= iso_sector && hunk_bytes <= max_hunk &&
            hunk_bytes % unit_bytes == 0 && bytes % unit_bytes == 0 &&
            (bytes-1)/hunk_bytes + 1 <= max_hunks &&
            map_at >= 124 && map_at < on_disk_bytes &&
            (metadata_at == 0 || metadata_at < on_disk_bytes),
            "CHD v5 image geometry, map or metadata offsets exceed safety limits");
    require(std::all_of(header_bytes.begin()+104,header_bytes.end(),
            [](std::uint8_t c){return c==0;}),
            "CHD v5 has an unsupported parent reference");
    require(std::fseek(file_.get(),0,SEEK_SET) == 0,
            "CHD input seek failed");
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
        const auto tracks = optical_track_map(chd_.get(), frames);
        for (const auto& track : tracks) {
            if (track.is_audio) {
                require(track.frames <= max_logical / 2352u,
                        "CHD audio track length exceeds safety limit");
                audio_tracks_.push_back({
                    track.number, track.first_frame, track.frames
                });
            }
        }


        // A valid ISO PVD or UDF anchor is required before a physical track
        // is exposed to the filesystem parsers. Try the last data track first,
        // matching CUE/NRG handling, while allowing audio tracks on either side.
        auto probe = [&](std::uint64_t frame, std::uint32_t offset,
                         bool udf) -> bool {
            if (frame >= frames || offset > unit_bytes_ - 16)
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
        for (auto it = tracks.rbegin(); it != tracks.rend() && !found; ++it) {
            const auto& track = *it;
            if (!track.readable_data) continue;
            const auto try_leading = [&](std::uint32_t lead, bool udf) -> bool {
                const std::uint64_t descriptor_sector = udf ? 256u : 16u;
                if (track.frames <= std::uint64_t(lead) + descriptor_sector)
                    return false;
                const auto anchor = track.first_frame + lead + descriptor_sector;
                for (const auto offset : {track.preferred_offset, 0u, 16u, 24u}) {
                    if (!probe(anchor, offset, udf)) continue;
                    data_offset_ = offset;
                    leading_frames_ = track.first_frame + lead;
                    logical_size_ = (track.frames - lead) * iso_sector;
                    label_ = "CHD v5 / compressed CD track " +
                             std::to_string(track.number) + " of " +
                             std::to_string(tracks.size()) +
                             " / validated Mode 1 or Mode 2 Form 1";
                    return true;
                }
                return false;
            };

            // The track start is derived from CHD metadata, never from
            // concatenated unpadded CUE lengths. A 150-sector lead-in or
            // captured INDEX 00 can shift the filesystem anchor.
            for (const auto lead : {0u, 150u}) {
                if (try_leading(lead, false) || try_leading(lead, true)) {
                    found = true;
                    break;
                }
            }
            if (found) break;
            const auto limit = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(512, track.frames > 16 ?
                    track.frames - 17 : 0));
            for (std::uint32_t lead = 1; lead <= limit; ++lead) {
                if (lead == 150) continue;
                if (try_leading(lead, false) || try_leading(lead, true)) {
                    found = true;
                    break;
                }
            }
        }
        // Audio-only CHDs and mixed discs without a supported filesystem are
        // valid audio archives. Other unrecognized optical layouts fail closed.
        require(found || !audio_tracks_.empty(),
                "CHD CD has neither validated ISO/UDF nor playable CDDA tracks");
        if (!found)
            label_ = "CHD v5 / native Red Book CDDA audio (no ISO/UDF track)";

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

std::vector<std::uint8_t> ChdStream::read_audio(
        std::uint32_t track_number, std::uint64_t start, std::size_t length) {
    require(unit_bytes_ == cd_frame,
            "only CD CHD frames contain Red Book PCM audio");
    const auto selected = std::find_if(audio_tracks_.begin(),audio_tracks_.end(),
        [track_number](const ChdAudioTrack& t){return t.number==track_number;});
    require(selected!=audio_tracks_.end(),"requested CHD CDDA track does not exist");
    require(selected->frames <= max_logical / 2352u,
            "CHD audio geometry exceeds size limit");
    const auto pcm_size=selected->frames*2352u;
    require(start <= pcm_size && length <= pcm_size-start &&
            length <= 32u*1024u*1024u && (start%2u)==0 && (length%2u)==0,
            "CHD audio PCM request exceeds a sample-aligned track");
    std::vector<std::uint8_t> result(length);
    std::size_t done=0;
    while(done<length) {
        const std::uint64_t position=start+done;
        const std::uint64_t local_frame=position/2352u;
        const std::uint32_t in_frame=static_cast<std::uint32_t>(position%2352u);
        require(local_frame<selected->frames,
                "CHD audio PCM frame exceeds track bounds");
        const std::uint64_t absolute_frame=selected->first_frame+local_frame;
        const std::uint64_t physical=absolute_frame*cd_frame+in_frame;
        const std::uint64_t index=physical/hunk_bytes_;
        if(cached_hunk_!=index)load_hunk(index);
        const auto hunk_pos=static_cast<std::size_t>(physical%hunk_bytes_);
        const auto count=std::min<std::size_t>(
            {length-done,2352u-in_frame,hunk_bytes_-hunk_pos});
        require(count>0 && count%2u==0 && hunk_pos<=hunk_.size() &&
                count<=hunk_.size()-hunk_pos,
                "CHD audio PCM hunk data is incomplete");
        std::copy_n(hunk_.data()+hunk_pos,count,result.data()+done);
        done+=count;
    }
    // MAME CD CHD stores standard CD audio as big-endian 16-bit sample
    // words; WAVE PCM16 is little-endian. Swapping after fully assembling
    // the bounded buffer also handles requests spanning hunk boundaries.
    for(std::size_t i=0;i<result.size();i+=2)
        std::swap(result[i],result[i+1]);
    return result;
}

} // namespace iso_wcx
