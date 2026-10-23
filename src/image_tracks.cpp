#include "iso_wcx/image_tracks.hpp"
#include "iso_wcx/mds_probe.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace iso_wcx {
namespace {
using Bytes=std::vector<std::uint8_t>;
void check(bool ok, const char* why) {
    if(!ok) throw std::runtime_error(why);
}
std::string uppercase(std::string text) {
    for(auto& c:text)c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}
void region(std::span<const std::uint8_t> b,std::size_t at,std::size_t length) {
    check(at<=b.size() && length<=b.size()-at,"image descriptor truncated");
}
std::uint16_t be16(std::span<const std::uint8_t> b,std::size_t at) {
    region(b,at,2);
    return (std::uint16_t(b[at])<<8)|b[at+1];
}
std::uint32_t be32(std::span<const std::uint8_t> b,std::size_t at) {
    region(b,at,4);
    return (std::uint32_t(b[at])<<24)|(std::uint32_t(b[at+1])<<16)|
           (std::uint32_t(b[at+2])<<8)|b[at+3];
}
std::uint64_t be64(std::span<const std::uint8_t> b,std::size_t at) {
    return (std::uint64_t(be32(b,at))<<32)|be32(b,at+4);
}
std::uint16_t le16(std::span<const std::uint8_t> b,std::size_t at) {
    region(b,at,2);
    return b[at]|(std::uint16_t(b[at+1])<<8);
}
std::uint32_t le32(std::span<const std::uint8_t> b,std::size_t at) {
    region(b,at,4);
    return std::uint32_t(b[at])|(std::uint32_t(b[at+1])<<8)|
           (std::uint32_t(b[at+2])<<16)|(std::uint32_t(b[at+3])<<24);
}
std::uint64_t le64(std::span<const std::uint8_t> b,std::size_t at) {
    return (std::uint64_t(le32(b,at+4))<<32)|le32(b,at);
}
class CheckedFile {
    std::ifstream stream_;
    std::uint64_t length_{};
public:
    explicit CheckedFile(const std::filesystem::path& path):stream_(path,std::ios::binary|std::ios::ate) {
        check(bool(stream_),"cannot open disc container");
        auto n=stream_.tellg();check(n>=0,"cannot size disc container");
        length_=static_cast<std::uint64_t>(n);
    }
    [[nodiscard]] std::uint64_t size() const {return length_;}
    Bytes read(std::uint64_t at,std::size_t length) {
        check(at<=length_ && length<=length_-at &&
              at<=static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()),
              "container offset outside file");
        Bytes b(length);
        stream_.clear();stream_.seekg(static_cast<std::streamoff>(at));
        check(bool(stream_) && (length==0 || bool(stream_.read(reinterpret_cast<char*>(b.data()),
                                           static_cast<std::streamsize>(length)))),"container read failure");
        return b;
    }
};
std::string fourcc(std::span<const std::uint8_t> b,std::size_t p) {
    region(b,p,4);
    return {reinterpret_cast<const char*>(b.data()+p),4};
}
std::optional<ImageTrack> nrg_track(const std::filesystem::path& image,
                                      std::uint64_t begin,std::uint64_t end,
                                      std::uint16_t sector,std::uint16_t mode,
                                      unsigned track,std::uint32_t session_lba,
                                      std::string label) {
    // DAO modes are Nero's BE16 mode values, NOT CD sector mode numbers:
    // 0000h=Mode1 user data, 0300h=Mode2 Form1, 0500h=raw Mode1,
    // 0600h=raw Mode2 Form1. Audio, subchannels and Form2 are excluded.
    std::uint32_t offset=0;
    std::uint8_t cd_mode=0;
    if(sector==2048 && (mode==0x0000 || mode==0x0300)) {
        offset=0;cd_mode=0;
    } else if(sector==2336 && mode==0x0300) {
        offset=8;cd_mode=2;
    } else if(sector==2352 && mode==0x0500) {
        offset=16;cd_mode=1;
    } else if(sector==2352 && mode==0x0600) {
        offset=24;cd_mode=2;
    } else return std::nullopt;
    if(begin>=end || (end-begin)%sector || (end-begin)/sector<17)
        return std::nullopt;
    return ImageTrack{image,begin,end,sector,offset,cd_mode,session_lba,track,std::move(label)};
}
std::string utf8(std::uint32_t c) {
    check(c<=0x10ffff && !(c>=0xd800&&c<=0xdfff),"invalid Unicode in MDS filename");
    if(c<0x80)return std::string(1,static_cast<char>(c));
    if(c<0x800)return {char(0xc0|(c>>6)),char(0x80|(c&63))};
    if(c<0x10000)return {char(0xe0|(c>>12)),char(0x80|((c>>6)&63)),char(0x80|(c&63))};
    return {char(0xf0|(c>>18)),char(0x80|((c>>12)&63)),char(0x80|((c>>6)&63)),char(0x80|(c&63))};
}
std::filesystem::path same_directory_file(const std::filesystem::path& descriptor,
                                          const std::string& filename) {
    check(!filename.empty()&&filename.size()<=1024,"invalid MDS data filename");
    std::string clean=filename;
    std::replace(clean.begin(),clean.end(),'\\','/');
    for(unsigned char c:clean)check(c>=32 && c!=':',"invalid MDS data path");
    auto ref=std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(clean.data()),clean.size()));
    check(!ref.is_absolute() && !ref.has_root_name(),"absolute MDS filename");
    for(const auto& part:ref)check(part!=".."&&part!="."&&!part.empty(),"MDS filename escapes directory");
    const auto directory=std::filesystem::canonical(descriptor).parent_path();
    const auto full=std::filesystem::canonical(directory/ref);
    const auto relative=full.lexically_relative(directory);
    check(!relative.empty()&&!relative.is_absolute(),"MDS file escapes directory");
    for(const auto& part:relative)check(part!="..","MDS file escapes directory");
    check(std::filesystem::is_regular_file(full),"MDS image must be a regular file");
    return full;
}
std::string mds_filename(std::span<const std::uint8_t> data,std::size_t block) {
    region(data,block,16);
    auto pointer=le32(data,block);
    auto encoding=data[block+4];
    check(encoding<=1,"unsupported MDS filename encoding");
    check(pointer<data.size(),"MDS filename pointer outside descriptor");
    std::string name;
    bool terminated=false;
    for(unsigned i=0;i<1024;i++) {
        if(encoding==0) {
            region(data,pointer+i,1);
            const auto c=data[pointer+i];
            if(c==0){terminated=true;break;}
            name+=static_cast<char>(c);
        } else {
            region(data,pointer+2*i,2);
            auto c=le16(data,pointer+2*i);
            if(c==0){terminated=true;break;}
            if(c>=0xd800&&c<=0xdbff) {
                check(i<1023,"MDS name missing low surrogate");
                region(data,pointer+2*(i+1),2);
                auto d=le16(data,pointer+2*(++i));
                check(d>=0xdc00&&d<=0xdfff,"bad MDS filename surrogate");
                name+=utf8(0x10000+((c-0xd800)<<10)+(d-0xdc00));
            } else name+=utf8(c);
        }
    }
    check(terminated,"unterminated MDS filename");
    return name;
}
}
bool is_nrg_path(const std::filesystem::path& image) {
    return uppercase(image.extension().string())==".NRG";
}
bool is_mds_path(const std::filesystem::path& descriptor) {
    return uppercase(descriptor.extension().string())==".MDS";
}
bool is_mdx_path(const std::filesystem::path& image) {
    return uppercase(image.extension().string())==".MDX";
}

std::vector<ImageTrack> nrg_data_tracks(const std::filesystem::path& image) {
    CheckedFile file(image);
    check(file.size()>=12,"NRG footer truncated");
    const auto tail=file.read(file.size()-12,12);
    std::uint64_t chunk_start=0,footer_start=0;
    if(fourcc(tail,0)=="NER5") {
        chunk_start=be64(tail,4);footer_start=file.size()-12;
    } else {
        const auto old=file.read(file.size()-8,8);
        check(fourcc(old,0)=="NERO","NRG trailer signature not found");
        chunk_start=be32(old,4);footer_start=file.size()-8;
    }
    check(chunk_start>=2048 && chunk_start<footer_start &&
          footer_start-chunk_start<=16*1024*1024,
          "NRG chunk table outside supported bounds");
    auto metadata=file.read(chunk_start,static_cast<std::size_t>(footer_start-chunk_start));
    std::vector<ImageTrack> tracks;
    bool ended=false;
    unsigned track_number=0;
    for(std::size_t at=0;at+8<=metadata.size();) {
        const auto id=fourcc(metadata,at);
        auto size=static_cast<std::size_t>(be32(metadata,at+4));
        at+=8;region(metadata,at,size);
        auto chunk=std::span<const std::uint8_t>(metadata).subspan(at,size);
        if(id=="END!") {check(size==0 && at==metadata.size(),"NRG END! is not terminal");ended=true;break;}
        if(id=="DAOI"||id=="DAOX") {
            check(chunk.size()>=22,"NRG DAO chunk truncated");
            const unsigned first=chunk[20],last=chunk[21];
            check(first>=1 && last<=99 && first<=last,"invalid NRG DAO track numbers");
            const std::size_t step=id=="DAOX"?42:30;
            check(last-first+1 <= (chunk.size()-22)/step,"truncated NRG DAO track table");
            for(unsigned n=first;n<=last;n++) {
                const auto p=22+std::size_t(n-first)*step;
                auto sector=be16(chunk,p+12);
                auto mode=be16(chunk,p+14);
                auto index0=id=="DAOX"?be64(chunk,p+18):be32(chunk,p+18);
                auto index1=id=="DAOX"?be64(chunk,p+26):be32(chunk,p+22);
                auto end=id=="DAOX"?be64(chunk,p+34):be32(chunk,p+26);
                check(index0<=index1 && index1<end && end<=chunk_start,
                      "NRG DAO track extent outside image");
                if(auto t=nrg_track(image,index1,end,sector,mode,n,0,"NRG DAO track "+std::to_string(n)))
                    tracks.push_back(std::move(*t));
            }
        } else if(id=="ETNF"||id=="ETN2") {
            const std::size_t step=id=="ETN2"?32:20;
            check(size%step==0 && size/step<=100,"invalid NRG TAO track table");
            for(std::size_t i=0;i<size/step;i++) {
                const auto p=i*step;
                const auto start=id=="ETN2"?be64(chunk,p):be32(chunk,p);
                const auto bytes=id=="ETN2"?be64(chunk,p+8):be32(chunk,p+4);
                const auto mode=be32(chunk,p+(id=="ETN2"?16:8));
                const auto lba=be32(chunk,p+(id=="ETN2"?20:12));
                check(start<=chunk_start && bytes<=chunk_start-start && bytes>0,
                      "NRG TAO track outside image");
                // Nero TAO modes: 0=Mode1 data, 3=Mode2 data, 7=audio.
                // ETNF/ETN2 do not specify sector size: try known layouts
                // and trust the ISO/UDF signature *inside* the track.
                if(mode!=0 && mode!=3)continue;
                const unsigned n=++track_number;
                for(const auto& layout:std::array{
                    std::array<unsigned,3>{2352u,mode==3?24u:16u,mode==3?2u:1u},
                    std::array<unsigned,3>{2336u,8u,2u},
                    std::array<unsigned,3>{2048u,0u,0u}}) {
                    const auto physical=layout[0];
                    if(physical==2336 && mode!=3)continue;
                    if(bytes%physical || bytes/physical<17)continue;
                    tracks.push_back({image,start,start+bytes,physical,layout[1],
                                      static_cast<std::uint8_t>(layout[2]),lba,n,
                                      "NRG TAO track "+std::to_string(n)});
                }
            }
        }
        at+=size;
    }
    check(ended,"NRG chunk table has no END! marker");
    check(!tracks.empty(),"NRG has no supported uncompressed data tracks");
    return tracks;
}

std::vector<ImageTrack> mds_data_tracks(const std::filesystem::path& descriptor) {
    CheckedFile file(descriptor);
    check(file.size()>=88 && file.size()<=16*1024*1024,"MDS descriptor size unsupported");
    auto bytes=file.read(0,static_cast<std::size_t>(file.size()));
    const std::span<const std::uint8_t> b(bytes);
    check(std::string_view(reinterpret_cast<const char*>(b.data()),16)=="MEDIA DESCRIPTOR",
          "invalid MDS signature");
    if(b[16]==2) {
        // Confirm the distinct encrypted/packed descriptor layout instead of
        // letting an MDSv2 pointer masquerade as an MDSv1 track table.
        (void)probe_mds_descriptor(descriptor);
        throw std::runtime_error(
            "MDS v2 recognized; encrypted/packed metadata and track decoding "
            "are not implemented in ISO-WCX");
    }
    check(b[16]==1,"unsupported MDS major version");
    const auto sessions=le16(b,0x14);
    const auto offset=le32(b,0x50);
    check(sessions>0 && sessions<=99,"invalid MDS session count");
    check(std::uint64_t(offset)+std::uint64_t(sessions)*24<=b.size(),
          "MDS session table outside descriptor");
    std::vector<ImageTrack> tracks;
    for(unsigned session=0;session<sessions;session++) {
        const auto at=std::size_t(offset)+session*24;
        const auto count=b[at+0x0a];
        const auto table=le32(b,at+0x14);
        check(count<=102 && std::uint64_t(table)+std::uint64_t(count)*80<=b.size(),
              "MDS track table outside descriptor");
        for(unsigned i=0;i<count;i++) {
            const auto p=std::size_t(table)+i*80;
            const auto point=b[p+4],kind=b[p],sub=b[p+1];
            if(point<1 || point>99)continue; // lead-in TOC entries
            // Interleaved subchannels, redbook audio and unsupported sector modes
            // are ignored (never parsed as 2048-byte payloads).
            if((kind!=0xaa && kind!=0xab && kind!=0xac))continue;
            // 2448 = 2352-byte raw sector plus 96 bytes of interleaved
            // subchannel data.  The subchannel bytes are not filesystem data.
            // Reject all other unsupported subchannel arrangements.
            if(sub!=0 && !(sub==8 && le16(b,p+0x10)==2448))continue;
            const auto physical=le16(b,p+0x10);
            std::uint32_t data_offset=0;
            std::uint8_t mode=0;
            if(physical==2048) {data_offset=0;mode=0;}
            else if(physical==2336 && kind==0xab) {data_offset=8;mode=2;}
            else if(physical==2352 || physical==2448) {
                mode=kind==0xaa?1:2;
                data_offset=mode==1?16:24;
            } else continue;
            const auto index_block=le32(b,p+0x0c);
            region(b,index_block,8);
            const auto index1_sectors=le32(b,index_block+4);
            check(index1_sectors>0,"MDS empty track data");
            const auto start=le64(b,p+0x28);
            const auto filename_block=le32(b,p+0x34);
            std::string filename="*.mdf";
            if(filename_block)filename=mds_filename(b,filename_block);
            if(uppercase(filename)=="*.MDF") {
                // Use the descriptor's stem, not an arbitrary wildcard glob.
                filename=descriptor.stem().string()+".mdf";
            }
            const auto path=same_directory_file(descriptor,filename);
            CheckedFile mdf(path);
            const auto size=std::uint64_t(index1_sectors)*physical;
            check(start<=mdf.size() && size<=mdf.size()-start,
                  "MDS track extends past MDF");
            tracks.push_back({path,start,start+size,physical,data_offset,mode,
                             le32(b,p+0x24),point,
                             "MDS session "+std::to_string(session+1)+" track "+std::to_string(point)});
        }
    }
    check(!tracks.empty(),"MDS has no supported uncompressed data tracks");
    return tracks;
}
} // namespace iso_wcx
