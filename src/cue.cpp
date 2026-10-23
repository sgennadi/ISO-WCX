#include "iso_wcx/cue.hpp"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <limits>

namespace iso_wcx {
namespace {
void check(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}
std::string upper(std::string value) {
    for (char& c : value)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return value;
}
std::string_view trim(std::string_view v) {
    while (!v.empty() && (v.front()==' ' || v.front()=='\t' || v.front()=='\r')) v.remove_prefix(1);
    while (!v.empty() && (v.back()==' ' || v.back()=='\t' || v.back()=='\r')) v.remove_suffix(1);
    return v;
}
std::pair<std::string,std::string_view> token(std::string_view s) {
    s=trim(s);
    check(!s.empty(), "missing CUE token");
    if(s.front()=='"') {
        s.remove_prefix(1);
        auto pos=s.find('"');
        check(pos!=std::string_view::npos && pos!=0, "unterminated CUE quoted string");
        auto result=std::string(s.substr(0,pos));
        return {result,trim(s.substr(pos+1))};
    }
    auto end=s.find_first_of(" \t\r");
    if(end==std::string_view::npos)return {std::string(s),{}};
    return {std::string(s.substr(0,end)),trim(s.substr(end))};
}
unsigned integer(std::string_view s) {
    unsigned n{};
    auto [ptr,err]=std::from_chars(s.data(),s.data()+s.size(),n);
    check(err==std::errc{} && ptr==s.data()+s.size(),"invalid CUE integer");
    return n;
}
std::uint32_t frames(std::string_view s) {
    auto first=s.find(':');
    auto last=s.rfind(':');
    check(first!=std::string_view::npos && last>first && last!=s.size()-1,
          "invalid CUE MM:SS:FF timecode");
    auto minutes=integer(s.substr(0,first));
    auto seconds=integer(s.substr(first+1,last-first-1));
    auto f=integer(s.substr(last+1));
    check(minutes<=999 && seconds<60 && f<75,"CUE timecode out of range");
    return static_cast<std::uint32_t>((minutes*60+seconds)*75+f);
}
std::filesystem::path safe_image(const std::filesystem::path& root,std::string ref) {
    check(!ref.empty() && ref.size()<=1024,"invalid CUE image path");
    // CUE paths commonly use DOS backslashes even on non-Windows hosts.
    std::replace(ref.begin(),ref.end(),'\\','/');
    for(unsigned char c:ref)check(c>=32 && c!=':',"unsupported CUE image path");
    auto candidate=std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(ref.data()),ref.size()));
    check(!candidate.empty() && !candidate.is_absolute() && !candidate.has_root_name(),
          "absolute CUE FILE reference is not allowed");
    for(const auto& part:candidate)
        check(part!="." && part!=".." && !part.empty(),"unsafe CUE FILE reference");
    // canonical() resolves symlinks. Reject a file link that escapes the CUE
    // directory even if the original spelling looks harmless.
    const auto full=std::filesystem::canonical(root/candidate);
    const auto relative=full.lexically_relative(root);
    check(!relative.empty() && !relative.is_absolute(),"CUE FILE escapes its directory");
    for(const auto& part:relative)
        check(part!="..", "CUE FILE escapes its directory");
    check(std::filesystem::is_regular_file(full),"CUE FILE must reference a regular file");
    return full;
}
struct ParsedTrack {
    std::string file_name;
    bool binary{};
    unsigned number{};
    std::string type;
    std::optional<std::uint32_t> index0,index1;
};
}

bool is_cue_path(const std::filesystem::path& path) {
    return upper(path.extension().string())==".CUE";
}

struct AllCueTracks {
    std::vector<CueTrack> data;
    std::vector<CueAudioTrack> audio;
};

// Both APIs use the same strict CUE syntax parser. This prevents audio and
// data readers from disagreeing about INDEX/pregap track boundaries.
static AllCueTracks parse_cue_tracks(const std::filesystem::path& cue) {
    std::ifstream in(cue,std::ios::binary|std::ios::ate);
    check(bool(in),"cannot open CUE sheet");
    const auto len=in.tellg();
    check(len>0 && len<=1024*1024,"CUE sheet exceeds 1 MiB size limit");
    std::string contents(static_cast<std::size_t>(len),'\0');
    in.seekg(0);
    check(bool(in.read(contents.data(),len)),"cannot read CUE sheet");
    if(contents.starts_with("\xef\xbb\xbf"))contents.erase(0,3);
    check(contents.find('\0')==std::string::npos,"binary CUE sheet is not supported");
    const auto root=std::filesystem::canonical(cue).parent_path();
    std::vector<ParsedTrack> tracks;
    std::optional<std::string> current_file;
    bool current_binary=false;
    unsigned previous_number=0;
    std::size_t offset=0;
    while(offset<contents.size()) {
        const auto end=contents.find('\n',offset);
        auto line=trim(std::string_view(contents).substr(offset,
                        end==std::string::npos?contents.size()-offset:end-offset));
        offset=end==std::string::npos?contents.size():end+1;
        check(line.size()<=4096,"CUE line too long");
        if(line.empty())continue;
        auto [command,args]=token(line);
        command=upper(command);
        if(command=="REM" || command=="TITLE" || command=="PERFORMER" ||
           command=="SONGWRITER" || command=="ISRC" || command=="CATALOG" ||
           command=="FLAGS" || command=="PREGAP" || command=="POSTGAP" ||
           command=="CDTEXTFILE")continue;
        if(command=="FILE") {
            auto [filename,tail]=token(args);
            auto [kind,leftover]=token(tail);
            check(leftover.empty(),"malformed CUE FILE declaration");
            current_binary=upper(kind)=="BINARY";
            // A non-binary FILE (e.g. WAVE) may describe an audio track. It
            // is never interpreted as sector-addressable disc data.
            // Resolve only selected data tracks: a missing audio track file
            // must not prevent reading a later valid data track.
            current_file=std::move(filename);
        } else if(command=="TRACK") {
            auto [number,tail]=token(args);
            auto [format,leftover]=token(tail);
            check(leftover.empty(),"malformed CUE TRACK declaration");
            auto n=integer(number);
            check(n>=1 && n<=99 && n>previous_number,"CUE tracks must increase from 01 to 99");
            previous_number=n;
            check(tracks.size()<99,"too many CUE tracks");
            tracks.push_back({current_file.value_or(std::string{}),current_binary,
                              n,upper(format),{}, {}});
        } else if(command=="INDEX") {
            check(!tracks.empty(),"CUE INDEX without TRACK");
            auto [number,tail]=token(args);
            auto [clock,leftover]=token(tail);
            check(leftover.empty(),"malformed CUE INDEX declaration");
            const auto n=integer(number);
            auto f=frames(clock);
            if(n==0) {check(!tracks.back().index0,"duplicate CUE INDEX 00");tracks.back().index0=f;}
            if(n==1) {check(!tracks.back().index1,"duplicate CUE INDEX 01");tracks.back().index1=f;}
        }
        // Other metadata directives do not influence the data track mapping.
    }
    AllCueTracks result;
    for(std::size_t i=0;i<tracks.size();++i) {
        const auto& t=tracks[i];
        if(!t.binary || !t.index1)continue;
        // An audio CD frame contains 588 stereo PCM samples (2*16-bit), or
        // 2352 bytes. A single FILE may hold many tracks; an INDEX 00 of the
        // next track marks the end of the preceding track, excluding pregap.
        if(t.type=="AUDIO") {
            // A CUE's absent, optional audio file must not prevent access to
            // a separate supported data FILE. Reject any unsafe existing
            // reference via the same canonicalized resolver as data tracks.
            std::string normalized=t.file_name;
            std::replace(normalized.begin(),normalized.end(),'\\','/');
            auto relative=std::filesystem::path(std::u8string(
                reinterpret_cast<const char8_t*>(normalized.data()),normalized.size()));
            check(!normalized.empty() && normalized.size()<=1024 &&
                  !relative.is_absolute() && !relative.has_root_name(),
                  "invalid CUE audio filename");
            for(const auto& part:relative)check(part!=".." && part!="." && !part.empty(),
                                                  "unsafe CUE audio filename");
            if(!std::filesystem::exists(root/relative))continue;
            auto source=safe_image(root,t.file_name);
            const auto bytes=std::filesystem::file_size(source);
            check(bytes%2352==0,"CUE BINARY AUDIO has an incomplete 2352-byte frame");
            const auto total_frames=bytes/2352;
            std::uint64_t first=*t.index1;
            std::uint64_t last=total_frames;
            for(std::size_t j=i+1;j<tracks.size();++j) {
                const auto& next=tracks[j];
                if(next.file_name!=t.file_name)break;
                // Mixed-sector-sized tracks in one file cannot safely be
                // addressed by a common MM:SS:FF index without explicit
                // sector boundary metadata.
                check(next.type!="MODE1/2048" && next.type!="MODE2/2336",
                      "CUE mixed physical frame sizes in one BINARY FILE");
                if(next.index0 || next.index1) {
                    last=next.index0 ? *next.index0 : *next.index1;
                    break;
                }
            }
            check(first<last && last<=total_frames,"CUE AUDIO track outside BINARY source");
            const std::uint64_t length=(last-first)*2352;
            // Standard PCM RIFF WAV uses 32-bit chunk lengths. Reject tracks
            // larger than 4 GiB rather than producing corrupt WAV headers.
            check(length<=std::numeric_limits<std::uint32_t>::max()-36,
                  "CUE AUDIO track too large for RIFF WAVE");
            result.audio.push_back({std::move(source),first*2352,length,t.number});
            continue;
        }
        std::uint32_t size=0,offset_in_sector=0;
        std::uint8_t mode=0;
        if(t.type=="MODE1/2048") {size=2048;offset_in_sector=0;mode=0;}
        else if(t.type=="MODE1/2352") {size=2352;offset_in_sector=16;mode=1;}
        else if(t.type=="MODE2/2352") {size=2352;offset_in_sector=24;mode=2;}
        else if(t.type=="MODE2/2336") {size=2336;offset_in_sector=8;mode=2;}
        else continue;
        std::optional<std::uint32_t> end;
        // A subsequent track in the SAME FILE ends this track. INDEX 00
        // starts that next track's pregap and is the safest exclusive bound.
        for(std::size_t j=i+1;j<tracks.size();++j) {
            const auto& next=tracks[j];
            if(next.file_name!=t.file_name)break;
            if(next.index0 || next.index1) {
                end=next.index0 ? next.index0 : next.index1;
                break;
            }
        }
        if(end)check(*end>*t.index1,"CUE track boundaries overlap");
        result.data.push_back({safe_image(root,t.file_name),*t.index1,end,size,offset_in_sector,mode,t.number});
    }
    check(!result.data.empty() || !result.audio.empty(),
          "CUE has no supported BINARY data or PCM audio tracks");
    return result;
}

std::vector<CueTrack> cue_data_tracks(const std::filesystem::path& cue) {
    return parse_cue_tracks(cue).data;
}
std::vector<CueAudioTrack> cue_audio_tracks(const std::filesystem::path& cue) {
    return parse_cue_tracks(cue).audio;
}
}
