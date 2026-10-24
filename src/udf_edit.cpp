#include "iso_wcx/archive_edit.hpp"
#include "iso_wcx/disc.hpp"
#include "iso_wcx/edit_transaction.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace iso_wcx {
namespace {
using Bytes=std::vector<std::uint8_t>;
constexpr std::uint64_t block=2048;
constexpr std::uint64_t max_image=0xffffffffull*block;
constexpr std::size_t max_dir=8u*1024u*1024u;
constexpr unsigned max_depth=32;
constexpr std::size_t max_items=100000;

void need(bool condition,const char* text) {
    if(!condition)throw EditUnsupported(text);
}
std::uint16_t u16(const Bytes& b,std::size_t at) {
    need(at<=b.size() && b.size()-at>=2,"truncated UDF uint16");
    return std::uint16_t(b[at]) | (std::uint16_t(b[at+1])<<8);
}
std::uint32_t u32(const Bytes& b,std::size_t at) {
    need(at<=b.size() && b.size()-at>=4,"truncated UDF uint32");
    return std::uint32_t(u16(b,at)) | (std::uint32_t(u16(b,at+2))<<16);
}
std::uint64_t u64(const Bytes& b,std::size_t at) {
    return std::uint64_t(u32(b,at)) | (std::uint64_t(u32(b,at+4))<<32);
}
void put16(Bytes& b,std::size_t at,std::uint16_t n) {
    need(at<=b.size() && b.size()-at>=2,"short UDF uint16 write");
    b[at]=static_cast<std::uint8_t>(n);
    b[at+1]=static_cast<std::uint8_t>(n>>8);
}
void put32(Bytes& b,std::size_t at,std::uint32_t n) {
    put16(b,at,static_cast<std::uint16_t>(n));
    put16(b,at+2,static_cast<std::uint16_t>(n>>16));
}
void put64(Bytes& b,std::size_t at,std::uint64_t n) {
    put32(b,at,static_cast<std::uint32_t>(n));
    put32(b,at+4,static_cast<std::uint32_t>(n>>32));
}
std::uint16_t crc16(const std::uint8_t* data,std::size_t len) {
    std::uint16_t crc=0;
    for(std::size_t i=0;i<len;++i) {
        crc^=std::uint16_t(data[i])<<8;
        for(unsigned bit=0;bit<8;++bit)
            crc=static_cast<std::uint16_t>(
                (crc&0x8000u)?(crc<<1)^0x1021u:(crc<<1));
    }
    return crc;
}
void tag_check(const Bytes& b,std::size_t offset,std::uint16_t expected) {
    need(offset<=b.size() && b.size()-offset>=16 &&
         u16(b,offset)==expected,"unexpected UDF tag identifier");
    const auto version=u16(b,offset+2);
    need(version==2 || version==3,"unsupported UDF descriptor version");
    std::uint32_t sum=0;
    for(std::size_t j=0;j<16;++j)
        if(j!=4)sum+=b[offset+j];
    need((sum&255)==b[offset+4],"UDF tag checksum mismatch");
    const auto length=u16(b,offset+10);
    need(length<=b.size()-(offset+16),"UDF tag CRC length outside descriptor");
    if(length)need(crc16(b.data()+offset+16,length)==u16(b,offset+8),
                   "UDF descriptor CRC16 mismatch");
}
void tag_update(Bytes& b,std::size_t at,std::size_t descriptor_bytes) {
    need(at<=b.size() && descriptor_bytes>=16 &&
         descriptor_bytes<=b.size()-at &&
         descriptor_bytes-16<=65535,"UDF tag update outside record");
    const auto count=static_cast<std::uint16_t>(descriptor_bytes-16);
    put16(b,at+10,count);
    put16(b,at+8,crc16(b.data()+at+16,count));
    std::uint32_t sum=0;
    for(std::size_t i=0;i<16;++i)
        if(i!=4)sum+=b[at+i];
    b[at+4]=static_cast<std::uint8_t>(sum);
}
void utf8(std::string& text,std::uint32_t c) {
    need(c!=0 && c<=0x10ffff && !(c>=0xd800 && c<=0xdfff),
         "invalid UDF Unicode scalar");
    if(c<=0x7f)text.push_back(static_cast<char>(c));
    else if(c<=0x7ff) {
        text.push_back(static_cast<char>(0xc0|(c>>6)));
        text.push_back(static_cast<char>(0x80|(c&63)));
    } else if(c<=0xffff) {
        text.push_back(static_cast<char>(0xe0|(c>>12)));
        text.push_back(static_cast<char>(0x80|((c>>6)&63)));
        text.push_back(static_cast<char>(0x80|(c&63)));
    } else {
        text.push_back(static_cast<char>(0xf0|(c>>18)));
        text.push_back(static_cast<char>(0x80|((c>>12)&63)));
        text.push_back(static_cast<char>(0x80|((c>>6)&63)));
        text.push_back(static_cast<char>(0x80|(c&63)));
    }
}
std::string name(const Bytes& b,std::size_t p,std::size_t n) {
    need(n>=2 && p<=b.size() && n<=b.size()-p,
         "invalid UDF file identifier length");
    const auto compression=b[p++];
    --n;
    std::string result;
    if(compression==8) {
        for(std::size_t i=0;i<n;++i)utf8(result,b[p+i]);
    } else {
        need(compression==16 && n%2==0,"invalid UDF CS0 filename");
        for(std::size_t i=0;i<n;i+=2) {
            std::uint32_t cp=(std::uint32_t(b[p+i])<<8)|b[p+i+1];
            if(cp>=0xd800 && cp<=0xdbff) {
                need(i+3<n,"truncated UDF filename surrogate");
                const auto low=(std::uint32_t(b[p+i+2])<<8)|b[p+i+3];
                need(low>=0xdc00 && low<=0xdfff,
                     "invalid UDF filename surrogate");
                cp=0x10000+((cp-0xd800)<<10)+low-0xdc00;
                i+=2;
            }
            utf8(result,cp);
        }
    }
    need(result.find('/')==std::string::npos,
         "UDF file identifier contains a path separator");
    return edit_path(result);
}

struct Location {
    std::uint64_t at=0,bytes=0;
};
bool intersects(Location a,Location b) {
    return a.bytes && b.bytes &&
           a.at<b.at+b.bytes && b.at<a.at+a.bytes;
}
struct Fe {
    Bytes bytes;
    std::uint64_t at{},data_at{},size{},capacity{};
    std::size_t base{},ads{},ad_offset{};
    unsigned storage{};
    std::uint8_t type{};
};
struct File {
    Fe fe;
    std::uint64_t fid_at{};
    std::size_t fid_bytes{};
    std::uint64_t parent_at{};
    bool parent_embedded{};
};
struct Interval {
    Location range;
    std::uint64_t owner;
};

class Type1 {
public:
    explicit Type1(const std::filesystem::path& path)
        :file_(path,std::ios::binary),size_(std::filesystem::file_size(path)) {
        need(file_.good() && size_%block==0 && size_>=300*block &&
             size_<max_image,"unsupported UDF image size/sector geometry");
        const auto system=read(0,16*block);
        need(std::all_of(system.begin(),system.end(),
                         [](std::uint8_t x){return x==0;}),
             "UDF system-area hybrid image is read-only");
        bool vrs=false;
        for(std::uint64_t i=16;i<256 && (i+1)*block<=size_;++i) {
            const auto b=read(i*block,8);
            if(std::equal(b.begin()+1,b.begin()+6,"CD001"))
                throw EditUnsupported("ISO/UDF Bridge metadata rewriting unsupported");
            if(std::equal(b.begin()+1,b.begin()+6,"NSR02") ||
               std::equal(b.begin()+1,b.begin()+6,"NSR03"))
                vrs=true;
        }
        need(vrs,"UDF-only Type-1 editing requires a volume recognition sequence");
        const auto anchor=read(256*block,block);
        tag_check(anchor,0,2);
        need(u32(anchor,12)==256,"unexpected UDF anchor tag location");
        const auto len=u32(anchor,16);
        const auto start=u32(anchor,20);
        need(len>=block && len<=16*1024*1024 &&
             std::uint64_t(start)*block<size_ &&
             len<=size_-std::uint64_t(start)*block,
             "invalid UDF main descriptor sequence");
        Bytes pd,lvd;
        for(std::uint64_t i=0;i<(len+block-1)/block;++i) {
            const auto b=read((std::uint64_t(start)+i)*block,block);
            const auto tag=u16(b,0);
            tag_check(b,0,tag);
            if(tag==5) {need(pd.empty(),"multiple UDF partitions unsupported");pd=b;}
            if(tag==6) {need(lvd.empty(),"multiple UDF logical volumes unsupported");lvd=b;}
            if(tag==8) break;
        }
        need(!pd.empty() && !lvd.empty() && u32(lvd,212)==block &&
             u32(lvd,264)==6 && u32(lvd,268)==1 &&
             lvd[440]==1 && lvd[441]==6 &&
             u16(lvd,444)==u16(pd,22),
             "only single physical UDF Type-1 partition maps are writable");
        partition_=u32(pd,188);
        partition_blocks_=u32(pd,192);
        need(partition_blocks_>2 && partition_<size_/block &&
             partition_blocks_<=size_/block-partition_,
             "UDF partition bounds outside image");
        const auto fsd_size=u32(lvd,248)&0x3fffffffu;
        need(fsd_size>=416 && fsd_size<=block && u16(lvd,256)==0,
             "unsupported UDF file-set mapping");
        const auto fsd=read(physical(u32(lvd,252)),block);
        tag_check(fsd,0,256);
        reserved_.push_back({{256*block,block},0});
        reserved_.push_back({{std::uint64_t(start)*block,len},0});
        reserved_.push_back({{physical(u32(lvd,252)),block},0});
        walk(u32(fsd,404),u16(fsd,408),"",0);
    }

    [[nodiscard]] const std::map<std::string,File>& files() const noexcept {
        return files_;
    }
    [[nodiscard]] const std::vector<Interval>& intervals() const noexcept {
        return reserved_;
    }
    void close() {file_.close();}
private:
    std::ifstream file_;
    std::uint64_t size_{};
    std::uint32_t partition_{},partition_blocks_{};
    std::map<std::string,File> files_;
    std::map<std::uint64_t,std::size_t> references_;
    std::set<std::uint32_t> directories_;
    std::vector<Interval> reserved_;

    Bytes read(std::uint64_t offset,std::size_t bytes) {
        need(offset<=size_ && bytes<=size_-offset && bytes<=max_dir,
             "UDF file reader out of image bounds");
        Bytes result(bytes);
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(offset));
        file_.read(reinterpret_cast<char*>(result.data()),
                   static_cast<std::streamsize>(bytes));
        need(bool(file_),"UDF image read failed");
        return result;
    }
    std::uint64_t physical(std::uint32_t logical) const {
        need(logical<partition_blocks_,"UDF extent outside partition");
        return (std::uint64_t(partition_)+logical)*block;
    }
    Fe parse_fe(std::uint32_t lba,std::uint16_t map) {
        need(map==0,"UDF nonphysical map index is read-only");
        const auto offset=physical(lba);
        Fe fe;fe.at=offset;fe.bytes=read(offset,block);
        const auto type=u16(fe.bytes,0);
        need(type==261||type==266,"unsupported UDF file entry descriptor");
        tag_check(fe.bytes,0,type);
        fe.type=fe.bytes[27];
        need(fe.type==4 || fe.type==5,
             "UDF special files/symlinks are not writable");
        fe.storage=u16(fe.bytes,34)&7u;
        need(fe.storage==0 || fe.storage==3,
             "UDF LongAD/extended allocation is read-only");
        fe.size=u64(fe.bytes,56);
        fe.base=type==261?176:216;
        const auto ea=u32(fe.bytes,type==261?168:208);
        fe.ads=u32(fe.bytes,type==261?172:212);
        need(ea<=block-fe.base && fe.ads<=block-fe.base-ea,
             "UDF file-entry AD outside descriptor");
        fe.ad_offset=fe.base+ea;
        if(fe.storage==3) {
            need(fe.size<=fe.ads,"UDF embedded file size exceeds AD length");
            fe.data_at=offset+fe.ad_offset;
            fe.capacity=block-fe.ad_offset;
            need(fe.size<=fe.capacity,"UDF embedded data outside descriptor");
        } else {
            need(fe.ads==8,"only one short UDF extent can be rewritten");
            const auto raw=u32(fe.bytes,fe.ad_offset);
            need((raw>>30)==0,"unrecorded/sparse UDF extent is read-only");
            const auto lba_data=u32(fe.bytes,fe.ad_offset+4);
            const auto sectors=(std::uint64_t(raw&0x3fffffffu)+block-1)/block;
            need(sectors>0 && lba_data<partition_blocks_ &&
                 sectors<=partition_blocks_-lba_data,
                 "UDF data extent outside partition");
            fe.data_at=physical(lba_data);
            // The allocation occupies whole logical blocks, although the
            // shortAD recorded length may be smaller than one block.
            // Growth is permitted only inside those blocks and after an
            // overlap check against all indexed data/metadata allocations.
            fe.capacity=sectors*block;
            need(fe.size<=std::uint64_t(raw&0x3fffffffu),
                 "UDF file size exceeds recorded extent");
            need(fe.capacity<=size_-fe.data_at,
                 "UDF extent outside disk");
        }
        return fe;
    }
    Bytes directory_data(const Fe& fe) {
        need(fe.type==4 && fe.size<=max_dir,
             "unsupported oversized or invalid UDF directory");
        return read(fe.data_at,static_cast<std::size_t>(fe.size));
    }
    void walk(std::uint32_t lba,std::uint16_t map,
              const std::string& prefix,unsigned depth) {
        need(depth<=max_depth && directories_.insert(lba).second &&
             directories_.size()<=max_items,
             "UDF directory depth, alias or cycle unsupported");
        const auto directory=parse_fe(lba,map);
        need(directory.type==4,"UDF FID references a non-directory");
        reserved_.push_back({{directory.at,block},directory.at});
        if(directory.storage!=3)
            reserved_.push_back({{directory.data_at,directory.capacity},
                                 directory.at});
        const auto data=directory_data(directory);
        for(std::size_t p=0;p+38<=data.size();) {
            if(std::all_of(data.begin()+static_cast<std::ptrdiff_t>(p),
                           data.begin()+static_cast<std::ptrdiff_t>(p+16),
                           [](std::uint8_t n){return n==0;}))break;
            tag_check(data,p,257);
            const auto n=std::size_t(data[p+19]);
            const auto impl=std::size_t(u16(data,p+36));
            need(impl<=data.size()-p-38 && n<=data.size()-p-38-impl,
                 "UDF directory FID name out of bounds");
            const auto len=(38+impl+n+3u)&~std::size_t(3);
            need(len>=40 && len<=data.size()-p,
                 "truncated UDF FID");
            const auto characteristics=data[p+18];
            const auto child_lba=u32(data,p+24);
            const auto child_map=u16(data,p+28);
            if(n && !(characteristics&0x0cu)) {
                const auto filename=name(data,p+38+impl,n);
                const auto path=prefix.empty()?filename:prefix+"/"+filename;
                need(path.size()<=2048 && files_.size()<max_items,
                     "too many UDF file entries");
                auto child=parse_fe(child_lba,child_map);
                ++references_[child.at];
                need(references_[child.at]==1,
                     "hard-linked/multiple referenced UDF entries read-only");
                if(child.type==4) {
                    reserved_.push_back({{child.at,block},child.at});
                    walk(child_lba,child_map,path,depth+1);
                } else {
                    reserved_.push_back({{child.at,block},child.at});
                    if(child.storage!=3)
                        reserved_.push_back({{child.data_at,child.capacity},
                                             child.at});
                    File file;
                    file.fe=std::move(child);
                    file.fid_at=directory.data_at+p;
                    file.fid_bytes=len;
                    file.parent_at=directory.at;
                    file.parent_embedded=directory.storage==3;
                    need(files_.emplace(path,std::move(file)).second,
                         "duplicate UDF file path");
                }
            }
            p+=len;
        }
    }
};

void staged_write(std::fstream& file,std::uint64_t at,const Bytes& b) {
    file.clear();
    file.seekp(static_cast<std::streamoff>(at));
    need(bool(file),"cannot seek staged UDF descriptor");
    file.write(reinterpret_cast<const char*>(b.data()),
               static_cast<std::streamsize>(b.size()));
    need(bool(file),"cannot write staged UDF descriptor");
}
Bytes staged_read(std::fstream& file,std::uint64_t at,std::size_t n) {
    Bytes out(n);
    file.flush();
    need(bool(file),"cannot flush staged UDF descriptor");
    file.clear();
    file.seekg(static_cast<std::streamoff>(at));
    file.read(reinterpret_cast<char*>(out.data()),
              static_cast<std::streamsize>(n));
    need(bool(file),"cannot read staged UDF descriptor");
    return out;
}
void copy(std::ifstream& in,std::fstream& out,std::uint64_t n,
          const EditProgress& progress) {
    Bytes buffer(1024*1024);
    while(n) {
        edit_tick(progress);
        const auto count=static_cast<std::size_t>(
            std::min<std::uint64_t>(n,buffer.size()));
        in.read(reinterpret_cast<char*>(buffer.data()),
                static_cast<std::streamsize>(count));
        need(bool(in),"UDF replacement source truncated");
        out.write(reinterpret_cast<const char*>(buffer.data()),
                  static_cast<std::streamsize>(count));
        need(bool(out),"staged UDF data write failed");
        n-=count;
    }
}
bool same_file(const std::filesystem::path& left,
               const std::filesystem::path& right,
               const EditProgress& progress) {
    std::ifstream a(left,std::ios::binary),b(right,std::ios::binary);
    need(bool(a) && bool(b),"cannot open UDF verification file");
    if(std::filesystem::file_size(left)!=std::filesystem::file_size(right))
        return false;
    Bytes aa(1024*1024),bb(1024*1024);
    auto n=std::filesystem::file_size(left);
    while(n) {
        edit_tick(progress);
        const auto count=static_cast<std::size_t>(
            std::min<std::uint64_t>(n,aa.size()));
        a.read(reinterpret_cast<char*>(aa.data()),
               static_cast<std::streamsize>(count));
        b.read(reinterpret_cast<char*>(bb.data()),
               static_cast<std::streamsize>(count));
        need(bool(a) && bool(b),"UDF verification read truncated");
        if(!std::equal(aa.begin(),aa.begin()+
                       static_cast<std::ptrdiff_t>(count),bb.begin()))
            return false;
        n-=count;
    }
    return true;
}
} // namespace

void edit_udf(const std::filesystem::path& image,
              const std::vector<ArchiveChange>& changes,
              const EditProgress& progress) {
    edit_tick(progress);
    const Disc before(image);
    need(before.format()=="UDF","not a plain readable UDF volume");
    Type1 volume(image);
    need(!changes.empty(),"no UDF changes");
    std::set<std::string> distinct;
    for(const auto& change:changes) {
        const auto name=edit_path(change.path);
        need(distinct.insert(name).second,"duplicate UDF change path");
        need(!change.directory,"UDF directory creation/deletion not yet supported");
        const auto it=volume.files().find(name);
        need(it!=volume.files().end(),"UDF only supports existing files");
        const auto& target=it->second;
        need(target.fe.type==5,
             "UDF deletion or replacement of a directory is unsupported");
        if(!change.remove) {
            need(std::filesystem::is_regular_file(
                     std::filesystem::symlink_status(change.source)),
                 "UDF replacement must be a regular file, not symlink");
            const auto len=std::filesystem::file_size(change.source);
            need(len<=target.fe.capacity,
                 "UDF replacement exceeds existing allocation capacity");
            if(target.fe.storage!=3) {
                const Location dest{target.fe.data_at,target.fe.capacity};
                for(const auto& interval:volume.intervals())
                    if(interval.owner!=target.fe.at)
                        need(!intersects(dest,interval.range),
                             "UDF file extent overlaps another file/descriptor");
            }
        }
    }

    EditTransaction tx(image);
    std::fstream out(tx.output,std::ios::binary|std::ios::in|
                     std::ios::out|std::ios::trunc);
    need(bool(out),"cannot create staged UDF image");
    std::ifstream original(image,std::ios::binary);
    need(bool(original),"cannot open original UDF image");
    copy(original,out,std::filesystem::file_size(image),progress);
    original.close();
    volume.close();
    for(const auto& change:changes) {
        edit_tick(progress);
        const auto& target=volume.files().at(edit_path(change.path));
        if(change.remove) {
            auto fid=staged_read(out,target.fid_at,target.fid_bytes);
            tag_check(fid,0,257);
            need(!(fid[18]&0x0cu),"UDF directory entry is already deleted");
            fid[18]|=0x04; // UDF FID deleted characteristic.
            tag_update(fid,0,fid.size());
            staged_write(out,target.fid_at,fid);
            if(target.parent_embedded) {
                auto parent=staged_read(out,target.parent_at,block);
                const auto tag=u16(parent,0);
                tag_check(parent,0,tag);
                const auto base=tag==261?176u:216u;
                const auto ea=u32(parent,tag==261?168:208);
                const auto ad=u32(parent,tag==261?172:212);
                tag_update(parent,0,base+ea+ad);
                staged_write(out,target.parent_at,parent);
            }
            continue;
        }
        const auto len=std::filesystem::file_size(change.source);
        auto fe=staged_read(out,target.fe.at,block);
        tag_check(fe,0,u16(fe,0));
        if(target.fe.storage==3) {
            Bytes data(static_cast<std::size_t>(len));
            std::ifstream input(change.source,std::ios::binary);
            need(bool(input),"cannot open UDF replacement");
            input.read(reinterpret_cast<char*>(data.data()),
                       static_cast<std::streamsize>(data.size()));
            need(bool(input) || data.empty(),"UDF source shortened");
            const auto at=target.fe.ad_offset;
            std::fill(fe.begin()+static_cast<std::ptrdiff_t>(at),
                      fe.begin()+static_cast<std::ptrdiff_t>(
                          at+std::max<std::uint64_t>(len,target.fe.size)),0);
            std::copy(data.begin(),data.end(),
                      fe.begin()+static_cast<std::ptrdiff_t>(at));
            // For Extended File Entry this is at 212; regular File
            // Entries keep the allocation-descriptor length at 172.
            put32(fe,u16(fe,0)==266?212u:172u,
                  static_cast<std::uint32_t>(len));
            put64(fe,56,len);
            tag_update(fe,0,target.fe.ad_offset+static_cast<std::size_t>(len));
            staged_write(out,target.fe.at,fe);
        } else {
            out.clear();
            out.seekp(static_cast<std::streamoff>(target.fe.data_at));
            need(bool(out),"cannot seek UDF content extent");
            std::ifstream source(change.source,std::ios::binary);
            need(bool(source),"cannot open UDF replacement content");
            copy(source,out,len,progress);
            put64(fe,56,len);
            // Maintain the old extent length on shrink, otherwise extend
            // the short AD only within the already-allocated full blocks.
            const auto current=u32(fe,target.fe.ad_offset)&0x3fffffffu;
            if(len>current) {
                need(len<=0x3fffffffull,
                     "UDF short AD cannot describe this file size");
                put32(fe,target.fe.ad_offset,
                      static_cast<std::uint32_t>(len));
            }
            tag_update(fe,0,target.fe.ad_offset+target.fe.ads);
            staged_write(out,target.fe.at,fe);
        }
    }
    out.flush();
    need(bool(out),"staged UDF flush failed");
    out.close();

    {
        const Disc checked(tx.output);
        need(checked.format()=="UDF","staged UDF metadata did not validate");
        std::map<std::string,std::size_t> by_path;
        for(std::size_t i=0;i<checked.items().size();++i)
            need(by_path.emplace(checked.items()[i].path,i).second,
                 "duplicate files in staged UDF namespace");
        for(std::size_t i=0;i<changes.size();++i) {
            edit_tick(progress);
            const auto path=edit_path(changes[i].path);
            const auto it=by_path.find(path);
            if(changes[i].remove) {
                need(it==by_path.end(),"deleted UDF file remains visible");
                continue;
            }
            need(it!=by_path.end() &&
                 checked.items()[it->second].size==
                     std::filesystem::file_size(changes[i].source),
                 "staged UDF file size mismatch");
            const auto dest=tx.workspace/("udf-check-"+std::to_string(i));
            need(checked.copy(it->second,dest),"staged UDF extraction cancelled");
            need(same_file(dest,changes[i].source,progress),
                 "UDF staged file differs from replacement source");
        }
    }
    tx.commit(progress);
}
} // namespace iso_wcx
