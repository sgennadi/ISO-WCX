#include "iso_wcx/disc.hpp"
#include "iso_wcx/cd_sector.hpp"
#include "iso_wcx/subq.hpp"
#include "iso_wcx/compressed_image.hpp"
#include "iso_wcx/packed_optical.hpp"
#include "iso_wcx/cue.hpp"
#include "iso_wcx/image_tracks.hpp"
#include "iso_wcx/mds_probe.hpp"
#include "iso_wcx/rock_ridge.hpp"
#include "iso_wcx/legacy_formats.hpp"
#include "iso_wcx/virtual_archive.hpp"
#ifdef _WIN32
#include "iso_wcx/wim_archive.hpp"
#endif
#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <cctype>
#include <cwctype>
#include <exception>
#include <string_view>
#include <random>
#include <system_error>

namespace iso_wcx {
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::uint64_t iso_sector = 2048;
constexpr std::size_t max_directory = 32u * 1024 * 1024;
constexpr std::size_t max_entries = 100000;

void require(bool ok, std::string_view reason) {
    if (!ok) throw std::runtime_error(std::string(reason));
}
void bounds(const Bytes& b, std::size_t p, std::size_t n) {
    require(p <= b.size() && n <= b.size()-p, "truncated disc descriptor");
}
std::uint16_t u16(const Bytes& b, std::size_t p) {
    bounds(b,p,2); return std::uint16_t(b[p] | (std::uint16_t(b[p+1]) << 8));
}
std::uint32_t u32(const Bytes& b, std::size_t p) {
    bounds(b,p,4); return std::uint32_t(u16(b,p)) | (std::uint32_t(u16(b,p+2)) << 16);
}
std::uint64_t u64(const Bytes& b, std::size_t p) {
    bounds(b,p,8); return std::uint64_t(u32(b,p)) | (std::uint64_t(u32(b,p+4)) << 32);
}
std::uint16_t be16(const Bytes& b,std::size_t p) {
    bounds(b,p,2);return std::uint16_t((std::uint16_t(b[p])<<8) | b[p+1]);
}
std::string utf8(std::uint32_t c) {
    require(c<=0x10ffff && !(c>=0xd800 && c<=0xdfff),"invalid Unicode code point");
    if(c<128) return std::string(1,char(c));
    if(c<2048) return {char(0xc0|(c>>6)),char(0x80|(c&63))};
    if(c<65536) return {char(0xe0|(c>>12)),char(0x80|((c>>6)&63)),char(0x80|(c&63))};
    return {char(0xf0|(c>>18)),char(0x80|((c>>12)&63)),char(0x80|((c>>6)&63)),char(0x80|(c&63))};
}
void safe_name(const std::string& s) {
    require(!s.empty() && s!="." && s!=".." && s.back()!=' ' && s.back()!='.',"unsafe file name");
    require(s.size()<=250,"file name too long");
    for(unsigned char c: s) require(c>=32 && c!='/' && c!='\\' && c!=':' && c!='*' && c!='?' && c!='\"' && c!='<' && c!='>' && c!='|',"unsafe file name");
    std::string base=s.substr(0,s.find('.'));
    std::transform(base.begin(),base.end(),base.begin(),[](unsigned char c){return static_cast<char>(std::toupper(c));});
    require(base!="CON" && base!="PRN" && base!="AUX" && base!="NUL" && base!="CLOCK$","Windows reserved file name");
    if(base.size()==4 && ((base.substr(0,3)=="COM")||(base.substr(0,3)=="LPT")))
        require(base[3]<'1'||base[3]>'9',"Windows device file name");
}

class Source {
    std::ifstream file_;
    std::unique_ptr<CompressedImage> compressed_;
    std::unique_ptr<PackedOptical> packed_;
    std::uint64_t raw_length_{};
    std::uint64_t logical_length_{};
    std::uint64_t raw_base_{};
    std::uint64_t raw_end_{};
    std::optional<std::uint32_t> session_lba_;
    std::string track_label_;
    std::uint32_t physical_sector_ = 2048;
    std::uint32_t data_offset_ = 0;
    std::uint32_t leading_sectors_ = 0;
    std::uint8_t mode_ = 0;
    bool scrambled_ = false;
    std::optional<unsigned> cue_track_number_;

    Bytes raw_read(std::uint64_t p, std::size_t n) {
        require(p<=raw_end_ && n<=raw_end_-p &&
                p<=static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()),
                "raw extent outside image");
        if(compressed_)return compressed_->read(p,n);
        if(packed_)return packed_->read(p,n);
        Bytes b(n);
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(p));
        require(bool(file_) && (n==0 || bool(file_.read(reinterpret_cast<char*>(b.data()),
                static_cast<std::streamsize>(n)))),"raw image read failed");
        if(scrambled_) {
            // Do not trust the raw header: the LFSR is reset per physical
            // sector, including when the caller only reads a small slice.
            require(p>=raw_base_,"CloneCD scrambled sector offset precedes track");
            const auto& mask=cd_scramble_mask();
            for(std::size_t i=0;i<b.size();++i) {
                const auto local=(p-raw_base_+i)%2352u;
                if(local>=12) b[i]^=mask[static_cast<std::size_t>(local-12)];
            }
        }
        return b;
    }
    bool candidate(std::uint32_t physical,std::uint32_t data_offset,
                   std::uint32_t leading,std::uint8_t mode) {
        constexpr std::uint64_t vd_sector = 16;
        const auto record = raw_base_+(std::uint64_t(leading)+vd_sector)*physical;
        if(record>raw_end_ || raw_end_-record<physical)return false;
        auto frame=raw_read(record,physical);
        if (physical==2352 || physical==2448) {
            if (frame[0]!=0 || frame[11]!=0 || frame[15]!=mode) return false;
            for(int i=1;i<=10;i++)if(frame[i]!=0xff)return false;
            if(mode==2) {
                if (!std::equal(frame.begin()+16,frame.begin()+20,frame.begin()+20) ||
                    (frame[18]&0x20))return false; // XA Form 2 has 2324 data bytes.
            }
        }
        if (physical==2336 && ((frame[2]&0x20)!=0 ||
            !std::equal(frame.begin(),frame.begin()+4,frame.begin()+4)))return false;
        if(data_offset>frame.size() || 7>frame.size()-data_offset)return false;
        return frame[data_offset]==1 &&
               std::string_view(reinterpret_cast<const char*>(frame.data()+data_offset+1),5)=="CD001" &&
               frame[data_offset+6]==1;
    }
    bool candidate_udf(std::uint32_t physical,std::uint32_t offset,
                       std::uint32_t leading,std::uint8_t mode) {
        const auto physical_at=raw_base_+(std::uint64_t(leading)+256)*physical;
        if(physical_at>raw_end_ || physical>raw_end_-physical_at)return false;
        const auto frame=raw_read(physical_at,physical);
        if(physical==2352 || physical==2448) {
            if(frame[0]!=0 || frame[11]!=0 || frame[15]!=mode)return false;
            for(int i=1;i<=10;i++)if(frame[i]!=0xff)return false;
            if(mode==2 && ((frame[18]&0x20) ||
                 !std::equal(frame.begin()+16,frame.begin()+20,frame.begin()+20)))return false;
        }
        if(physical==2336 && ((frame[2]&0x20) ||
            !std::equal(frame.begin(),frame.begin()+4,frame.begin()+4)))return false;
        if(offset>frame.size() || 16>frame.size()-offset)return false;
        const auto* data=frame.data()+offset;
        if(data[0]!=2 || data[1]!=0)return false; // Anchor Volume Descriptor Pointer, tag 2
        unsigned sum=0;
        for(int i=0;i<16;i++)if(i!=4)sum+=data[i];
        return (sum&255)==data[4];
    }
    void choose(std::uint32_t physical,std::uint32_t offset,std::uint32_t leading,
                std::uint8_t mode) {
        physical_sector_=physical;data_offset_=offset;leading_sectors_=leading;mode_=mode;
        require(raw_base_<=raw_end_ && raw_end_-raw_base_>=std::uint64_t(leading)*physical,
                "invalid track prefix or end bound");
        const auto sectors = (raw_end_-raw_base_)/physical - leading;
        require(sectors<=std::numeric_limits<std::uint64_t>::max()/2048,
                "image too large");
        logical_length_ = sectors*2048;
    }
    void open_image(const std::filesystem::path& p) {
        compressed_.reset();
        packed_.reset();
        file_.close();
        file_.clear();
        file_.open(p,std::ios::binary|std::ios::ate);
        require(file_.is_open(),"cannot open disc image");
        auto n=file_.tellg();require(n>=0,"cannot stat disc image");
        raw_length_=static_cast<std::uint64_t>(n);
        raw_base_=0;raw_end_=raw_length_;session_lba_.reset();track_label_.clear();scrambled_=false;
        logical_length_=raw_length_;
        if(is_compressed_image_path(p)) {
            compressed_=std::make_unique<CompressedImage>(p);
            raw_length_=compressed_->size();
            raw_end_=logical_length_=raw_length_;
            track_label_=compressed_->label();
        } else if(is_packed_optical_path(p)) {
            packed_=std::make_unique<PackedOptical>(p);
            raw_length_=packed_->size();
            raw_end_=logical_length_=raw_length_;
            track_label_=packed_->label();
        }
    }
public:
    explicit Source(const std::filesystem::path& p, std::optional<std::size_t> requested = std::nullopt) {
        if(is_nrg_path(p) || is_mds_path(p) || is_ccd_path(p)) {
            const auto tracks=is_nrg_path(p)?nrg_data_tracks(p):
                (is_mds_path(p)?mds_data_tracks(p):ccd_data_tracks(p));
            // Prefer a later playable data track, never probe across track
            // boundaries or metadata appended after the disc content.
            for(auto it=tracks.rbegin();it!=tracks.rend();++it) {
                const auto index=static_cast<std::size_t>(std::distance(tracks.begin(),it.base())-1);
                if(requested && *requested!=index)continue;
                open_image(it->image);
                require(it->begin<=it->end && it->end<=raw_length_,
                        "descriptor track range exceeds image");
                raw_base_=it->begin;raw_end_=it->end;
                scrambled_=it->scrambled;
                require(!scrambled_ || it->physical_sector==2352,
                        "scrambled CloneCD requires 2352-byte sectors");
                choose(it->physical_sector,it->data_offset,0,it->mode);
                if(candidate(it->physical_sector,it->data_offset,0,it->mode) ||
                   candidate_udf(it->physical_sector,it->data_offset,0,it->mode)) {
                    track_label_=it->container;
                    session_lba_=it->session_lba;
                    return;
                }
            }
            throw std::runtime_error("no recognized ISO9660/UDF data track in image descriptor");
        }
        if(is_cue_path(p)) {
            // The CUE file specifies the physical layout and track boundaries.
            // Prefer the latest track containing a recognizable file system.
            auto tracks=cue_data_tracks(p);
            for(auto it=tracks.rbegin();it!=tracks.rend();++it) {
                const auto index=static_cast<std::size_t>(std::distance(tracks.begin(),it.base())-1);
                if(requested && *requested!=index)continue;
                open_image(it->image);
                if(it->start_frame >= raw_length_/it->physical_sector)continue;
                choose(it->physical_sector,it->data_offset,it->start_frame,it->mode);
                if(it->end_frame) {
                    auto count=std::uint64_t(*it->end_frame-it->start_frame);
                    logical_length_=std::min(logical_length_,count*iso_sector);
                }
                if(candidate(it->physical_sector,it->data_offset,it->start_frame,it->mode) ||
                   candidate_udf(it->physical_sector,it->data_offset,it->start_frame,it->mode)) {
                    cue_track_number_=it->number;
                    return;
                }
            }
            throw std::runtime_error("no ISO9660/UDF data track found in CUE");
        }
        require(!requested || *requested==0,"track index is unsupported for bare image");
        open_image(p);
        if(compressed_ || packed_) {
            require(candidate(2048,0,0,0) || candidate_udf(2048,0,0,0),
                    "packed optical image does not contain recognized ISO or UDF filesystem");
            return;
        }
        // Only select a raw layout after seeing a valid ISO PVD and raw sync/mode.
        // A no-header 2048 image is left unchanged so that UDF anchors still work.
        if(candidate(2048,0,0,0))return;
        for(const auto leading: {0u,150u}) {
            for(const auto layout: {
                    std::array<std::uint32_t,3>{2352,16,1},
                    std::array<std::uint32_t,3>{2448,16,1},
                    std::array<std::uint32_t,3>{2352,24,2},
                    std::array<std::uint32_t,3>{2448,24,2},
                    std::array<std::uint32_t,3>{2336,8,2},
                    std::array<std::uint32_t,3>{2048,0,0}}) {
                if(leading==0 && layout[0]==2048)continue;
                if(candidate(layout[0],layout[1],leading,static_cast<std::uint8_t>(layout[2])) ||
                   candidate_udf(layout[0],layout[1],leading,static_cast<std::uint8_t>(layout[2]))) {
                    choose(layout[0],layout[1],leading,static_cast<std::uint8_t>(layout[2]));
                    return;
                }
            }
        }
    }
    [[nodiscard]] std::uint64_t size() const {return logical_length_;}
    [[nodiscard]] bool raw_mode() const {return physical_sector_!=2048;}
    [[nodiscard]] std::uint32_t session_leadin() const {return session_lba_.value_or(leading_sectors_);}
    [[nodiscard]] std::string layout() const {
        if(!track_label_.empty())return track_label_+" / "+std::to_string(physical_sector_)+"-byte sectors";
        if(cue_track_number_)
            return "CUE track " + std::to_string(*cue_track_number_) + " / " +
                std::to_string(physical_sector_) + "-byte sectors";
        if(physical_sector_==2048)return leading_sectors_?"2048-sector, 150-sector prefix":"";
        return std::to_string(physical_sector_)+"-sector Mode "+std::to_string(mode_)+
               (leading_sectors_?", 150-sector prefix":"");
    }
    Bytes read(std::uint64_t p, std::size_t n) {
        require(n<=max_directory && p<=logical_length_ && n<=logical_length_-p,
                "extent outside image");
        if(physical_sector_==2048)
            return raw_read(raw_base_+std::uint64_t(leading_sectors_)*2048+p,n);
        Bytes b(n);
        std::size_t copied=0;
        while(copied<n) {
            const auto logical=p+copied;
            const auto sector=logical/2048;
            const auto inside=static_cast<std::size_t>(logical%2048);
            const auto count=std::min<std::size_t>(n-copied,2048-inside);
            const auto physical=raw_base_+(std::uint64_t(leading_sectors_)+sector)*physical_sector_;
            if(physical_sector_==2352 || physical_sector_==2448) {
                auto h=raw_read(physical,mode_==1?16:24);
                require(h[0]==0 && h[11]==0 && h[15]==mode_,"raw sector mode mismatch");
                for(int i=1;i<=10;i++)require(h[i]==0xff,"raw CD sector sync invalid");
                if(mode_==2)require(!(h[18]&0x20) &&
                    std::equal(h.begin()+16,h.begin()+20,h.begin()+20),
                    "unsupported Mode 2 Form 2 sector");
            }
            auto payload=raw_read(physical+data_offset_+inside,count);
            std::copy(payload.begin(),payload.end(),b.begin()+static_cast<std::ptrdiff_t>(copied));
            copied+=count;
        }
        return b;
    }
};
void append(std::vector<Item>& out,Item entry) {
    require(out.size()<max_entries,"too many disc entries");
    out.push_back(std::move(entry));
}
struct IsoRecord {
    std::uint32_t lba{},size{};
    std::uint8_t flags{},unit{},gap{},ext_attr_blocks{};
    std::uint32_t dos_time{};
    std::string name;
};
std::string iso_name(const Bytes& data,std::size_t p,std::size_t n,bool joliet,bool directory) {
    bounds(data,p,n);
    std::string name;
    if(joliet) {
        require(n%2==0,"invalid Joliet name length");
        for(std::size_t i=0;i<n;i+=2) {
            auto cp=be16(data,p+i);
            if(cp>=0xd800 && cp<=0xdbff) {
                require(i+4<=n,"truncated surrogate pair");
                auto low=be16(data,p+i+2);
                require(low>=0xdc00 && low<=0xdfff,"invalid surrogate pair");
                name+=utf8(0x10000u + ((cp-0xd800u)<<10) + low-0xdc00u);i+=2;
            } else name+=utf8(cp);
        }
    } else name.assign(reinterpret_cast<const char*>(data.data()+p),n);
    // Strip ISO9660 version suffix, not embedded semicolons without a numeric version.
    auto split=name.rfind(';');
    if(!directory && split!=std::string::npos && split+1<name.size() &&
       std::all_of(name.begin()+static_cast<std::ptrdiff_t>(split+1),name.end(),[](unsigned char c){return c>='0'&&c<='9';}))
        name.resize(split);
    while(!name.empty()&&name.back()=='.') name.pop_back();
    safe_name(name);return name;
}
IsoRecord record(const Bytes& b,std::size_t p,bool joliet) {
    bounds(b,p,34);
    std::uint8_t len=b[p];require(len>=34 && len<=b.size()-p,"invalid ISO directory record");
    auto n=b[p+32];require(33u+n<=len,"invalid ISO filename field");
    IsoRecord r;r.lba=u32(b,p+2);r.size=u32(b,p+10);
    // ECMA-119 stores the location and length in both endian orders. Treat
    // disagreement as malformed media instead of trusting whichever half a
    // damaged image happened to corrupt.
    auto big32=[&](std::size_t at)->std::uint32_t {
        return (std::uint32_t(b[at])<<24)|(std::uint32_t(b[at+1])<<16)|
               (std::uint32_t(b[at+2])<<8)|std::uint32_t(b[at+3]);
    };
    require(r.lba==big32(p+6) && r.size==big32(p+14),
            "mismatched ISO directory extent/length byte orders");
    require(u16(b,p+28)==be16(b,p+30) && u16(b,p+28)>0,
            "mismatched ISO file interleave sequence number");
    r.ext_attr_blocks=b[p+1];
    r.dos_time=iso_dos_timestamp({b.data()+p+18,7});
    r.flags=b[p+25];r.unit=b[p+26];r.gap=b[p+27];
    require(!r.gap || r.unit,"interleaved ISO file requires nonzero file unit size");
    require(!(r.flags&2) || !r.gap,"interleaved ISO directory unsupported");
    if(n==1&&(b[p+33]==0||b[p+33]==1)) r.name=".";
    else r.name=iso_name(b,p+33,n,joliet,(r.flags&2)!=0);
    return r;
}
std::vector<Item> parse_iso(Source& src,std::string& format) {
    Bytes primary,alternative,boot_record;
    for(std::uint64_t i=16;i<16+128;i++) {
        require(i<=src.size()/iso_sector && src.size()/iso_sector-i>=1,"ISO volume descriptors truncated");
        auto b=src.read(i*iso_sector,iso_sector);
        require(std::string_view(reinterpret_cast<const char*>(b.data()+1),5)=="CD001" && b[6]==1,"not ISO 9660");
        if(b[0]==1 && primary.empty()) primary=b;
        if(b[0]==0 && boot_record.empty() &&
           std::string_view(reinterpret_cast<const char*>(b.data()+7),23)=="EL TORITO SPECIFICATION")
            boot_record=b;
        if(b[0]==2 && b[88]=='%' && b[89]=='/' && (b[90]=='@'||b[90]=='C'||b[90]=='E'))alternative=b;
        if(b[0]==255)break;
    }
    // Track images sometimes record directory LBAs as absolute disc sectors,
    // while the descriptor set appears 16 sectors into a prefixed session.
    // Detect the LBA convention using the ISO root '.' record rather than
    // guessing based on file extension or the numerical LBA itself.
    bool absolute_lba=false;
    const auto session=src.session_leadin();
    if(session && !primary.empty()) {
        const auto root=record(primary,156,false);
        auto root_ok=[&](std::uint32_t pos)->bool {
            try {
                const auto data=src.read((std::uint64_t(pos)+root.ext_attr_blocks)*iso_sector,iso_sector);
                if(data[0]<34 || data[0]>data.size())return false;
                const auto dot=record(data,0,false);
                return dot.name=="." && (dot.flags&2) && dot.lba==root.lba;
            } catch(const std::exception&){return false;}
        };
        if(!root_ok(root.lba) && root.lba>=session && root_ok(root.lba-session))
            absolute_lba=true;
    }
    auto adjusted_lba=[&](std::uint32_t lba,std::uint32_t length)->std::uint32_t {
        if(!absolute_lba || length==0)return lba;
        require(lba>=session,"absolute ISO extent predates the current session");
        return lba-session;
    };
    auto read_record=[&](const Bytes& data,std::size_t pos,bool use_joliet)->IsoRecord {
        auto r=record(data,pos,use_joliet);
        r.lba=adjusted_lba(r.lba,r.size);
        // ECMA-119 directory record byte 1: the Extended Attribute Record
        // occupies this many logical blocks before the actual file or
        // directory data. The directory's logical block index points at the
        // start of the attribute blocks, not the first content block.
        require(r.lba<=std::numeric_limits<std::uint32_t>::max()-r.ext_attr_blocks,
                "ISO extended-attribute extent overflow");
        r.lba+=r.ext_attr_blocks;
        return r;
    };
    auto read_susp=[&](std::uint64_t pos,std::size_t length)->Bytes {
        if(absolute_lba) {
            const auto base=std::uint64_t(session)*iso_sector;
            require(pos>=base,"absolute SUSP CE points before current session");
            pos-=base;
        }
        return src.read(pos,length);
    };
    // The root '.' entry advertises Rock Ridge through SUSP SP. When both
    // Rock Ridge and Joliet are present, prefer RRIP's original POSIX names.
    std::optional<std::uint8_t> rr_skip;
    if(!primary.empty()) {
        auto pvd_root=read_record(primary,156,false);
        if((pvd_root.flags&2) && pvd_root.size>=34) {
            auto first=src.read(std::uint64_t(pvd_root.lba)*iso_sector,
                                std::min<std::uint32_t>(pvd_root.size,2048));
            if(first[0]>=34 && first[0]<=first.size()) {
                auto dot=std::span<const std::uint8_t>(first.data(),first[0]);
                auto sp=rock_ridge_skip(dot);
                if(sp && rock_ridge_entry(dot,*sp,[&](std::uint64_t offset,std::size_t n) {
                    return read_susp(offset,n);
                }).extension)rr_skip=sp;
            }
        }
    }
    bool joliet=!alternative.empty() && !rr_skip.has_value();
    require(joliet||!primary.empty(),"no ISO volume descriptor");
    const auto& volume=joliet?alternative:primary;
    auto root=read_record(volume,156,joliet);require(root.flags&2,"ISO root is not a directory");
    format=rr_skip?"Rock Ridge":(joliet?"Joliet":"ISO 9660");
    if(absolute_lba)format+=" / session absolute-LBA";
    std::vector<Item> out;
    std::set<std::pair<std::uint32_t,std::uint32_t>> active;
    auto walk=[&](auto&& self,const IsoRecord& dir,const std::string& base,unsigned depth)->void {
        require(depth<64,"ISO nesting too deep");
        require(dir.size<=max_directory,"ISO directory too large");
        auto key=std::make_pair(dir.lba,dir.size);
        require(active.insert(key).second,"cyclic ISO directories");
        auto data=src.read(std::uint64_t(dir.lba)*iso_sector,dir.size);
        std::string pending_multiextent, pending_iso_name;
        for(std::size_t p=0;p<data.size();) {
            if(data[p]==0) {p+=iso_sector-(p%iso_sector);continue;}
            auto length=data[p];
            require(length>=34 && length<=data.size()-p && length<=iso_sector-(p%iso_sector),"invalid ISO record length");
            auto r=read_record(data,p,joliet);
            const auto iso_identifier=r.name;
            if(r.name==".") {p+=length;continue;}
            std::optional<std::uint32_t> rr_mode,rr_modification;
            if(rr_skip) {
                const auto rr=rock_ridge_entry(
                    std::span<const std::uint8_t>(data.data()+p,length),*rr_skip,
                    [&](std::uint64_t offset,std::size_t size){return read_susp(offset,size);});
                // Never traverse a link on the host. A relocated child (CL)
                // needs directory-structure rewriting; fail instead of silently
                // listing the wrong files. RE entries are duplicate placeholders.
                if(rr.relocated || rr.symlink) {p+=length;continue;}
                if(rr.posix_mode) {
                    const auto kind=*rr.posix_mode & 0170000u;
                    // A POSIX link or device masquerading as an ISO data file
                    // must not be materialized as ordinary extracted bytes.
                    if(kind!=0 && kind!=0100000u && kind!=0040000u) {
                        p+=length;
                        continue;
                    }
                    require(kind==0 || (kind==0040000u)==bool(r.flags&2),
                            "RRIP PX file type contradicts ISO directory flag");
                    rr_mode=rr.posix_mode;
                }
                rr_modification=rr.modification_dos_time;
                if(rr.child_link) {
                    require((r.flags&2) && rr.child_lba.has_value(),
                            "Rock Ridge CL must reference a directory");
                    auto normalized_child=adjusted_lba(*rr.child_lba,iso_sector);
                    auto first=src.read(std::uint64_t(normalized_child)*iso_sector,iso_sector);
                    require(first[0]>=34 && first[0]<=first.size(),
                            "invalid relocated Rock Ridge directory");
                    auto relocated_dot=read_record(first,0,joliet);
                    require(relocated_dot.name=="." && (relocated_dot.flags&2) &&
                            relocated_dot.lba==normalized_child &&
                            relocated_dot.size>=34 && relocated_dot.size<=max_directory,
                            "invalid Rock Ridge CL destination");
                    r.lba=normalized_child;
                    r.size=relocated_dot.size;
                }
                if(rr.alternate_name) {r.name=*rr.alternate_name;safe_name(r.name);}
                else if(!pending_multiextent.empty() && iso_identifier==pending_iso_name)
                    r.name=pending_multiextent.substr(base.size());
            }
            p+=length;
            Item item;item.path=base+r.name;item.directory=bool(r.flags&2);item.size=item.directory?0:r.size;
            item.dos_time=rr_modification.value_or(r.dos_time);
            item.attributes=item.directory?0x10u:0x20u;
            if(r.flags&1)item.attributes|=0x02u;
            // DOS/WCX only exposes a subset of POSIX permissions. Keep the
            // original mode in RRIP parsing, map non-writable to read-only.
            if(rr_mode && ((*rr_mode & 0222u)==0))item.attributes|=0x01u;
            if(!item.directory) {
                if(!r.gap) {
                    item.extents.push_back({std::uint64_t(r.lba)*iso_sector,r.size,false});
                } else {
                    // ISO9660 7.1.5: file units (contiguous logical blocks)
                    // alternate with interleave gaps. Gaps are not file data.
                    // A file may end halfway through its final unit.
                    const auto unit_bytes=std::uint64_t(r.unit)*iso_sector;
                    auto remaining=std::uint64_t(r.size);
                    auto sector=std::uint64_t(r.lba);
                    while(remaining) {
                        const auto chunk=std::min(remaining,unit_bytes);
                        require(sector<=std::numeric_limits<std::uint64_t>::max()/iso_sector,
                                "interleaved ISO extent offset overflow");
                        item.extents.push_back({sector*iso_sector,chunk,false});
                        require(item.extents.size()<=max_entries,
                                "too many ISO interleaved file units");
                        remaining-=chunk;
                        if(remaining)sector+=std::uint64_t(r.unit)+r.gap;
                    }
                }
            }
            if(!pending_multiextent.empty()) {
                require(!item.directory && item.path==pending_multiextent,"invalid ISO multi-extent sequence");
                auto& previous=out.back();
                require(previous.size<=std::numeric_limits<std::uint64_t>::max()-item.size,"ISO file size overflow");
                previous.size+=item.size;
                previous.extents.insert(previous.extents.end(),item.extents.begin(),item.extents.end());
                require(previous.extents.size()<=max_entries,"too many ISO file extents");
            } else {
                require(item.directory || !(r.flags&0x80u) || r.size,"invalid empty ISO multi-extent");
                append(out,std::move(item));
            }
            pending_multiextent=(r.flags&0x80u)?base+r.name:std::string{};
            pending_iso_name=(r.flags&0x80u)?iso_identifier:std::string{};
            if(r.flags&2) self(self,r,base+r.name+"/",depth+1);
        }
        require(pending_multiextent.empty(),"missing final ISO multi-extent record");
        active.erase(key);
    };
    try {
        walk(walk,root,"",0);
    } catch(const std::exception&) {
        // Some mastering programs ship a valid ISO9660 primary descriptor
        // alongside a broken/incomplete Joliet tree. The base ISO tree must
        // remain accessible. Never let an incomplete Joliet listing leak out.
        if(!joliet || primary.empty())throw;
        out.clear();active.clear();
        joliet=false;
        root=read_record(primary,156,false);
        require(root.flags&2,"ISO root is not a directory");
        format="ISO 9660 (Joliet fallback)";
        if(absolute_lba)format+=" / session absolute-LBA";
        walk(walk,root,"",0);
    }
    if(!boot_record.empty()) {
        // The virtual folder is a view on boot catalog sectors and load
        // images; never overwrite an ordinary ISO folder of the same name.
        const bool collision=std::any_of(out.begin(),out.end(),[](const Item& item){
            return item.path=="boot.images" || item.path.rfind("boot.images/",0)==0;
        });
        if(!collision) {
            try {
                auto boot=el_torito_images(boot_record,src.size(),
                    [&](std::uint64_t p,std::size_t n){return src.read(p,n);},
                    absolute_lba?session:0);
                for(auto& item:boot)append(out,std::move(item));
            } catch(const std::exception&) { /* Malformed boot catalog does not hide ISO files. */ }
        }
    }
    return out;
}

void check_tag(const Bytes& b,std::size_t p,std::uint16_t expected) {
    bounds(b,p,16);require(u16(b,p)==expected,"unexpected UDF descriptor tag");
    unsigned sum=0;for(std::size_t i=0;i<16;i++)if(i!=4)sum+=b[p+i];
    require((sum&255)==b[p+4],"invalid UDF descriptor tag checksum");
    auto crc_bytes = u16(b,p+10);
    require(crc_bytes<=b.size()-(p+16),"truncated UDF CRC descriptor");
    if(crc_bytes) {
        std::uint16_t crc=0;
        for(std::size_t i=0;i<crc_bytes;i++) {
            crc ^= std::uint16_t(b[p+16+i])<<8;
            for(unsigned bit=0;bit<8;bit++)
                crc=std::uint16_t((crc&0x8000u) ? ((crc<<1)^0x1021u) : (crc<<1));
        }
        require(crc==u16(b,p+8),"invalid UDF descriptor CRC16");
    }
}
std::string udf_name(const Bytes& b,std::size_t p,std::size_t n) {
    bounds(b,p,n);require(n>1,"empty UDF filename");
    int compression=b[p++];--n;
    require(compression==8||compression==16,"unsupported OSTA CS0 encoding");
    require(compression==8||n%2==0,"invalid UDF UTF16 name");
    std::string result;
    for(std::size_t i=0;i<n;) {
        std::uint32_t cp=(compression==8)?b[p+i++]:be16(b,p+i);
        if(compression==16) {
            i+=2;
            if(cp>=0xd800 && cp<=0xdbff) {
                require(i+2<=n,"truncated UDF UTF-16 surrogate pair");
                const auto low=be16(b,p+i);
                require(low>=0xdc00 && low<=0xdfff,"invalid UDF UTF-16 surrogate pair");
                cp=0x10000u+((cp-0xd800u)<<10)+(low-0xdc00u);
                i+=2;
            }
        }
        result+=utf8(cp);
    }
    safe_name(result);return result;
}
struct UdfFe {std::uint64_t size{};std::uint8_t type{};std::uint32_t dos_time{};std::vector<Extent> extents;Bytes inline_bytes;};
class Udf {
    Source& src_;
    std::uint32_t block_{};
    std::map<std::uint16_t,std::pair<std::uint32_t,std::uint32_t>> partitions_;
    std::vector<int> mapping_; // >=0 physical, -2 metadata, -3 virtual, -4 sparable
    struct MetadataMap {
        std::uint16_t partition{};
        std::uint32_t icb{}, mirror{};
        std::uint64_t size{};
        std::vector<Extent> extents;
    };
    struct VirtualMap {
        std::uint16_t partition{};
        std::vector<std::uint32_t> entries;
    };
    struct SparableMap {
        std::uint16_t partition{},packet{};
        std::uint32_t table_size{};
        std::vector<std::uint32_t> locations;
        std::map<std::uint32_t,std::uint32_t> redirects;
    };
    std::map<std::uint16_t,MetadataMap> metadata_;
    std::map<std::uint16_t,VirtualMap> virtual_;
    std::map<std::uint16_t,SparableMap> sparable_;
    std::vector<Item> entries_;
    std::set<std::uint64_t> active_;
    [[nodiscard]] std::uint64_t address(std::uint32_t lba,std::uint16_t map,
                                         std::uint64_t extent_length=0) const {
        require(map<mapping_.size(),"unsupported UDF partition map index");
        const auto kind=mapping_[map];
        if(kind==-2) {
            auto it=metadata_.find(map);
            require(it!=metadata_.end() && !it->second.extents.empty(),
                    "uninitialized UDF metadata partition");
            const auto at=std::uint64_t(lba)*block_;
            require(at<it->second.size && extent_length<=it->second.size-at,
                    "UDF metadata partition boundary");
            // Resolve the logical position using cumulative extent lengths.
            std::uint64_t pos=0;
            for(const auto& ex:it->second.extents) {
                if(at>=pos && at-pos<ex.length) {
                    const auto delta=at-pos;
                    require(!ex.sparse && extent_length<=ex.length-delta,
                            "UDF metadata mapping crosses an extent");
                    return ex.offset+delta;
                }
                require(pos<=std::numeric_limits<std::uint64_t>::max()-ex.length,
                        "UDF metadata allocation overflow");
                pos+=ex.length;
            }
            throw std::runtime_error("UDF metadata block not mapped");
        }
        std::uint32_t physical_lba=lba;
        std::uint16_t physical_partition{};
        if(kind==-4) {
            auto st=sparable_.find(map);
            require(st!=sparable_.end() && st->second.packet>0,"missing sparable map");
            const auto packet=st->second.packet;
            const auto packet_start=(lba/packet)*packet;
            require(extent_length<=std::uint64_t(packet-(lba%packet))*block_,
                    "Sparable extent crosses packet boundary");
            auto partition=partitions_.find(st->second.partition);
            require(partition!=partitions_.end() && lba<partition->second.second,
                    "Sparable extent outside partition");
            require(extent_length<=std::uint64_t(partition->second.second-lba)*block_,
                    "Sparable extent crosses partition boundary");
            auto it=st->second.redirects.find(packet_start);
            const auto sector=it==st->second.redirects.end()?
                              std::uint64_t(partition->second.first)+lba:
                              std::uint64_t(it->second)+(lba-packet_start);
            require(sector<src_.size()/block_ &&
                    extent_length<=(src_.size()-sector*block_),
                    "Sparable physical address outside image");
            return sector*block_;
        }
        if(kind==-3) {
            auto it=virtual_.find(map);
            require(it!=virtual_.end() && lba<it->second.entries.size(),
                    "unmapped UDF VAT virtual address");
            physical_lba=it->second.entries[lba];
            require(physical_lba!=0xffffffffu && extent_length<=block_,
                    "invalid UDF VAT block reference");
            physical_partition=it->second.partition;
        } else {
            require(kind>=0,"unsupported UDF partition map");
            physical_partition=static_cast<std::uint16_t>(kind);
        }
        auto it=partitions_.find(physical_partition);
        require(it!=partitions_.end() && physical_lba<it->second.second,
                "UDF extent outside partition");
        require(extent_length <= std::uint64_t(it->second.second-physical_lba)*block_,
                "UDF extent crosses partition boundary");
        return (std::uint64_t(it->second.first)+physical_lba)*block_;
    }
    void mapped_data(std::vector<Extent>& result, std::uint32_t lba,
                     std::uint16_t map, std::uint64_t length, bool sparse) const {
        if(!length)return;
        require(map<mapping_.size(),"invalid UDF allocation map index");
        if(sparse) { result.push_back({0,length,true});return; }
        if(mapping_[map]==-2) {
            const auto it=metadata_.find(map);
            require(it!=metadata_.end(),"missing metadata map");
            const auto at=std::uint64_t(lba)*block_;
            require(at<it->second.size && length<=it->second.size-at,
                    "metadata file extent beyond partition");
            std::uint64_t logical=0;
            std::uint64_t remain=length;
            std::uint64_t current=at;
            for(const auto& run:it->second.extents) {
                require(logical<=std::numeric_limits<std::uint64_t>::max()-run.length,
                        "metadata run overflow");
                const auto next=logical+run.length;
                if(current>=next){logical=next;continue;}
                require(current>=logical && !run.sparse,"unrecorded metadata extent");
                const auto inside=current-logical;
                const auto n=std::min<std::uint64_t>(remain,run.length-inside);
                require(result.size()<max_entries,"too many metadata file extents");
                result.push_back({run.offset+inside,n,false});
                current+=n;remain-=n;
                if(!remain)break;
                logical=next;
            }
            require(remain==0,"incomplete metadata partition mapping");
        } else if(mapping_[map]==-4) {
            auto it=sparable_.find(map);
            require(it!=sparable_.end() && it->second.packet,"invalid sparable map");
            std::uint64_t consumed=0;
            while(consumed<length) {
                const auto block=std::uint64_t(lba)+consumed/block_;
                require(block<=std::numeric_limits<std::uint32_t>::max(),"sparable LBA overflow");
                const auto packets_left=it->second.packet-(block%it->second.packet);
                const auto n=std::min<std::uint64_t>(length-consumed,
                                                    packets_left*block_);
                require(result.size()<max_entries,"too many sparable file extents");
                result.push_back({address(static_cast<std::uint32_t>(block),map,n),n,false});
                consumed+=n;
            }
        } else if(mapping_[map]==-3) {
            // VAT remaps logical blocks independently. Split the byte run at
            // virtual-block boundaries rather than assuming contiguity.
            std::uint64_t consumed=0;
            while(consumed<length) {
                require(result.size()<max_entries,"too many virtual file extents");
                const auto block=lba+consumed/block_;
                require(block<=std::numeric_limits<std::uint32_t>::max(),"VAT LBA overflow");
                const auto n=std::min<std::uint64_t>(block_,length-consumed);
                result.push_back({address(static_cast<std::uint32_t>(block),map,n),n,false});
                consumed+=n;
            }
        } else result.push_back({address(lba,map,length),length,false});
    }
    Bytes data_bytes(const UdfFe& file, std::size_t limit) {
        require(file.size<=limit,"UDF embedded metadata too large");
        if(!file.inline_bytes.empty())return file.inline_bytes;
        Bytes data;
        data.reserve(static_cast<std::size_t>(file.size));
        for(const auto& e:file.extents){
            require(e.length<=limit-data.size(),"UDF extents too large");
            if(e.sparse)data.insert(data.end(),static_cast<std::size_t>(e.length),0);
            else {
                auto b=src_.read(e.offset,static_cast<std::size_t>(e.length));
                data.insert(data.end(),b.begin(),b.end());
            }
        }
        require(data.size()==file.size,"incomplete UDF metadata file");
        return data;
    }
    std::uint16_t physical_map(std::uint16_t partition) const {
        for(std::size_t i=0;i<mapping_.size();++i)
            if(mapping_[i]==partition)return static_cast<std::uint16_t>(i);
        for(const auto& [id,map]:sparable_)
            if(map.partition==partition)return id;
        throw std::runtime_error("missing backing physical or sparable partition map");
    }
    void init_sparable() {
        for(auto& [map,sp]:sparable_) {
            require(sp.packet>0 && sp.packet<=256 && sp.table_size>=56 &&
                    sp.table_size<=max_directory && !sp.locations.empty(),
                    "invalid sparable partition map");
            bool valid=false;
            std::uint32_t selected_seq=0;
            for(const auto table_location:sp.locations) {
                if(table_location==0xffffffffu)continue;
                try {
                    const auto b=src_.read(std::uint64_t(table_location)*block_,sp.table_size);
                    check_tag(b,0,0);
                    require(std::string_view(reinterpret_cast<const char*>(b.data()+17),18)==
                            "*UDF Sparing Table","invalid sparing table identifier");
                    const auto records=u16(b,48);
                    require(records<=static_cast<std::size_t>(b.size()-56)/8,
                            "invalid sparing table records count");
                    const auto sequence=u32(b,52);
                    std::map<std::uint32_t,std::uint32_t> redirects;
                    for(std::uint16_t i=0;i<records;i++) {
                        const auto old=u32(b,56+8u*i),replacement=u32(b,60+8u*i);
                        if(old>=0xfffffff0u)continue;
                        require(old%sp.packet==0 &&
                                std::uint64_t(replacement)+sp.packet<=src_.size()/block_,
                                "invalid sparing entry packet");
                        require(redirects.emplace(old,replacement).second,
                                "duplicate sparable packet replacement");
                    }
                    if(!valid || sequence>selected_seq){
                        sp.redirects=std::move(redirects);
                        selected_seq=sequence;valid=true;
                    }
                } catch(const std::exception&) { /* Use another valid table copy. */ }
            }
            require(valid,"no valid UDF sparing table available");
        }
    }
    void init_metadata() {
        for(auto& [map,md]:metadata_) {
            const auto physical=physical_map(md.partition);
            bool loaded=false;
            for(auto location:{md.icb,md.mirror}) {
                if(location==0xffffffffu)continue;
                try {
                    auto file=fe(location,physical);
                    require(file.type==250||file.type==251,
                            "metadata map must reference Metadata File Entry");
                    require(file.size>=block_ && file.size%block_==0,
                            "invalid metadata partition length");
                    require(file.inline_bytes.empty(),"embedded metadata partition unsupported");
                    for(const auto& run:file.extents)
                        require(!run.sparse && run.length%block_==0,
                                "sparse or unaligned metadata partition");
                    md.size=file.size;
                    md.extents=std::move(file.extents);
                    loaded=true;break;
                } catch(const std::exception&) { /* Try metadata mirror if available. */ }
            }
            require(loaded,"unable to load UDF metadata partition");
        }
    }
    void init_virtual() {
        for(auto& [map,vt]:virtual_) {
            const auto physical=physical_map(vt.partition);
            const auto partition=partitions_.find(vt.partition);
            require(partition!=partitions_.end(),"missing backing partition for UDF VAT");
            // The VAT ICB is the last recorded data sector of a transaction.
            // Search near the end of its containing partition; do not use the
            // backup anchor as a replacement for a validated FE/EFE descriptor.
            const auto start=partition->second.first;
            const auto count=partition->second.second;
            const auto available=src_.size()/block_;
            require(start<available,"VAT partition outside disc image");
            const auto max_lba=std::min<std::uint64_t>(count,available-start);
            require(max_lba>0,"empty VAT partition");
            bool found=false;
            for(std::uint64_t next=max_lba, probes=0;next>0 && probes<8192;--next,++probes) {
                const auto at=next-1;
                auto descriptor=src_.read((std::uint64_t(start)+at)*block_,block_);
                if(u16(descriptor,0)!=261 && u16(descriptor,0)!=266)continue;
                try {
                    check_tag(descriptor,0,u16(descriptor,0));
                    if(descriptor[27]!=248)continue;
                    const auto file=fe(static_cast<std::uint32_t>(at),physical);
                    const auto vat=data_bytes(file,16*1024*1024);
                    std::size_t header=0,entries_end=vat.size();
                    if(vat.size()>=152 && u16(vat,0)>=152 && u16(vat,0)<=vat.size() &&
                       u16(vat,0)==152u+u16(vat,2)) header=u16(vat,0);
                    else {
                        // UDF 1.50 layout has a 36-byte trailer, not a header.
                        require(vat.size()>=40 &&
                                std::string_view(reinterpret_cast<const char*>(vat.data()+vat.size()-35),22)==
                                "*UDF Virtual Alloc Tbl","invalid VAT header or trailer");
                        entries_end=vat.size()-36;
                    }
                    require((entries_end-header)%4==0 && entries_end>header &&
                            (entries_end-header)/4<=8*1024*1024,
                            "invalid VAT length");
                    vt.entries.clear();
                    for(auto p=header;p<entries_end;p+=4)vt.entries.push_back(u32(vat,p));
                    found=true;break;
                } catch(const std::exception&) { /* Ignore damaged historic VAT ICB. */ }
            }
            require(found,"UDF virtual partition requires a valid VAT ICB");
        }
    }
    Bytes read_extent(std::uint64_t p,std::size_t n){return src_.read(p,n);}
    UdfFe fe(std::uint32_t lba,std::uint16_t map) {
        auto b=src_.read(address(lba,map),block_);
        require(b.size()>=216,"truncated UDF file entry");
        auto id=u16(b,0);require(id==261||id==266,"not UDF FE/EFE");
        check_tag(b,0,id);
        std::size_t base=id==261?176:216;
        std::size_t ea=id==261?168:208,adl=id==261?172:212;
        auto extra=u32(b,ea), ads=u32(b,adl);
        require(extra<=b.size()-base && ads<=b.size()-base-extra,"UDF allocation descriptors outside FE");
        UdfFe result;result.type=b[27];result.size=u64(b,56);
        result.dos_time=udf_dos_timestamp({b.data()+(id==261?84u:92u),12});
        auto mode=u16(b,34)&7;
        std::size_t begin=base+extra;
        if(mode==3) {
            require(result.size<=ads,"truncated embedded UDF data");
            result.inline_bytes.insert(result.inline_bytes.end(),b.begin()+static_cast<std::ptrdiff_t>(begin),b.begin()+static_cast<std::ptrdiff_t>(begin+static_cast<std::size_t>(result.size)));
            return result;
        }
        require(mode==0||mode==1,"unsupported UDF allocation descriptor type");
        const std::size_t step=mode==0?8:16;
        require(ads%step==0,"truncated UDF allocation descriptors");
        std::uint64_t remain=result.size;
        std::set<std::pair<std::uint16_t,std::uint32_t>> followed;
        Bytes continuation;
        const Bytes* current=&b;
        std::size_t pos=begin, end=begin+ads;
        unsigned redirects=0;
        while(remain && pos<end) {
            const auto raw=u32(*current,pos);
            const auto length=std::uint64_t(raw&0x3fffffffu);
            const auto kind=raw>>30;
            auto current_map=mode==0?map:u16(*current,pos+8);
            auto current_lba=u32(*current,pos+4);
            pos+=step;
            if(kind==3) {
                // ECMA-167 Allocation Extent Descriptor (tag 258). Continuation
                // descriptors do not themselves account for file contents.
                require(pos==end,"UDF allocation continuation must be last descriptor");
                require(++redirects<=64 && length>=24 && length<=max_directory,
                        "invalid UDF allocation continuation size");
                require(followed.insert({current_map,current_lba}).second,
                        "cyclic UDF allocation continuation");
                continuation=src_.read(address(current_lba,current_map,length),
                                        static_cast<std::size_t>(length));
                check_tag(continuation,0,258);
                const auto continuation_ads=u32(continuation,20);
                require(continuation_ads>0 && continuation_ads%step==0 &&
                        continuation_ads<=continuation.size()-24,
                        "invalid UDF allocation extent descriptor length");
                current=&continuation;
                pos=24;
                end=24+continuation_ads;
                continue;
            }
            const auto chunk=std::min(length,remain);
            if(!chunk)continue;
            const bool sparse=kind!=0;
            mapped_data(result.extents,current_lba,current_map,chunk,sparse);
            remain-=chunk;
        }
        require(remain==0,"UDF file allocation shorter than information length");
        return result;
    }
    Bytes directory_bytes(const UdfFe& directory) {
        require(directory.size<=max_directory,"UDF directory too large");
        Bytes b;
        b.reserve(static_cast<std::size_t>(directory.size));
        if(!directory.inline_bytes.empty())return directory.inline_bytes;
        for(const auto& extent:directory.extents) {
            auto n=static_cast<std::size_t>(extent.length);
            if(extent.sparse)b.insert(b.end(),n,0);
            else {
                auto chunk=read_extent(extent.offset,n);
                b.insert(b.end(),chunk.begin(),chunk.end());
            }
        }
        require(b.size()==directory.size,"UDF directory size mismatch");return b;
    }
    void walk(std::uint32_t lba,std::uint16_t map,const std::string& prefix,unsigned depth) {
        require(depth<64,"UDF nesting too deep");
        auto key=(std::uint64_t(map)<<32)|lba;
        require(active_.insert(key).second,"cyclic UDF directory");
        auto directory=fe(lba,map);require(directory.type==4,"UDF ICB is not a directory");
        auto bytes=directory_bytes(directory);
        for(std::size_t p=0;p<bytes.size();) {
            const auto remaining=bytes.size()-p;
            if(remaining<38) {
                require(std::all_of(
                    bytes.begin()+static_cast<std::ptrdiff_t>(p),
                    bytes.end(),[](std::uint8_t v){return v==0;}),
                    "trailing incomplete UDF directory identifier");
                break;
            }
            if(std::all_of(bytes.begin()+static_cast<std::ptrdiff_t>(p),
                           bytes.begin()+static_cast<std::ptrdiff_t>(p+16),
                           [](std::uint8_t v){return v==0;})) {
                const auto padding=std::min<std::size_t>(
                    block_-p%block_,remaining);
                require(std::all_of(
                    bytes.begin()+static_cast<std::ptrdiff_t>(p),
                    bytes.begin()+static_cast<std::ptrdiff_t>(p+padding),
                    [](std::uint8_t v){return v==0;}),
                    "invalid UDF sector padding");
                p+=padding;
                continue;
            }
            check_tag(bytes,p,257);
            auto n=bytes[p+19];auto impl=u16(bytes,p+36);
            require(impl<=bytes.size()-p-38 && n<=bytes.size()-p-38-impl,"invalid UDF FID length");
            auto record_length=(std::size_t(38)+impl+n+3u)&~std::size_t(3);
            require(record_length<=remaining && record_length>0,
                    "UDF FID exceeds logical directory length");
            if(!(bytes[p+18]&0x0cu) && n) {
                auto item_name=udf_name(bytes,p+38+impl,n);
                auto child_lba=u32(bytes,p+24);auto child_map=u16(bytes,p+28);
                auto child=fe(child_lba,child_map);
                // ECMA-167 ICB file types 4 and 5 are directories and
                // ordinary files. Never present symbolic links (type 12),
                // devices, FIFOs or other special ICBs as ordinary bytes
                // to the WCX extraction API. We do not materialize links.
                if(child.type==4 || child.type==5) {
                    Item item;item.path=prefix+item_name;
                    item.directory=child.type==4;
                    item.attributes=item.directory?0x10u:0x20u;
                    item.dos_time=child.dos_time;
                    item.size=item.directory?0:child.size;
                    item.extents=std::move(child.extents);
                    item.inline_data=std::move(child.inline_bytes);
                    append(entries_,std::move(item));
                    if(child.type==4)
                        walk(child_lba,child_map,prefix+item_name+"/",depth+1);
                }
            }
            p+=record_length;
        }
        active_.erase(key);
    }
public:
    explicit Udf(Source& src) : src_(src) {}
    bool parse() {
        // Read unusual large-block UDF images, and probe sector 512 as
        // a last-resort, validated AVDP when ordinary anchors are absent.
        constexpr std::array<std::uint32_t,7> candidates={
            2048,512,1024,4096,8192,16384,32768};
        for(auto sector:candidates) {
            auto count=src_.size()/sector;
            std::array<std::uint64_t,4> anchors={
                256,count>=256?count-256:0,count?count-1:0,512};
            for(auto anchor:anchors) {
                if(anchor>=count)continue;
                try {
                    auto a=src_.read(anchor*sector,sector);
                    check_tag(a,0,2);
                    if(anchor==512)
                        require(u32(a,12)==512,
                                "UDF sector-512 anchor location mismatch");
                    for(unsigned copy=0;copy<2;copy++) {
                        try {
                    auto length=u32(a,16+8*copy),start=u32(a,20+8*copy);
                    require(length>0 && length<=16*1024*1024,"invalid UDF volume descriptor span");
                    partitions_.clear();mapping_.clear();metadata_.clear();virtual_.clear();sparable_.clear();
                    entries_.clear();active_.clear();
                    Bytes lvd;
                    for(std::uint64_t i=0;i<(length+sector-1)/sector && i<8192;i++) {
                        auto b=src_.read((std::uint64_t(start)+i)*sector,sector);
                        std::uint16_t id=u16(b,0);
                        try {check_tag(b,0,id);} catch(const std::exception&){continue;}
                        if(id==5) partitions_[u16(b,22)]={u32(b,188),u32(b,192)};
                        else if(id==6) lvd=std::move(b);
                        if(id==8)break;
                    }
                    require(!lvd.empty()&&!partitions_.empty(),"missing UDF volume descriptor");
                    block_=u32(lvd,212);
                    require(block_>=512 && block_<=32768 &&
                            (block_&(block_-1))==0,
                            "unsupported UDF logical block size");
                    auto map_bytes=u32(lvd,264),map_count=u32(lvd,268);
                    require(map_count>0&&map_count<=64 && lvd.size()>=440 && map_bytes<=lvd.size()-440,"UDF partition maps too large");
                    std::size_t p=440;
                    for(std::uint32_t i=0;i<map_count;i++) {
                        require(p+2<=440+map_bytes,"short UDF partition map");
                        auto type=lvd[p],n=lvd[p+1];
                        require(n>=2 && n<=440+map_bytes-p,"truncated UDF partition map");
                        if(type==1 && n==6)mapping_.push_back(int(u16(lvd,p+4)));
                        else if(type==2 && n==64) {
                            require(p+64<=lvd.size(),"short UDF Type-2 map");
                            std::string_view identifier(reinterpret_cast<const char*>(lvd.data()+p+5),23);
                            if(identifier=="*UDF Metadata Partition") {
                                auto map_index=static_cast<std::uint16_t>(mapping_.size());
                                mapping_.push_back(-2);
                                metadata_[map_index]={u16(lvd,p+38),u32(lvd,p+40),u32(lvd,p+44),0,{}};
                            } else if(identifier.substr(0,22)=="*UDF Virtual Partition") {
                                auto map_index=static_cast<std::uint16_t>(mapping_.size());
                                mapping_.push_back(-3);
                                virtual_[map_index]={u16(lvd,p+38),{}};
                            } else if(identifier=="*UDF Sparable Partition") {
                                auto map_index=static_cast<std::uint16_t>(mapping_.size());
                                mapping_.push_back(-4);
                                auto& sp=sparable_[map_index];
                                sp.partition=u16(lvd,p+38);
                                sp.packet=u16(lvd,p+40);
                                sp.table_size=u32(lvd,p+44);
                                const auto sparing_table_count=lvd[p+42];
                                require(sparing_table_count>=1&&sparing_table_count<=4,
                                        "invalid number of sparing tables");
                                for(unsigned t=0;t<sparing_table_count;t++)
                                    sp.locations.push_back(u32(lvd,p+48+t*4));
                            } else mapping_.push_back(-1);
                        } else mapping_.push_back(-1);
                        p+=n;
                    }
                    init_sparable();
                    init_metadata();
                    init_virtual();
                    auto fsd_len=u32(lvd,248)&0x3fffffffu;
                    require(fsd_len>=416 && fsd_len<=max_directory,"invalid UDF file-set descriptor length");
                    auto fsd=src_.read(address(u32(lvd,252),u16(lvd,256)),static_cast<std::size_t>(fsd_len));
                    check_tag(fsd,0,256);
                    walk(u32(fsd,404),u16(fsd,408),"",0);
                    return true;
                        } catch(const std::exception&) { /* Try reserve VDS or backup anchor. */ }
                    }
                } catch(const std::exception&) {/* Try next backup anchor, then ISO fallback. */}
            }
        }
        return false;
    }
    std::vector<Item> take() {return std::move(entries_);}
};
}

Disc::Disc(std::filesystem::path image): image_(std::move(image)) {
    // For descriptor-driven disc images, each independently recognizable
    // filesystem track is made available. Do not scan raw media heuristically:
    // only CUE/MDS/NRG tables provide trusted track start/end boundaries.
    const bool cue=is_cue_path(image_);
    const bool nrg=is_nrg_path(image_);
    const bool mds=is_mds_path(image_);
    const bool ccd=is_ccd_path(image_);
    if(is_mdx_path(image_)) {
        (void)probe_mds_descriptor(image_);
        throw std::runtime_error(
            "MDX recognized; encrypted/packed MDSv2 metadata and "
            "data-track extraction are not implemented in ISO-WCX");
    }
    // Virtual HDD formats are served directly by the in-process FAT backend.
    // Ambiguous .img files are handled by the optical reader first.
    auto extension = image_.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    auto open_virtual = [&] {
        auto archive = std::make_shared<VirtualArchive>(image_);
        entries_ = archive->items();
        format_ = archive->format();
        virtual_archive_ = std::move(archive);
    };
    if(extension == L".wim" || extension == L".esd" || extension == L".swm") {
#ifdef _WIN32
        // WIMGAPI is a Windows system component, not an external helper.
        auto archive = std::make_shared<WimArchive>(image_);
        entries_ = archive->items();
        format_ = archive->format();
        wim_archive_ = std::move(archive);
        return;
#else
        throw std::runtime_error("WIM/ESD browsing requires Windows WIMGAPI");
#endif
    }
    if(extension == L".vhd" || extension == L".vhdx") {
        open_virtual();
        return;
    }
    if(extension == L".img" || extension == L".raw") {
        // A .raw file can be a whole-disk UDF volume (even with a fake,
        // zero-start MBR). First try real MBR/GPT filesystems, then require
        // independently valid UDF/ISO descriptors before using optical mode.
        try {open_virtual(); return;}
        catch(const std::exception&) { /* Try optical/UDF validation. */ }
    }
    if(cue || nrg || mds || ccd) {
        const std::size_t count=cue?cue_data_tracks(image_).size():
            (nrg?nrg_data_tracks(image_).size():
             (mds?mds_data_tracks(image_).size():ccd_data_tracks(image_).size()));
        require(count<=512,"too many descriptor tracks");
        struct View {std::uint32_t index;std::string format;std::vector<Item> entries;};
        std::vector<View> views;
        for(std::size_t i=0;i<count;i++) {
            try {
                Source src(image_,i);
                std::string detected;
                std::vector<Item> list;
                Udf udf(src);
                if(udf.parse()){list=udf.take();detected="UDF";}
                else list=parse_iso(src,detected);
                if(list.empty())continue;
                detected+=" / "+src.layout();
                views.push_back({static_cast<std::uint32_t>(i),std::move(detected),std::move(list)});
            } catch(const std::exception&) { /* Track has no supported filesystem. */ }
        }
        require(!views.empty(),"no supported filesystem track in image descriptor");
        if(views.size()>1) {
            format_="Multisession / ";
            format_+=cue?"CUE tracks":(nrg?"NRG tracks":(mds?"MDS sessions":"CloneCD tracks"));
            for(std::size_t v=0;v<views.size();v++) {
                const auto prefix="session"+std::to_string(v+1);
                Item directory;directory.directory=true;directory.path=prefix;
                directory.attributes=0x10u;
                append(entries_,std::move(directory));
                for(auto& item:views[v].entries) {
                    item.path=prefix+"/"+item.path;
                    item.source_index=views[v].index;
                    append(entries_,std::move(item));
                }
            }
        } else {
            format_=views[0].format;
            entries_=std::move(views[0].entries);
            for(auto& item:entries_)item.source_index=views[0].index;
        }
        if(ccd) {
            const auto sub=ccd_subchannels(image_);
            if(sub) {
                // Keep the raw 96-byte-per-sector P-W stream intact. We do
                // not mistake intentionally nonstandard Q data for a damaged
                // ISO filesystem or attempt to follow sidecar links.
                const bool collision=std::any_of(entries_.begin(),entries_.end(),
                    [](const Item& item) {
                        return item.path=="subchannels" ||
                               item.path.rfind("subchannels/",0)==0;
                    });
                if(!collision) {
                    Item dir;dir.path="subchannels";dir.directory=true;dir.attributes=0x10;
                    append(entries_,std::move(dir));
                    Item raw;raw.path="subchannels/disc.sub";raw.size=sub->size;
                    raw.attributes=0x20;raw.direct_source=sub->source;
                    raw.extents.push_back({0,sub->size,false});
                    append(entries_,std::move(raw));
                    // The physical Q-channel is advisory: corrupted or
                    // intentionally protected subcodes must never hide disc
                    // files or make the raw .sub inaccessible.
                    try {
                        const std::string csv=subq_index_report(sub->source);
                        require(csv.size()<=1024u*1024u,"CloneCD Q report too large");
                        Item info;
                        info.path="subchannels/q-index.csv";
                        info.size=csv.size();
                        info.attributes=0x20u;
                        info.inline_data.assign(csv.begin(),csv.end());
                        append(entries_,std::move(info));
                    } catch(const std::exception&) {
                        // An unreadable Q report is non-fatal; disc.sub remains.
                    }
                }
            }
        }
        return;
    }
    Source source(image_);
    Udf udf(source);
    if(udf.parse()) {entries_=udf.take();format_="UDF";}
    else {
        try {entries_=parse_iso(source,format_);}
        catch(const std::exception&) {
            // Original Xbox XDVDFS uses a different volume descriptor at
            // logical sector 32 and does not have ISO9660's PVD at sector 16.
            auto xbox=xbox_xdvdfs(source.size(),[&](std::uint64_t p,std::size_t n){return source.read(p,n);});
            if(xbox.empty())throw;
            entries_=std::move(xbox);format_="Xbox XDVDFS";
        }
    }
    if(!source.layout().empty())format_+=" / "+source.layout();
}

bool Disc::copy(std::size_t index, const std::filesystem::path& output,
                const std::function<bool(std::uint64_t)>& progress, bool test_only) const {
    require(index<entries_.size(),"invalid image entry index");
    const auto& entry=entries_[index];
    if(entry.directory){
        if(!test_only) {
            auto path=output;
            while(!path.empty()) {
                require(!std::filesystem::is_symlink(path),"refuse symlink in output path");
                auto parent=path.parent_path();if(parent==path)break;path=parent;
            }
            std::filesystem::create_directories(output);
        }
        return true;
    }
    std::optional<std::size_t> track;
    if(entry.source_index!=0xffffffffu) track.emplace(entry.source_index);
    std::unique_ptr<Source> source;
    std::ifstream direct;
    if(!virtual_archive_ && !wim_archive_ && entry.direct_source.empty())source=std::make_unique<Source>(image_,track);
    else if(!virtual_archive_ && !wim_archive_) {
        require(std::filesystem::is_regular_file(entry.direct_source) &&
                !std::filesystem::is_symlink(entry.direct_source),
                "unsafe raw CloneCD sidecar source");
        direct.open(entry.direct_source,std::ios::binary|std::ios::ate);
        require(direct.is_open() && direct.tellg()>=0 &&
                static_cast<std::uint64_t>(direct.tellg())>=entry.size,
                "truncated raw CloneCD sidecar");
    }
    std::ofstream sink;
    std::filesystem::path pending;
    // Keep the original destination intact until every extent is read,
    // written and closed successfully. A cancelled extraction must not leave
    // an apparently valid but truncated file in Total Commander.
    struct PendingCleanup {
        std::ofstream& stream;
        std::filesystem::path& path;
        ~PendingCleanup() noexcept {
            if(stream.is_open())stream.close();
            if(!path.empty()) {
                std::error_code ec;
                std::filesystem::remove(path,ec);
            }
        }
    } cleanup{sink,pending};
    if(!test_only) {
        auto parent=output.parent_path();
        // Check existing ancestors before creating directories. Recheck after creation.
        for(int pass=0;pass<2;pass++) {
            for(auto dir=output;!dir.empty();) {
                require(!std::filesystem::is_symlink(dir),"refuse symlink in output path");
                auto next=dir.parent_path();if(next==dir)break;dir=next;
            }
            if(pass==0 && !parent.empty())std::filesystem::create_directories(parent);
        }
        // Write to a random sibling staging file rather than truncating an
        // existing output. The sibling lives on the same filesystem so the
        // final rename cannot require a cross-volume copy.
        std::random_device random;
        for(unsigned attempt=0;attempt<16 && pending.empty();++attempt) {
            auto candidate=output;
            const std::uint64_t nonce=(std::uint64_t(random())<<32)|random();
            candidate += ".iso-wcx-" + std::to_string(nonce) + ".part";
            if(std::filesystem::exists(candidate))continue;
            sink.open(candidate,std::ios::binary|std::ios::trunc);
            if(sink.is_open())pending=std::move(candidate);
            else sink.clear();
        }
        require(sink.is_open(),"cannot create temporary extracted file");
    }
    std::uint64_t written=0;
    auto deliver=[&](const std::uint8_t* bytes,std::size_t n) {
        if(!test_only && n) {
            sink.write(reinterpret_cast<const char*>(bytes),static_cast<std::streamsize>(n));
            require(bool(sink),"cannot write extracted file");
        }
        written+=n;
        return !progress || progress(written);
    };
    if(virtual_archive_) {
        if(!virtual_archive_->read(index,deliver))return false;
    } else if(wim_archive_) {
#ifdef _WIN32
        if(!wim_archive_->read(index,deliver))return false;
#else
        throw std::runtime_error("WIM/ESD is unsupported on this platform");
#endif
    } else if(!entry.inline_data.empty()) {
        require(entry.inline_data.size()==entry.size,"invalid inline data length");
        if(!deliver(entry.inline_data.data(),entry.inline_data.size()))return false;
    } else {
        std::vector<std::uint8_t> zeros(1024*1024,0); // Heap storage: Windows default stack is only 1 MiB.
        for(const auto& ex:entry.extents) {
            std::uint64_t remain=ex.length, offset=ex.offset;
            while(remain) {
                auto n=static_cast<std::size_t>(std::min<std::uint64_t>(remain,zeros.size()));
                if(ex.sparse) {if(!deliver(zeros.data(),n))return false;}
                else {
                    Bytes b;
                    if(direct.is_open()) {
                        require(offset<=static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()),
                                "invalid raw subchannel stream offset");
                        b.resize(n);
                        direct.clear();
                        direct.seekg(static_cast<std::streamoff>(offset));
                        require(bool(direct) && bool(direct.read(reinterpret_cast<char*>(b.data()),
                                       static_cast<std::streamsize>(n))),
                                "raw subchannel read failure");
                    } else b=source->read(offset,n);
                    if(!deliver(b.data(),n))return false;
                }
                offset+=n;remain-=n;
            }
        }
    }
    require(written==entry.size,"extraction length mismatch");
    if(sink.is_open()) {
        sink.close();
        require(!sink.fail(),"output close failed");
        // Atomic replacement on the same filesystem, including an existing
        // destination, is provided by std::filesystem::rename. A failed rename
        // leaves the prior destination unchanged; cleanup removes the staging.
        std::filesystem::rename(pending,output);
        pending.clear();
    }
    return true;
}
}

