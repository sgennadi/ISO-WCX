// Independent, read-only CloneCD CCD/IMG track layout parser.
// .SUB subchannels and scrambled sectors are intentionally not decoded.
#include "iso_wcx/image_tracks.hpp"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace iso_wcx {
namespace {
void check(bool condition, const char* reason) {
    if(!condition)throw std::runtime_error(reason);
}
std::string_view trim(std::string_view s) {
    while(!s.empty() && (s.front()==' '||s.front()=='\t'||s.front()=='\r'))s.remove_prefix(1);
    while(!s.empty() && (s.back()==' '||s.back()=='\t'||s.back()=='\r'))s.remove_suffix(1);
    return s;
}
std::string upper(std::string_view s) {
    std::string result(s);
    for(auto& c:result)c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return result;
}
unsigned number(std::string_view value) {
    unsigned v=0;
    value=trim(value);
    check(!value.empty(),"empty CloneCD numeric value");
    const auto* first=value.data();
    const auto* last=first+value.size();
    const auto [ptr,err]=std::from_chars(first,last,v,10);
    check(err==std::errc{} && ptr==last,"invalid CloneCD integer");
    return v;
}
struct Track {
    unsigned number{};
    std::optional<unsigned> mode, index0, index1;
};
std::filesystem::path sidecar(const std::filesystem::path& descriptor) {
    const auto ccd=std::filesystem::canonical(descriptor);
    const auto root=ccd.parent_path();
    auto img=ccd;img.replace_extension(".img");
    if(!std::filesystem::exists(img))img.replace_extension(".IMG");
    check(std::filesystem::exists(img)&&!std::filesystem::is_symlink(img),
          "CloneCD .img sidecar is missing or symbolic link");
    const auto target=std::filesystem::canonical(img);
    check(target.parent_path()==root && std::filesystem::is_regular_file(target),
          "CloneCD .img sidecar must be in the descriptor directory");
    return target;
}
} // namespace

bool is_ccd_path(const std::filesystem::path& descriptor) {
    return upper(descriptor.extension().string())==".CCD";
}

std::vector<ImageTrack> ccd_data_tracks(const std::filesystem::path& descriptor) {
    std::ifstream input(descriptor,std::ios::binary|std::ios::ate);
    check(bool(input),"cannot open CloneCD descriptor");
    const auto n=input.tellg();
    check(n>0 && n<=1024*1024,"CloneCD descriptor exceeds size limit");
    std::string text(static_cast<std::size_t>(n),'\0');
    input.seekg(0);
    check(bool(input.read(text.data(),static_cast<std::streamsize>(text.size()))),
          "cannot read CloneCD descriptor");
    if(text.starts_with("\xef\xbb\xbf"))text.erase(0,3);
    check(text.find('\0')==std::string::npos,"binary CloneCD descriptor");
    std::string section;
    bool has_header=false,has_disc=false;
    std::optional<unsigned> version,sessions,scrambled;
    std::vector<Track> parsed;
    std::size_t p=0;
    while(p<text.size()) {
        const auto end=text.find('\n',p);
        auto line=trim(std::string_view(text).substr(p,end==std::string::npos?text.size()-p:end-p));
        p=end==std::string::npos?text.size():end+1;
        check(line.size()<=4096,"CloneCD line too long");
        if(line.empty()||line.front()==';'||line.front()=='#')continue;
        if(line.front()=='[') {
            check(line.back()==']'&&line.size()>=3,"bad CloneCD section");
            section=upper(trim(line.substr(1,line.size()-2)));
            if(section=="CLONECD")has_header=true;
            else if(section=="DISC")has_disc=true;
            else if(section.starts_with("TRACK ")) {
                const auto id=number(std::string_view(section).substr(6));
                check(id>=1&&id<=99&&parsed.size()<99,"invalid CloneCD track number");
                if(!parsed.empty())check(id>parsed.back().number,"unordered/duplicate CloneCD track");
                parsed.push_back({id,{}, {}, {}});
            }
            continue;
        }
        const auto eq=line.find('=');
        check(eq!=std::string_view::npos,"invalid CloneCD field");
        const auto key=upper(trim(line.substr(0,eq)));
        const auto val=trim(line.substr(eq+1));
        if(section=="CLONECD"&&key=="VERSION") {
            check(!version,"duplicate CloneCD version");
            version=number(val);
        } else if(section=="DISC"&&key=="SESSIONS") {
            check(!sessions,"duplicate CloneCD session count");
            sessions=number(val);
        } else if(section=="DISC"&&key=="DATATRACKSSCRAMBLED") {
            check(!scrambled,"duplicate CloneCD scrambled flag");
            scrambled=number(val);
        } else if(section.starts_with("TRACK ")&&!parsed.empty()) {
            auto& t=parsed.back();
            if(key=="MODE") {
                check(!t.mode,"duplicate CloneCD track mode");
                t.mode=number(val);
            } else if(key=="INDEX 0") {
                check(!t.index0,"duplicate CloneCD track INDEX 0");
                t.index0=number(val);
            } else if(key=="INDEX 1") {
                check(!t.index1,"duplicate CloneCD track INDEX 1");
                t.index1=number(val);
            }
        }
    }
    check(has_header&&has_disc&&version&&*version==3,
          "unsupported CloneCD format version");
    check(sessions&&*sessions==1,"multisession CloneCD not yet supported");
    check(scrambled&&*scrambled<=1,"invalid CloneCD scrambled flag");
    check(!parsed.empty(),"CloneCD descriptor has no tracks");
    const auto img=sidecar(descriptor);
    const auto bytes=std::filesystem::file_size(img);
    constexpr std::uint64_t physical=2352;
    check(bytes%physical==0,"CloneCD .img size is not sector-aligned");
    const auto total=bytes/physical;
    std::vector<ImageTrack> tracks;
    for(std::size_t i=0;i<parsed.size();++i) {
        const auto& t=parsed[i];
        check(t.mode && t.index1, "incomplete CloneCD TRACK section");
        check(*t.mode<=2, "unsupported CloneCD track mode");
        const auto start=std::uint64_t(*t.index1);
        check(start<total,"CloneCD track outside .img");
        std::uint64_t end=total;
        if(i+1<parsed.size()) {
            const auto& following=parsed[i+1];
            check(following.index1.has_value(),"missing next CloneCD INDEX 1");
            if(following.index0)check(*following.index0<=*following.index1,
                                      "invalid CloneCD INDEX 0");
            end=following.index0?*following.index0:*following.index1;
        }
        check(end>start&&end<=total,"CloneCD track boundaries overlap");
        if(t.index0)check(*t.index0<=*t.index1,"invalid CloneCD INDEX 0");
        if(*t.mode==0)continue; // CD-DA is handled separately from filesystem tracks.
        tracks.push_back({img,start*physical,end*physical,2352,
                          *t.mode==1?16u:24u,static_cast<std::uint8_t>(*t.mode),
                          0,t.number,"CloneCD track "+std::to_string(t.number),*scrambled!=0});
    }
    check(!tracks.empty(),"CloneCD has no supported data tracks");
    return tracks;
}
std::optional<CloneCdSub> ccd_subchannels(const std::filesystem::path& descriptor) {
    const auto img=sidecar(descriptor);
    const auto img_size=std::filesystem::file_size(img);
    check(img_size%2352==0,"CloneCD IMG must contain whole sectors");
    const auto sectors=img_size/2352;
    check(sectors<=std::numeric_limits<std::uint64_t>::max()/96,
          "CloneCD SUB size overflow");
    auto sub=std::filesystem::canonical(descriptor);
    sub.replace_extension(".sub");
    if(!std::filesystem::exists(sub))sub.replace_extension(".SUB");
    // Reject symlinks even if they are broken; an attacker must not point a
    // supposed sidecar to data outside the image directory.
    check(!std::filesystem::is_symlink(sub),"CloneCD SUB symlink is forbidden");
    if(!std::filesystem::exists(sub))return std::nullopt;
    const auto full=std::filesystem::canonical(sub);
    check(full.parent_path()==std::filesystem::canonical(descriptor).parent_path() &&
          std::filesystem::is_regular_file(full),
          "CloneCD SUB must be a regular sibling sidecar");
    const auto size=std::filesystem::file_size(full);
    check(size==sectors*96,"CloneCD SUB size does not match IMG sector count");
    return CloneCdSub{full,size};
}

} // namespace iso_wcx
