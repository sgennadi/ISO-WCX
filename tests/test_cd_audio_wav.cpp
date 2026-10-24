#include "iso_wcx/cd_audio_wav.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
std::uint32_t le32(const std::vector<std::uint8_t>& b,std::size_t p) {
    return std::uint32_t(b[p]) | (std::uint32_t(b[p+1])<<8) |
           (std::uint32_t(b[p+2])<<16) | (std::uint32_t(b[p+3])<<24);
}
std::uint64_t le64(const std::vector<std::uint8_t>& b,std::size_t p) {
    return std::uint64_t(le32(b,p))|(std::uint64_t(le32(b,p+4))<<32);
}
bool tag(const std::vector<std::uint8_t>& b,std::size_t p,std::string_view expected) {
    return expected.size()==4 && p+4<=b.size() &&
        std::equal(expected.begin(),expected.end(),b.begin()+static_cast<std::ptrdiff_t>(p));
}
void check(bool ok,const char* explanation) {
    if(!ok)throw std::runtime_error(explanation);
}
}
int main() {
    constexpr std::uint64_t frames=37;
    constexpr std::uint64_t bytes=frames*2352u;
    const auto wav=iso_wcx::cdda_wave_header(bytes);
    check(wav.size()==44 && iso_wcx::cdda_wave_file_length(frames)==bytes+44,
          "ordinary RIFF/WAV header size mismatch");
    check(tag(wav,0,"RIFF") && tag(wav,8,"WAVE") &&
          tag(wav,12,"fmt ") && tag(wav,36,"data"),
          "ordinary RIFF chunk ordering invalid");
    check(le32(wav,4)==bytes+36 && le32(wav,16)==16 &&
          le32(wav,24)==44100 && le32(wav,28)==176400 &&
          le32(wav,40)==bytes && wav[20]==1 && wav[22]==2 &&
          wav[32]==4 && wav[34]==16,
          "Red Book CDDA WAVE PCM parameters invalid");
    constexpr std::uint64_t big_frames=2'000'000;
    constexpr std::uint64_t big_pcm=big_frames*2352u;
    const auto rf64=iso_wcx::cdda_wave_header(big_pcm);
    check(rf64.size()==80 && iso_wcx::cdda_wave_file_length(big_frames)==big_pcm+80,
          "RF64 virtual file size or header length invalid");
    check(tag(rf64,0,"RF64") && tag(rf64,8,"WAVE") &&
          tag(rf64,12,"ds64") && tag(rf64,48,"fmt ") &&
          tag(rf64,72,"data"),
          "RF64 chunk ordering invalid");
    check(le32(rf64,4)==0xffffffffu && le32(rf64,16)==28 &&
          le64(rf64,20)==big_pcm+72 &&
          le64(rf64,28)==big_pcm && le64(rf64,36)==big_pcm/4 &&
          le32(rf64,44)==0 && le32(rf64,60)==44100 &&
          le32(rf64,76)==0xffffffffu,
          "RF64 ds64 data size or PCM header invalid");
    bool rejected=false;
    try {(void)iso_wcx::cdda_wave_header(0);}catch(const std::runtime_error&){rejected=true;}
    check(rejected,"zero-sized CDDA track must not create a WAVE file");
    rejected=false;
    try {(void)iso_wcx::cdda_wave_header(2353);}catch(const std::runtime_error&){rejected=true;}
    check(rejected,"partial sector must not create a WAVE file");
    rejected=false;
    try {(void)iso_wcx::cdda_wave_file_length(
        std::numeric_limits<std::uint64_t>::max());}
    catch(const std::runtime_error&){rejected=true;}
    check(rejected,"oversized CDDA stream must be rejected without allocation");
    std::cout<<"PASS CDDA RIFF WAVE and RF64 >4GiB headers, dimensions and rejections\n";
}
