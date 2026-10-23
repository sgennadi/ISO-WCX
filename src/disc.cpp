#include "iso_wcx/disc.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <cctype>
#include <string_view>

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
    std::uint64_t length_{};
public:
    explicit Source(const std::filesystem::path& p) : file_(p,std::ios::binary|std::ios::ate) {
        require(file_.is_open(),"cannot open disc image");
        auto n=file_.tellg();require(n>=0,"cannot stat disc image");
        length_=static_cast<std::uint64_t>(n);
    }
    [[nodiscard]] std::uint64_t size() const {return length_;}
    Bytes read(std::uint64_t p, std::size_t n) {
        require(n<=max_directory && p<=length_ && n<=length_-p &&
                p<=static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()),"extent outside image");
        Bytes b(n);
        file_.clear();file_.seekg(static_cast<std::streamoff>(p));
        require(bool(file_) && (n==0 || bool(file_.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(n)))),"image read failed");
        return b;
    }
};
void append(std::vector<Item>& out,Item entry) {
    require(out.size()<max_entries,"too many disc entries");
    out.push_back(std::move(entry));
}
struct IsoRecord {std::uint32_t lba{},size{};std::uint8_t flags{};std::string name;};
std::string iso_name(const Bytes& data,std::size_t p,std::size_t n,bool joliet) {
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
    if(split!=std::string::npos && split+1<name.size() &&
       std::all_of(name.begin()+static_cast<std::ptrdiff_t>(split+1),name.end(),[](unsigned char c){return c>='0'&&c<='9';}))
        name.resize(split);
    while(!name.empty()&&name.back()=='.') name.pop_back();
    safe_name(name);return name;
}
IsoRecord record(const Bytes& b,std::size_t p,bool joliet) {
    bounds(b,p,34);
    std::uint8_t len=b[p];require(len>=34 && len<=b.size()-p,"invalid ISO directory record");
    auto n=b[p+32];require(33u+n<=len,"invalid ISO filename field");
    IsoRecord r;r.lba=u32(b,p+2);r.size=u32(b,p+10);r.flags=b[p+25];
    if(n==1&&(b[p+33]==0||b[p+33]==1)) r.name=".";
    else r.name=iso_name(b,p+33,n,joliet);
    return r;
}
std::vector<Item> parse_iso(Source& src,std::string& format) {
    Bytes primary,alternative;
    for(std::uint64_t i=16;i<16+128;i++) {
        require(i<=src.size()/iso_sector && src.size()/iso_sector-i>=1,"ISO volume descriptors truncated");
        auto b=src.read(i*iso_sector,iso_sector);
        require(std::string_view(reinterpret_cast<const char*>(b.data()+1),5)=="CD001" && b[6]==1,"not ISO 9660");
        if(b[0]==1 && primary.empty()) primary=b;
        if(b[0]==2 && b[88]=='%' && b[89]=='/' && (b[90]=='@'||b[90]=='C'||b[90]=='E'))alternative=b;
        if(b[0]==255)break;
    }
    bool joliet=!alternative.empty();
    require(joliet||!primary.empty(),"no ISO volume descriptor");
    const auto& volume=joliet?alternative:primary;
    auto root=record(volume,156,joliet);require(root.flags&2,"ISO root is not a directory");
    format=joliet?"Joliet":"ISO 9660";
    std::vector<Item> out;
    std::set<std::pair<std::uint32_t,std::uint32_t>> active;
    auto walk=[&](auto&& self,const IsoRecord& dir,const std::string& base,unsigned depth)->void {
        require(depth<64,"ISO nesting too deep");
        require(dir.size<=max_directory,"ISO directory too large");
        auto key=std::make_pair(dir.lba,dir.size);
        require(active.insert(key).second,"cyclic ISO directories");
        auto data=src.read(std::uint64_t(dir.lba)*iso_sector,dir.size);
        std::string pending_multiextent;
        for(std::size_t p=0;p<data.size();) {
            if(data[p]==0) {p+=iso_sector-(p%iso_sector);continue;}
            auto length=data[p];
            require(length>=34 && length<=data.size()-p && length<=iso_sector-(p%iso_sector),"invalid ISO record length");
            auto r=record(data,p,joliet);p+=length;
            if(r.name==".")continue;
            Item item;item.path=base+r.name;item.directory=bool(r.flags&2);item.size=item.directory?0:r.size;
            if(!item.directory)item.extents.push_back({std::uint64_t(r.lba)*iso_sector,r.size,false});
            if(!pending_multiextent.empty()) {
                require(!item.directory && item.path==pending_multiextent,"invalid ISO multi-extent sequence");
                auto& previous=out.back();
                require(previous.size<=std::numeric_limits<std::uint64_t>::max()-item.size,"ISO file size overflow");
                previous.size+=item.size;
                previous.extents.push_back(item.extents.front());
            } else {
                require(item.directory || !(r.flags&0x80u) || r.size,"invalid empty ISO multi-extent");
                append(out,std::move(item));
            }
            pending_multiextent=(r.flags&0x80u)?base+r.name:std::string{};
            if(r.flags&2) self(self,r,base+r.name+"/",depth+1);
        }
        require(pending_multiextent.empty(),"missing final ISO multi-extent record");
        active.erase(key);
    };
    walk(walk,root,"",0);
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
        if(compression==16)i+=2;
        result+=utf8(cp);
    }
    safe_name(result);return result;
}
struct UdfFe {std::uint64_t size{};std::uint8_t type{};std::vector<Extent> extents;Bytes inline_bytes;};
class Udf {
    Source& src_;
    std::uint32_t block_{};
    std::map<std::uint16_t,std::pair<std::uint32_t,std::uint32_t>> partitions_;
    std::vector<int> mapping_;
    std::vector<Item> entries_;
    std::set<std::uint64_t> active_;
    [[nodiscard]] std::uint64_t address(std::uint32_t lba,std::uint16_t map,
                                         std::uint64_t extent_length=0) const {
        require(map<mapping_.size()&&mapping_[map]>=0,"unsupported UDF partition map");
        auto it=partitions_.find(static_cast<std::uint16_t>(mapping_[map]));
        require(it!=partitions_.end() && lba<it->second.second,"UDF extent outside partition");
        require(extent_length <= std::uint64_t(it->second.second-lba)*block_,
                "UDF extent crosses partition boundary");
        return (std::uint64_t(it->second.first)+lba)*block_;
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
        auto mode=u16(b,34)&7;
        std::size_t begin=base+extra;
        if(mode==3) {
            require(result.size<=ads,"truncated embedded UDF data");
            result.inline_bytes.insert(result.inline_bytes.end(),b.begin()+static_cast<std::ptrdiff_t>(begin),b.begin()+static_cast<std::ptrdiff_t>(begin+static_cast<std::size_t>(result.size)));
            return result;
        }
        require(mode==0||mode==1,"unsupported UDF allocation descriptor type");
        std::size_t step=mode==0?8:16;
        require(ads%step==0,"truncated UDF allocation descriptors");
        std::uint64_t remain=result.size;
        for(std::size_t p=begin;p<begin+ads && remain;p+=step) {
            auto raw=u32(b,p);
            auto length=std::uint64_t(raw&0x3fffffffu);
            auto kind=raw>>30;
            require(kind!=3,"UDF allocation extent continuation unsupported");
            auto chunk=std::min(length,remain);
            if(!chunk)continue;
            bool sparse=kind!=0;
            std::uint64_t offset=0;
            if(!sparse)offset=address(u32(b,p+4),mode==0?map:u16(b,p+8),chunk);
            result.extents.push_back({offset,chunk,sparse});
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
        for(std::size_t p=0;p+38<=bytes.size();) {
            if(std::all_of(bytes.begin()+static_cast<std::ptrdiff_t>(p),bytes.begin()+static_cast<std::ptrdiff_t>(p+16),[](std::uint8_t v){return v==0;}))break;
            check_tag(bytes,p,257);
            auto n=bytes[p+19];auto impl=u16(bytes,p+36);
            require(impl<=bytes.size()-p-38 && n<=bytes.size()-p-38-impl,"invalid UDF FID length");
            auto record_length=(std::size_t(38)+impl+n+3u)&~std::size_t(3);
            require(record_length<=bytes.size()-p && record_length>0,"invalid UDF FID record");
            if(!(bytes[p+18]&0x0cu) && n) {
                auto item_name=udf_name(bytes,p+38+impl,n);
                auto child_lba=u32(bytes,p+24);auto child_map=u16(bytes,p+28);
                auto child=fe(child_lba,child_map);
                Item item;item.path=prefix+item_name;item.directory=child.type==4;
                item.size=item.directory?0:child.size;
                item.extents=std::move(child.extents);item.inline_data=std::move(child.inline_bytes);
                append(entries_,std::move(item));
                if(child.type==4)walk(child_lba,child_map,prefix+item_name+"/",depth+1);
            }
            p+=record_length;
        }
        active_.erase(key);
    }
public:
    explicit Udf(Source& src) : src_(src) {}
    bool parse() {
        constexpr std::array<std::uint32_t,4> candidates={2048,512,1024,4096};
        for(auto sector:candidates) {
            auto count=src_.size()/sector;
            std::array<std::uint64_t,3> anchors={256,count>=256?count-256:0,count?count-1:0};
            for(auto anchor:anchors) {
                if(anchor>=count)continue;
                try {
                    auto a=src_.read(anchor*sector,sector);
                    check_tag(a,0,2);
                    auto length=u32(a,16),start=u32(a,20);
                    require(length>0 && length<=16*1024*1024,"invalid UDF volume descriptor span");
                    partitions_.clear();mapping_.clear();entries_.clear();active_.clear();
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
                    require(block_==512||block_==1024||block_==2048||block_==4096,"unsupported UDF block size");
                    auto map_bytes=u32(lvd,264),map_count=u32(lvd,268);
                    require(map_count>0&&map_count<=64 && lvd.size()>=440 && map_bytes<=lvd.size()-440,"UDF partition maps too large");
                    std::size_t p=440;
                    for(std::uint32_t i=0;i<map_count;i++) {
                        require(p+2<=440+map_bytes,"short UDF partition map");
                        auto type=lvd[p],n=lvd[p+1];
                        require(n>=2 && n<=440+map_bytes-p,"truncated UDF partition map");
                        mapping_.push_back(type==1 && n==6?int(u16(lvd,p+4)):-1);
                        p+=n;
                    }
                    auto fsd_len=u32(lvd,248)&0x3fffffffu;
                    require(fsd_len>=416 && fsd_len<=max_directory,"invalid UDF file-set descriptor length");
                    auto fsd=src_.read(address(u32(lvd,252),u16(lvd,256)),static_cast<std::size_t>(fsd_len));
                    check_tag(fsd,0,256);
                    walk(u32(fsd,404),u16(fsd,408),"",0);
                    return true;
                } catch(const std::exception&) {/* Try backup anchor, then ISO fallback. */}
            }
        }
        return false;
    }
    std::vector<Item> take() {return std::move(entries_);}
};
}

Disc::Disc(std::filesystem::path image): image_(std::move(image)) {
    Source source(image_);
    Udf udf(source);
    if(udf.parse()) {entries_=udf.take();format_="UDF Type-1";}
    else entries_=parse_iso(source,format_);
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
    Source source(image_);
    std::ofstream sink;
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
        sink.open(output,std::ios::binary|std::ios::trunc);
        require(sink.is_open(),"cannot create extracted file");
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
    if(!entry.inline_data.empty()) {
        require(entry.inline_data.size()==entry.size,"invalid inline data length");
        if(!deliver(entry.inline_data.data(),entry.inline_data.size()))return false;
    } else {
        std::array<std::uint8_t,1024*1024> zeros{};
        for(const auto& ex:entry.extents) {
            std::uint64_t remain=ex.length, offset=ex.offset;
            while(remain) {
                auto n=static_cast<std::size_t>(std::min<std::uint64_t>(remain,zeros.size()));
                if(ex.sparse) {if(!deliver(zeros.data(),n))return false;}
                else {
                    auto b=source.read(offset,n);
                    if(!deliver(b.data(),n))return false;
                }
                offset+=n;remain-=n;
            }
        }
    }
    require(written==entry.size,"extraction length mismatch");
    if(sink.is_open()) {sink.close();require(!sink.fail(),"output close failed");}
    return true;
}
}
