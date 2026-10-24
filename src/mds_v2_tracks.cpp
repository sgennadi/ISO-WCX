#include "iso_wcx/mds_v2_tracks.hpp"
#include "iso_wcx/mds_v2.hpp"
#include "iso_wcx/mds_probe.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <zlib.h>

namespace iso_wcx {
namespace {
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
constexpr u32 logical_sector = 2048;
constexpr u64 max_sectors = (u64(1)<<34);
constexpr std::size_t max_group_decoded = 32u * 1024u * 1024u;
constexpr std::size_t max_table_bytes = 32u * 1024u * 1024u;
constexpr std::size_t max_metadata = 16u * 1024u * 1024u;

void ensure(bool condition,const char* why) {
    if(!condition) throw std::runtime_error(why);
}
u16 le16(const u8* p) noexcept {
    return u16(u16(p[0]) | u16(u16(p[1])<<8));
}
u32 le32(const u8* p) noexcept {
    return u32(p[0])|(u32(p[1])<<8)|(u32(p[2])<<16)|(u32(p[3])<<24);
}
u64 le64(const u8* p) noexcept {
    return u64(le32(p))|(u64(le32(p+4))<<32);
}
void bounds(const std::vector<u8>& b,u64 at,u64 n,const char* why) {
    ensure(at<=b.size() && n<=b.size()-at,why);
}
std::string utf8_codepoint(u32 value) {
    ensure(value<=0x10ffff && !(value>=0xd800 && value<=0xdfff),
           "MDSv2 filename has invalid Unicode codepoint");
    if(value<128)return std::string(1,char(value));
    if(value<2048)return {char(0xc0|(value>>6)),char(0x80|(value&63))};
    if(value<65536)return {
        char(0xe0|(value>>12)),char(0x80|((value>>6)&63)),
        char(0x80|(value&63))};
    return {char(0xf0|(value>>18)),char(0x80|((value>>12)&63)),
            char(0x80|((value>>6)&63)),char(0x80|(value&63))};
}
std::string v2_filename(const std::vector<u8>& metadata,u32 offset) {
    bounds(metadata,offset,2,"MDS v2 footer filename offset is invalid");
    std::string result;
    bool terminated=false;
    for(std::size_t chars=0;chars<255;++chars) {
        const u64 at=u64(offset)+chars*2u;
        bounds(metadata,at,2,"MDS v2 footer filename is unterminated");
        u32 point=le16(metadata.data()+at);
        if(point==0) {terminated=true;break;}
        if(point>=0xd800 && point<=0xdbff) {
            bounds(metadata,at+2,2,"MDS v2 footer truncated UTF-16 surrogate");
            const auto low=le16(metadata.data()+at+2);
            ensure(low>=0xdc00 && low<=0xdfff,
                   "MDS v2 footer filename has invalid UTF-16 surrogate");
            point=0x10000u+((point-0xd800u)<<10)+(low-0xdc00u);
            ++chars;
        }
        result+=utf8_codepoint(point);
        ensure(result.size()<=1024,"MDS v2 footer filename is oversized");
    }
    ensure(terminated,"MDS v2 footer filename is unterminated");
    return result;
}
std::filesystem::path sibling(const std::filesystem::path& descriptor,
                              std::string filename) {
    if(filename.empty() || filename=="*.mdf" || filename=="*.MDF")
        filename=descriptor.stem().string()+".mdf";
    ensure(!filename.empty() && filename!="." && filename!=".." &&
            filename.size()<=1024 &&
            filename.find_first_of("/\\:*?\"<>|") == std::string::npos,
            "unsafe MDS v2 companion filename");
    for(unsigned char c:filename)
        ensure(c>=32,"MDS v2 companion filename contains control characters");
    const auto relative=std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(filename.data()),filename.size()));
    ensure(!relative.is_absolute() && !relative.has_parent_path() &&
            !relative.has_root_name(),
            "MDS v2 companion file must be in the descriptor directory");
    const auto result=descriptor.parent_path()/relative;
    ensure(std::filesystem::is_regular_file(result) &&
            !std::filesystem::is_symlink(result),
            "MDS v2 companion MDF data file is missing or unsafe");
    return result;
}
struct CheckedInput {
    std::ifstream input;
    u64 length=0;
    explicit CheckedInput(const std::filesystem::path& path):
        input(path,std::ios::binary|std::ios::ate) {
        ensure(std::filesystem::is_regular_file(path) &&
               !std::filesystem::is_symlink(path),
               "MDS v2 data source is not a regular non-symlink file");
        ensure(bool(input),"cannot open MDS v2 data source");
        const auto end=input.tellg();
        ensure(end>=0,"cannot determine MDS v2 data source length");
        length=static_cast<u64>(end);
    }
    void copy(u64 at,u8* destination,std::size_t bytes) {
        ensure(at<=length && bytes<=length-at &&
               at<=static_cast<u64>(std::numeric_limits<std::streamoff>::max()),
               "MDS v2 data fragment lies outside its input file");
        input.clear();
        input.seekg(static_cast<std::streamoff>(at),std::ios::beg);
        ensure(bool(input),"MDS v2 data seek failed");
        input.read(reinterpret_cast<char*>(destination),
                   static_cast<std::streamsize>(bytes));
        ensure(std::size_t(input.gcount())==bytes,
               "MDS v2 data fragment is truncated");
    }
    std::vector<u8> read(u64 at,std::size_t bytes) {
        std::vector<u8> result(bytes);
        copy(at,result.data(),bytes);
        return result;
    }
};
struct Fragment {
    std::filesystem::path file;
    u64 start=0;
    u64 sectors=0;
    u64 logical_start=0;
    u32 sector_bytes=0;
    u32 compression_group=0;
    bool compressed=false;
    std::vector<u16> entries;
    // Compressed block offsets relative to the data start. Repeated-byte
    // blocks have no payload and therefore do not advance this index.
    std::vector<u64> offsets;
};
struct Track {
    unsigned point=0;
    unsigned session=0;
    u32 sector_bytes=0;
    u32 data_offset=0;
    u64 sectors=0;
    std::vector<Fragment> parts;
};
bool is_iso(std::span<const u8> p) {
    return p.size()>=7 && p[0]==1 && std::memcmp(p.data()+1,"CD001",5)==0
           && p[6]==1;
}
bool is_udf(std::span<const u8> p) {
    if(p.size()<16 || p[0]!=2 || p[1]!=0)return false;
    unsigned sum=0;
    for(unsigned i=0;i<16;++i)if(i!=4)sum+=p[i];
    return (sum&255u)==p[4];
}
std::vector<u8> inflate_checked(const std::vector<u8>& compressed,
                                std::size_t expected,bool raw_deflate) {
    ensure(expected>0 && expected<=max_group_decoded,
           "MDS v2 inflated data group exceeds safety limit");
    std::vector<u8> output(expected);
    z_stream state{};
    const int init=inflateInit2(&state,raw_deflate ? -MAX_WBITS : MAX_WBITS);
    ensure(init==Z_OK,"MDS v2 zlib decoder initialization failed");
    state.next_in=const_cast<Bytef*>(compressed.data());
    state.avail_in=static_cast<uInt>(compressed.size());
    state.next_out=output.data();
    state.avail_out=static_cast<uInt>(output.size());
    const int status=inflate(&state,Z_FINISH);
    const auto produced=state.total_out;
    inflateEnd(&state);
    ensure(status==Z_STREAM_END && produced==output.size(),
           "MDS v2 group is truncated or decompressed to an invalid size");
    return output;
}
} // namespace

struct MdsV2TrackStream::Impl {
    explicit Impl(const std::filesystem::path& descriptor);
    std::map<std::filesystem::path,CheckedInput> files;
    std::vector<Track> tracks;
    std::size_t selected=0;
    u64 leading=0;
    u64 logical_size=0;
    u32 selected_offset=0;
    std::string format;

    const Fragment* cache_fragment=nullptr;
    std::size_t cache_group=std::numeric_limits<std::size_t>::max();
    std::vector<u8> cache;

    CheckedInput& file(const std::filesystem::path& path) {
        return files.try_emplace(path,path).first->second;
    }
    std::vector<u8> decompress_group(const Fragment& frag,std::size_t group_index) {
        ensure(group_index<frag.entries.size(),"invalid MDS v2 group index");
        const u64 first=u64(group_index)*frag.compression_group;
        const u64 count=std::min<u64>(frag.compression_group,frag.sectors-first);
        ensure(count<=max_group_decoded/frag.sector_bytes,
               "MDS v2 group declares oversized decoded data");
        const std::size_t bytes=std::size_t(count)*frag.sector_bytes;
        const u16 marker=frag.entries[group_index];
        if(marker&0x8000u) return std::vector<u8>(bytes,u8(marker&255u));
        const auto encoded=marker ? marker : bytes;
        const u64 pos=frag.start+frag.offsets[group_index];
        const auto payload=file(frag.file).read(pos,encoded);
        if(marker==0)return payload;
        return inflate_checked(payload,bytes,true);
    }
    void read_sector(const Track& track,u64 sector,u8* buffer) {
        ensure(sector<track.sectors,"MDS v2 sector index exceeds track");
        for(const auto& part:track.parts) {
            if(sector<part.logical_start ||
               sector-part.logical_start>=part.sectors)continue;
            const u64 local=sector-part.logical_start;
            if(!part.compressed) {
                const u64 at=part.start+local*track.sector_bytes;
                file(part.file).copy(at,buffer,track.sector_bytes);
                return;
            }
            const std::size_t group=static_cast<std::size_t>(
                local/part.compression_group);
            if(cache_fragment!=&part || cache_group!=group) {
                cache=decompress_group(part,group);
                cache_fragment=&part;
                cache_group=group;
            }
            const auto within=std::size_t(local%part.compression_group)*
                              track.sector_bytes;
            ensure(within<=cache.size() &&
                   track.sector_bytes<=cache.size()-within,
                   "MDS v2 compressed sector is outside group");
            std::copy_n(cache.data()+within,track.sector_bytes,buffer);
            return;
        }
        throw std::runtime_error("MDS v2 track has a missing data fragment");
    }
    bool has_anchor(Track& t,u64 lead,u32 offset,bool udf) {
        const u64 relative=lead+(udf?256u:16u);
        if(relative>=t.sectors || offset>t.sector_bytes-16u)return false;
        std::vector<u8> sector(t.sector_bytes);
        read_sector(t,relative,sector.data());
        const auto span=std::span<const u8>(sector.data()+offset,16);
        return udf ? is_udf(span) : is_iso(span);
    }
    std::vector<u8> read(u64 offset,std::size_t count) {
        ensure(count<=32u*1024u*1024u &&
               offset<=logical_size && count<=logical_size-offset,
               "MDS v2 read exceeds selected filesystem track");
        std::vector<u8> result(count);
        std::size_t copied=0;
        std::vector<u8> physical(tracks[selected].sector_bytes);
        while(copied<count) {
            const u64 position=offset+copied;
            const u64 sector=leading+position/logical_sector;
            const auto inside=std::size_t(position%logical_sector);
            read_sector(tracks[selected],sector,physical.data());
            const auto amount=std::min<std::size_t>(
                count-copied,logical_sector-inside);
            std::copy_n(physical.data()+selected_offset+inside,
                        amount,result.data()+copied);
            copied+=amount;
        }
        return result;
    }
};

MdsV2TrackStream::Impl::Impl(const std::filesystem::path& descriptor) {
    const auto checked=probe_mds_descriptor(descriptor);
    ensure(checked.kind!=MdsDescriptorKind::alcohol_v1,
           "MDS v2 data reader cannot open an MDS v1 descriptor");
    const auto metadata=decode_mds_v2_descriptor(descriptor);
    ensure(metadata.size()>=0x60 && metadata.size()<=max_metadata+18,
           "decrypted MDS v2 descriptor is oversized or truncated");
    ensure(le32(metadata.data()+0x58)==0,
           "MDS v2/MDX encrypted data sectors require AES-LRW support");
    const auto sessions=le16(metadata.data()+0x14);
    const auto session_offset=le32(metadata.data()+0x50);
    ensure(sessions>0 && sessions<=99 &&
           u64(session_offset)+u64(sessions)*32<=metadata.size(),
           "MDS v2 session table lies outside decrypted descriptor");
    const u16 medium=le16(metadata.data()+0x12);
    const bool dvd=medium==3 || medium==0x10 || medium==0x12;
    unsigned total_tracks=0;
    for(unsigned session=0;session<sessions;++session) {
        const auto base=u64(session_offset)+u64(session)*32;
        const auto entries=metadata[base+0x0a];
        const auto table=le32(metadata.data()+base+0x14);
        ensure(entries<=102 &&
               u64(table)+u64(entries)*80<=metadata.size(),
               "MDS v2 track table lies outside decrypted descriptor");
        for(unsigned i=0;i<entries;++i) {
            const auto at=u64(table)+u64(i)*80u;
            const auto kind=unsigned(metadata[at])&0x0fu;
            const auto mode=kind>=8 ? kind-8 : kind;
            const auto point=unsigned(metadata[at+4]);
            if(point<1 || point>99 || mode==1 || mode==5)continue;
            const bool mode1=mode==2;
            const bool mode2=mode==0 || mode==3 || mode==4 || mode==7;
            if(!mode1 && !mode2)continue;
            const auto physical=le16(metadata.data()+at+0x10);
            u32 offset=0;
            if(physical==2048 && mode1)offset=0;
            else if(physical==2336 && mode2)offset=8;
            else if(physical==2352 || physical==2448)
                offset=mode1?16u:24u;
            else continue;
            if(physical==2448) {
                if(metadata[at+1]!=8)continue;
            } else if(metadata[at+1]!=0)continue;
            const auto track_start=le64(metadata.data()+at+0x28);
            const auto footer_at=le32(metadata.data()+at+0x34);
            const auto footer_count=le32(metadata.data()+at+0x30);
            const auto extra_at=le32(metadata.data()+at+0x0c);
            const auto logical_count=dvd?
                le64(metadata.data()+at+0x40):
                (extra_at && u64(extra_at)+8<=metadata.size() ?
                    u64(le32(metadata.data()+extra_at+4)):u64(0));
            Track track{};
            track.point=point;
            track.session=session+1;
            track.sector_bytes=physical;
            track.data_offset=offset;
            if(footer_at!=0) {
                const auto n=std::clamp<u32>(footer_count,1u,64u);
                bounds(metadata,footer_at,u64(n)*32,
                       "MDS v2 footer extends beyond decrypted descriptor");
                for(u32 j=0;j<n;++j) {
                    const u64 pos=u64(footer_at)+u64(j)*32;
                    const auto name_offset=le32(metadata.data()+pos);
                    const auto flags=metadata[pos+4];
                    const auto group=le32(metadata.data()+pos+12);
                    u64 sectors=le64(metadata.data()+pos+16);
                    const u64 table_offset=le64(metadata.data()+pos+24);
                    if(sectors==0 && j==0)sectors=logical_count;
                    if(sectors==0)continue;
                    ensure(sectors<=max_sectors &&
                           track.sectors<=max_sectors-sectors,
                           "MDS v2 declared track sector count is unreasonably large");
                    std::filesystem::path image=descriptor;
                    if(checked.kind!=MdsDescriptorKind::daemon_mdx) {
                        std::string name=name_offset?
                            v2_filename(metadata,name_offset):"*.mdf";
                        image=sibling(descriptor,std::move(name));
                    }
                    Fragment fragment{};
                    fragment.file=std::move(image);
                    fragment.start=j==0?track_start:0;
                    fragment.sectors=sectors;
                    fragment.logical_start=track.sectors;
                    fragment.sector_bytes=physical;
                    fragment.compressed=(flags&1u)!=0;
                    fragment.compression_group=group;
                    auto& source=file(fragment.file);
                    if(fragment.compressed) {
                        ensure(group>0 && group<=max_group_decoded/physical,
                               "MDS v2 compressed group sector count is invalid");
                        const u64 blocks=(sectors+group-1u)/group;
                        ensure(blocks>0 && blocks<=max_table_bytes/2,
                               "MDS v2 compression table is unreasonably large");
                        const std::size_t expected=std::size_t(blocks)*2u;
                        ensure(fragment.start<=source.length &&
                               table_offset<=source.length-fragment.start,
                               "MDS v2 compressed fragment table offset exceeds MDF/MDX");
                        const auto table_start=fragment.start+table_offset;
                        const auto limit=std::min<u64>(
                            u64(expected)+4096,source.length-table_start);
                        ensure(limit>0 && limit<=max_table_bytes+4096,
                               "MDS v2 compressed block table is absent or oversized");
                        const auto compressed=source.read(
                            table_start,static_cast<std::size_t>(limit));
                        const auto decoded=inflate_checked(
                            compressed,expected,false);
                        fragment.entries.reserve(static_cast<std::size_t>(blocks));
                        fragment.offsets.reserve(static_cast<std::size_t>(blocks));
                        u64 cumulative=0;
                        for(u64 k=0;k<blocks;++k) {
                            const auto marker=le16(decoded.data()+std::size_t(k)*2u);
                            fragment.offsets.push_back(cumulative);
                            fragment.entries.push_back(marker);
                            const auto count=std::min<u64>(group,sectors-k*group);
                            const u64 length=marker&0x8000u ? 0u :
                                (marker?marker:count*physical);
                            ensure(cumulative<=source.length-fragment.start &&
                                   length<=source.length-fragment.start-cumulative,
                                   "MDS v2 compressed group exceeds MDF/MDX length");
                            cumulative+=length;
                        }
                    } else {
                        ensure(fragment.start<=source.length &&
                               sectors<=(source.length-fragment.start)/physical,
                               "MDS v2 plain track exceeds companion MDF/MDX");
                    }
                    track.sectors+=sectors;
                    track.parts.push_back(std::move(fragment));
                }
            }
            if(track.parts.empty() && logical_count>0) {
                ensure(logical_count<=max_sectors,
                       "MDS v2 plain track declares too many sectors");
                Fragment part{};
                part.file=checked.kind==MdsDescriptorKind::daemon_mdx?
                    descriptor:sibling(descriptor,"*.mdf");
                part.start=track_start;
                part.sectors=logical_count;
                part.sector_bytes=physical;
                auto& source=file(part.file);
                ensure(part.start<=source.length &&
                       logical_count<=(source.length-part.start)/physical,
                       "MDS v2 plain track extends past its source");
                track.sectors=logical_count;
                track.parts.push_back(std::move(part));
            }
            if(track.parts.empty())continue;
            tracks.push_back(std::move(track));
            ensure(++total_tracks<=99,"MDS v2 contains too many supported tracks");
        }
    }
    ensure(!tracks.empty(),"MDS v2 contains no supported data tracks");
    bool found=false;
    for(std::size_t r=tracks.size();r>0 && !found;--r) {
        auto& track=tracks[r-1];
        auto try_lead=[&](u64 shift)->bool {
            for(const auto off:{track.data_offset,0u,16u,24u}) {
                if(off>track.sector_bytes-logical_sector)continue;
                if(!has_anchor(track,shift,off,false) &&
                   !has_anchor(track,shift,off,true))continue;
                selected=r-1;
                leading=shift;
                selected_offset=off;
                logical_size=(track.sectors-shift)*logical_sector;
                format="MDS v2 / ";
                if(checked.kind==MdsDescriptorKind::daemon_mdx)
                    format="MDX / ";
                format+="native ";
                format+=track.parts.size()==1?"single-fragment":"multi-fragment";
                format+=" track "+std::to_string(track.point);
                format+=" (session "+std::to_string(track.session)+")";
                return true;
            }
            return false;
        };
        if(try_lead(0)||try_lead(150)) {
            found=true;break;
        }
        const auto limit=std::min<u64>(512,track.sectors>256?
                                                track.sectors-257:0u);
        for(u64 lead=1;lead<=limit;++lead)
            if(lead!=150 && try_lead(lead)) {
                found=true;break;
            }
    }
    ensure(found,"MDS v2/MDX has no validated ISO/UDF Mode 1/2 Form 1 data track");
}
MdsV2TrackStream::MdsV2TrackStream(const std::filesystem::path& descriptor):
    impl_(std::make_unique<Impl>(descriptor)) {}
MdsV2TrackStream::~MdsV2TrackStream() = default;
std::uint64_t MdsV2TrackStream::size() const noexcept {
    return impl_->logical_size;
}
const std::string& MdsV2TrackStream::label() const noexcept {
    return impl_->format;
}
std::vector<std::uint8_t> MdsV2TrackStream::read(std::uint64_t start,
                                                 std::size_t length) {
    return impl_->read(start,length);
}
} // namespace iso_wcx
