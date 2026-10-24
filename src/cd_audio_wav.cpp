#include "iso_wcx/cd_audio_wav.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace iso_wcx {
namespace {
constexpr std::uint64_t pcm_bytes_per_frame = 2352;
constexpr std::uint32_t sample_rate = 44100;
constexpr std::uint32_t bytes_per_second = 176400;
constexpr std::uint16_t channel_count = 2;
constexpr std::uint16_t sample_bits = 16;

void require(bool condition, const char* what) {
    if (!condition) throw std::runtime_error(what);
}
void tag(std::vector<std::uint8_t>& dest, std::size_t at,
         std::string_view fourcc) {
    require(fourcc.size() == 4 && at <= dest.size() &&
            4 <= dest.size()-at, "invalid WAV header fourcc");
    for(std::size_t i=0;i<4;++i)
        dest[at+i]=static_cast<std::uint8_t>(fourcc[i]);
}
void le16(std::vector<std::uint8_t>& b, std::size_t at, std::uint16_t n) {
    b[at]=static_cast<std::uint8_t>(n);
    b[at+1]=static_cast<std::uint8_t>(n>>8);
}
void le32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t n) {
    for(unsigned i=0;i<4;++i)
        b[at+i]=static_cast<std::uint8_t>(n>>(8*i));
}
void le64(std::vector<std::uint8_t>& b, std::size_t at, std::uint64_t n) {
    for(unsigned i=0;i<8;++i)
        b[at+i]=static_cast<std::uint8_t>(n>>(8*i));
}
} // namespace

std::vector<std::uint8_t> cdda_wave_header(std::uint64_t pcm_bytes) {
    require(pcm_bytes>0 && pcm_bytes%pcm_bytes_per_frame==0,
            "CDDA data length must be whole 2352-byte CD sectors");
    // RIFF chunk size (file length - 8) cannot exceed UINT32_MAX.
    const bool rf64 =
        pcm_bytes > std::uint64_t(std::numeric_limits<std::uint32_t>::max())-36;
    const std::uint64_t extra = rf64 ? 80u : 44u;
    require(pcm_bytes<=std::numeric_limits<std::uint64_t>::max()-extra,
            "CDDA WAVE file length overflow");
    std::vector<std::uint8_t> header(static_cast<std::size_t>(extra),0);
    if(rf64) {
        tag(header,0,"RF64");
        le32(header,4,0xffffffffu);
        tag(header,8,"WAVE");
        tag(header,12,"ds64");
        le32(header,16,28);
        le64(header,20,pcm_bytes+extra-8);
        le64(header,28,pcm_bytes);
        le64(header,36,pcm_bytes/4); // Per-channel stereo PCM sample frames.
        le32(header,44,0);           // No extra ds64 table entries.
        tag(header,48,"fmt ");
        le32(header,52,16);
        le16(header,56,1); // WAVE_FORMAT_PCM.
        le16(header,58,channel_count);
        le32(header,60,sample_rate);
        le32(header,64,bytes_per_second);
        le16(header,68,channel_count*(sample_bits/8));
        le16(header,70,sample_bits);
        tag(header,72,"data");
        le32(header,76,0xffffffffu);
    } else {
        tag(header,0,"RIFF");
        le32(header,4,static_cast<std::uint32_t>(36u+pcm_bytes));
        tag(header,8,"WAVE");
        tag(header,12,"fmt ");
        le32(header,16,16);
        le16(header,20,1);
        le16(header,22,channel_count);
        le32(header,24,sample_rate);
        le32(header,28,bytes_per_second);
        le16(header,32,channel_count*(sample_bits/8));
        le16(header,34,sample_bits);
        tag(header,36,"data");
        le32(header,40,static_cast<std::uint32_t>(pcm_bytes));
    }
    return header;
}

std::uint64_t cdda_wave_file_length(std::uint64_t cd_frames) {
    require(cd_frames>0 &&
            cd_frames <=
                (std::numeric_limits<std::uint64_t>::max()-80u)/
                    pcm_bytes_per_frame,
            "CDDA stream has invalid or overflowing frame count");
    const std::uint64_t pcm=cd_frames*pcm_bytes_per_frame;
    return pcm+cdda_wave_header(pcm).size();
}
} // namespace iso_wcx
