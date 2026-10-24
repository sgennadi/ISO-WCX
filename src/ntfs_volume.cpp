#include "iso_wcx/ntfs_volume.hpp"
#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <iterator>
#include <ostream>
#include <stdexcept>

namespace iso_wcx {
namespace {
constexpr std::uint32_t kData = 0x80, kIndexRoot = 0x90;
constexpr std::uint32_t kIndexAllocation = 0xa0, kAttributeList = 0x20;
constexpr std::uint32_t kMaxFiles = 100000;
constexpr std::uint64_t kMaxMftRecords = 10000000;
void need(bool ok, const char* error) {
    if (!ok) throw std::runtime_error(error);
}
std::uint16_t le16(const std::uint8_t* p) noexcept {
    return std::uint16_t(p[0]) | (std::uint16_t(p[1])<<8);
}
std::uint32_t le32(const std::uint8_t* p) noexcept {
    return std::uint32_t(le16(p)) | (std::uint32_t(le16(p+2))<<16);
}
std::uint64_t le64(const std::uint8_t* p) noexcept {
    return std::uint64_t(le32(p)) | (std::uint64_t(le32(p+4))<<32);
}
bool power_of_two(std::uint32_t x) noexcept {return x && (x&(x-1))==0;}
std::uint32_t record_size(std::int8_t code,std::uint32_t cluster_size) {
    if (code > 0) {
        need(std::uint32_t(code)<=65536u/cluster_size,
             "NTFS record size out of bounds");
        return std::uint32_t(code)*cluster_size;
    }
    need(code<=-9 && code>=-16, "unsupported NTFS record shift");
    return 1u<<unsigned(-int(code));
}
void encode_utf8(std::string& out,std::uint32_t cp) {
    if(cp<0x80)out.push_back(static_cast<char>(cp));
    else if(cp<0x800) {
        out.push_back(static_cast<char>(0xc0|(cp>>6)));
        out.push_back(static_cast<char>(0x80|(cp&63)));
    } else if(cp<0x10000) {
        out.push_back(static_cast<char>(0xe0|(cp>>12)));
        out.push_back(static_cast<char>(0x80|((cp>>6)&63)));
        out.push_back(static_cast<char>(0x80|(cp&63)));
    } else {
        out.push_back(static_cast<char>(0xf0|(cp>>18)));
        out.push_back(static_cast<char>(0x80|((cp>>12)&63)));
        out.push_back(static_cast<char>(0x80|((cp>>6)&63)));
        out.push_back(static_cast<char>(0x80|(cp&63)));
    }
}
} // namespace

NtfsVolume::NtfsVolume(VirtualDiskReader& disk,std::uint64_t start,
                       std::uint64_t length)
    : disk_(disk),start_(start),length_(length) {
    need(start_<=disk_.size() && length_<=disk_.size()-start_ && length_>=4096,
         "NTFS partition outside virtual disk");
    const auto boot=disk_.read(start_,512);
    need((boot[0]==0xeb || boot[0]==0xe9) &&
         std::equal(boot.begin()+3,boot.begin()+11,"NTFS    ") &&
         boot[510]==0x55 && boot[511]==0xaa,"not an NTFS volume");
    sector_bytes_=le16(boot.data()+11);
    const auto spc=boot[13];
    need(sector_bytes_>=512 && sector_bytes_<=4096 &&
         power_of_two(sector_bytes_) && power_of_two(spc) &&
         spc<=128 && std::uint32_t(sector_bytes_)*spc<=1024*1024,
         "unsupported NTFS sector/cluster size");
    cluster_bytes_=sector_bytes_*spc;
    const auto total_sectors=le64(boot.data()+40);
    need(total_sectors>=8 && total_sectors<=length_/sector_bytes_,
         "invalid NTFS volume length");
    volume_bytes_=total_sectors*sector_bytes_;
    mft_lcn_=le64(boot.data()+48);
    need(mft_lcn_>0 && mft_lcn_<volume_bytes_/cluster_bytes_,
         "NTFS MFT LCN outside volume");
    record_bytes_=record_size(static_cast<std::int8_t>(boot[64]),cluster_bytes_);
    need(record_bytes_>=sector_bytes_ && record_bytes_<=65536 &&
         record_bytes_%sector_bytes_==0,
         "invalid NTFS FILE record geometry");
    const auto file0=read(mft_lcn_*cluster_bytes_,record_bytes_);
    const auto master=parse_record(file0);
    need((master.flags&1)!=0,"NTFS MFT record zero is not active");
    const auto* data=attribute(master,kData);
    need(data && data->data.nonresident && data->data.length>=record_bytes_ &&
         !data->data.runs.empty(),"unsupported or missing NTFS $MFT stream");
    mft_=data->data;
    need(mft_.length/record_bytes_<=kMaxMftRecords,
         "NTFS MFT record count exceeds safety bound");
}
std::vector<std::uint8_t> NtfsVolume::read(std::uint64_t at,std::size_t size) {
    need(size<=4u*1024u*1024u && at<=volume_bytes_ &&
         size<=volume_bytes_-at,"NTFS read outside volume");
    return disk_.read(start_+at,size);
}
void NtfsVolume::fixup(std::vector<std::uint8_t>& record,
                       const char* signature) {
    need(record.size()>=sector_bytes_ &&
         (record.size()%sector_bytes_)==0 &&
         std::equal(record.begin(),record.begin()+4,signature),
         "NTFS FILE/INDX signature mismatch");
    const auto usa_offset=le16(record.data()+4);
    const auto usa_count=le16(record.data()+6);
    const auto sectors=record.size()/sector_bytes_;
    need(usa_count==sectors+1 &&
         usa_offset>=8 &&
         std::size_t(usa_offset)+2*std::size_t(usa_count)<=record.size(),
         "NTFS update sequence array invalid");
    const auto usn=le16(record.data()+usa_offset);
    for(std::size_t i=0;i<sectors;++i) {
        const auto tail=(i+1)*sector_bytes_-2;
        need(le16(record.data()+tail)==usn,
             "NTFS update sequence fixup mismatch");
        record[tail]=record[usa_offset+2*(i+1)];
        record[tail+1]=record[usa_offset+2*(i+1)+1];
    }
}
NtfsVolume::Stream NtfsVolume::parse_nonresident(
    const std::uint8_t* b,std::size_t length,std::uint16_t attr_flags) {
    need(length>=64 && !(attr_flags&(0x0001u|0x4000u)),
         "compressed/encrypted NTFS stream unsupported");
    const auto first_vcn=le64(b+16),last_vcn=le64(b+24);
    const auto run_offset=le16(b+32);
    Stream stream;stream.nonresident=true;
    stream.length=le64(b+48);
    stream.initialized=le64(b+56);
    need(first_vcn==0 && run_offset>=64 && run_offset<length &&
         stream.initialized<=stream.length,
         "unsupported fragmented NTFS attribute metadata");
    std::uint64_t vcn=0;
    std::int64_t lcn=0;
    std::size_t at=run_offset;
    bool terminated=false;
    const auto total_clusters=volume_bytes_/cluster_bytes_;
    while(at<length) {
        const std::uint8_t layout=b[at++];
        if(layout==0){terminated=true;break;}
        const auto nlen=unsigned(layout&15);
        const auto noffset=unsigned(layout>>4);
        need(nlen>0 && nlen<=8 && noffset<=8 &&
             nlen+noffset<=length-at &&
             stream.runs.size()<kMaxFiles,
             "invalid NTFS runlist encoding");
        std::uint64_t count=0;
        for(unsigned i=0;i<nlen;++i)
            count|=std::uint64_t(b[at++])<<(8*i);
        need(count>0 && count<=total_clusters &&
             vcn<=std::numeric_limits<std::uint64_t>::max()-count,
             "invalid NTFS runlist length");
        Run run;run.first_vcn=vcn;run.clusters=count;
        run.sparse=noffset==0;
        if(!run.sparse) {
            std::uint64_t bits=0;
            for(unsigned i=0;i<noffset;++i)
                bits|=std::uint64_t(b[at+i])<<(8*i);
            if(noffset<8 && (b[at+noffset-1]&0x80))
                bits|=(~std::uint64_t(0))<<(8*noffset);
            std::int64_t delta=static_cast<std::int64_t>(bits);
            need((delta>=0 && lcn<=std::numeric_limits<std::int64_t>::max()-delta) ||
                 (delta<0 && lcn>=std::numeric_limits<std::int64_t>::min()-delta),
                 "NTFS runlist LCN overflow");
            lcn+=delta;
            need(lcn>=0 && std::uint64_t(lcn)<total_clusters &&
                 count<=total_clusters-std::uint64_t(lcn),
                 "NTFS runlist LCN outside volume");
            run.lcn=static_cast<std::uint64_t>(lcn);
            at+=noffset;
        }
        stream.runs.push_back(run);
        vcn+=count;
    }
    need(terminated && (stream.length==0 || !stream.runs.empty()) &&
         (vcn==0 ? last_vcn==0 : last_vcn==vcn-1) &&
         stream.length<=vcn*std::uint64_t(cluster_bytes_),
         "inconsistent NTFS data stream/runlist");
    return stream;
}
NtfsVolume::Record NtfsVolume::parse_record(std::vector<std::uint8_t> bytes) {
    fixup(bytes,"FILE");
    need(bytes.size()>=48,"truncated NTFS FILE header");
    const auto attr_offset=le16(bytes.data()+20);
    const auto used=le32(bytes.data()+24);
    need(attr_offset>=48 && attr_offset<used && used<=bytes.size() &&
         (attr_offset%8)==0,"invalid NTFS FILE attribute offset");
    Record r;r.sequence=le16(bytes.data()+16);
    r.flags=le16(bytes.data()+22);
    need((r.flags&1)!=0 && le64(bytes.data()+32)==0,
         "deleted/extension NTFS FILE record unsupported");
    for(std::size_t at=attr_offset;at+4<=used;) {
        const auto type=le32(bytes.data()+at);
        if(type==0xffffffffu)return r;
        need(at+16<=used,"short NTFS attribute");
        const auto size=le32(bytes.data()+at+4);
        need(size>=24 && size<=used-at && (size%8)==0 &&
             r.attributes.size()<256,"invalid NTFS attribute length/count");
        const auto* p=bytes.data()+at;
        need(type!=kAttributeList,
             "NTFS attribute lists are not supported in Preview");
        const auto name_len=p[9],name_offset=le16(p+10);
        const auto flags=le16(p+12);
        need(name_len==0 ||
             (name_offset>=16 && std::size_t(name_offset)+2*name_len<=size),
             "invalid NTFS attribute name");
        Attribute a;a.type=type;a.flags=flags;
        for(unsigned i=0;i<name_len;++i)
            a.name.push_back(static_cast<char16_t>(le16(p+name_offset+2*i)));
        if(p[8]==0) {
            const auto val_size=le32(p+16),val_off=le16(p+20);
            need(val_off>=24 && val_off<=size &&
                 val_size<=size-val_off &&
                 val_size<=1024*1024,"invalid resident NTFS data");
            a.data.resident.assign(p+val_off,p+val_off+val_size);
            a.data.length=val_size;
            a.data.initialized=val_size;
        } else {
            need(p[8]==1,"unknown NTFS residence form");
            a.data=parse_nonresident(p,size,flags);
        }
        r.attributes.push_back(std::move(a));
        at+=size;
    }
    throw std::runtime_error("NTFS attributes have no terminator");
}
const NtfsVolume::Attribute* NtfsVolume::attribute(
    const Record& r,std::uint32_t type,std::u16string_view name) {
    const Attribute* found=nullptr;
    for(const auto& a:r.attributes)
        if(a.type==type && a.name==name) {
            need(!found,"duplicate NTFS attribute stream unsupported");
            found=&a;
        }
    return found;
}
std::vector<std::uint8_t> NtfsVolume::stream_read(
    const Stream& s,std::uint64_t offset,std::size_t size) {
    need(size<=4u*1024u*1024u && offset<=s.length &&
         size<=s.length-offset,"NTFS stream read outside length");
    if(!s.nonresident) {
        need(offset<=s.resident.size() &&
             size<=s.resident.size()-offset,"NTFS resident stream truncated");
        return {s.resident.begin()+static_cast<std::ptrdiff_t>(offset),
                s.resident.begin()+static_cast<std::ptrdiff_t>(offset+size)};
    }
    std::vector<std::uint8_t> result(size,0);
    std::size_t done=0;
    while(done<size) {
        const auto absolute=offset+done;
        const auto vcn=absolute/cluster_bytes_;
        const auto within=static_cast<std::size_t>(absolute%cluster_bytes_);
        const auto take=std::min<std::size_t>(size-done,cluster_bytes_-within);
        const auto iter=std::upper_bound(s.runs.begin(),s.runs.end(),vcn,
            [](std::uint64_t v,const Run& r){return v<r.first_vcn;});
        need(iter!=s.runs.begin(),"NTFS stream has missing VCN");
        const auto& run=*std::prev(iter);
        need(vcn>=run.first_vcn && vcn-run.first_vcn<run.clusters,
             "NTFS runlist has missing VCN");
        if(!run.sparse && absolute<s.initialized) {
            const auto initialized_take=static_cast<std::size_t>(
                std::min<std::uint64_t>(take,s.initialized-absolute));
            const auto physical=(run.lcn+(vcn-run.first_vcn))*
                                 std::uint64_t(cluster_bytes_)+within;
            const auto data=read(physical,initialized_take);
            std::copy(data.begin(),data.end(),result.begin()+
                      static_cast<std::ptrdiff_t>(done));
        }
        done+=take;
    }
    return result;
}
NtfsVolume::Record NtfsVolume::file_record(std::uint64_t number) {
    need(number<kMaxMftRecords &&
         number<=std::numeric_limits<std::uint64_t>::max()/record_bytes_,
         "NTFS MFT record ID out of bounds");
    const auto offset=number*record_bytes_;
    need(offset<=mft_.length && record_bytes_<=mft_.length-offset,
         "NTFS MFT record outside stream");
    return parse_record(stream_read(mft_,offset,record_bytes_));
}
std::string NtfsVolume::utf16_name(const std::uint8_t* b,std::size_t n) {
    need(n>0 && n<=255,"invalid NTFS Unicode name length");
    std::string output;
    for(std::size_t i=0;i<n;++i) {
        auto cp=std::uint32_t(le16(b+2*i));
        if(cp>=0xd800 && cp<=0xdbff) {
            need(i+1<n,"NTFS truncated Unicode surrogate");
            const auto lo=std::uint32_t(le16(b+2*(++i)));
            need(lo>=0xdc00 && lo<=0xdfff,"NTFS invalid Unicode surrogate");
            cp=0x10000+((cp-0xd800)<<10)+lo-0xdc00;
        } else need(cp<0xdc00 || cp>0xdfff,"NTFS isolated Unicode surrogate");
        encode_utf8(output,cp);
    }
    safe_name(output);
    return output;
}
void NtfsVolume::safe_name(std::string_view s) {
    need(!s.empty() && s.size()<=1000 && s!="." && s!=".." &&
         s.back()!=' ' && s.back()!='.',"unsafe NTFS filename");
    for(unsigned char c:s)
        need(c>=32 && c!=127 && c!='/' && c!='\\' && c!=':' &&
             c!='*' && c!='?' && c!='"' && c!='<' &&
             c!='>' && c!='|',"unsafe NTFS filename character");
    std::string base(s.substr(0,s.find('.')));
    for(char& c:base)if(c>='a' && c<='z')c=char(c-'a'+'A');
    need(base!="CON" && base!="PRN" && base!="AUX" &&
         base!="NUL" && base!="CLOCK$","reserved NTFS Windows name");
    if(base.size()==4 && (base.starts_with("COM")||base.starts_with("LPT")))
        need(base[3]<'1'||base[3]>'9',"reserved NTFS port name");
}
void NtfsVolume::directory_entries(const Record& record,
    const std::function<void(std::uint64_t,std::uint16_t,
                             const std::string&,bool)>& receive) {
    const auto* root=attribute(record,kIndexRoot,u"$I30");
    need(root && !root->data.nonresident && root->data.resident.size()>=32,
         "NTFS directory lacks resident $I30 index root");
    const auto& root_bytes=root->data.resident;
    need(le32(root_bytes.data())==0x30 && le32(root_bytes.data()+4)==1,
         "unsupported NTFS directory index type/collation");
    const auto child_size=record_size(
        static_cast<std::int8_t>(root_bytes[12]),cluster_bytes_);
    need(child_size>=sector_bytes_ && child_size<=65536 &&
         child_size%sector_bytes_==0,
         "invalid NTFS index record size");
    const auto* allocation=attribute(record,kIndexAllocation,u"$I30");
    if(allocation)need(allocation->data.nonresident,
         "unsupported resident NTFS index allocation");
    std::set<std::uint64_t> visited;
    std::function<void(std::vector<std::uint8_t>,std::size_t,unsigned)> parse;
    std::function<void(std::uint64_t,unsigned)> follow;
    follow=[&](std::uint64_t vcn,unsigned depth) {
        need(depth<=32 && allocation && visited.size()<kMaxFiles &&
             visited.insert(vcn).second,
             "NTFS directory index cycle or unsupported allocation");
        need(vcn<=std::numeric_limits<std::uint64_t>::max()/cluster_bytes_,
             "NTFS index VCN overflow");
        auto bytes=stream_read(allocation->data,vcn*cluster_bytes_,child_size);
        fixup(bytes,"INDX");
        need(le64(bytes.data()+16)==vcn,
             "NTFS index block VCN mismatch");
        parse(std::move(bytes),24,depth);
    };
    parse=[&](std::vector<std::uint8_t> bytes,
              std::size_t header,unsigned depth) {
        need(depth<=32 && header<=bytes.size() &&
             bytes.size()-header>=16,
             "invalid NTFS index header bounds");
        const auto* h=bytes.data()+header;
        const auto first=le32(h);
        const auto total=le32(h+4);
        need(first>=16 && total>=first && total<=bytes.size()-header,
             "NTFS index header lengths invalid");
        std::size_t at=header+first;
        const auto end=header+total;
        bool terminal=false;
        while(at<end) {
            need(end-at>=16,"short NTFS index entry");
            const auto* e=bytes.data()+at;
            const auto n=le16(e+8),key=le16(e+10),flags=le16(e+12);
            need(n>=16 && (n%8)==0 && n<=end-at &&
                 !(flags&~3u) &&
                 key<=n-16-(flags&1?8u:0u),
                 "invalid NTFS index entry");
            if(flags&1)
                follow(le64(e+n-8),depth+1);
            if(flags&2) {
                terminal=true;
                need(key==0,"invalid NTFS index end key");
                break;
            }
            need(key>=66,"NTFS index key lacks $FILE_NAME");
            const auto* filename=e+16;
            const auto chars=std::size_t(filename[64]);
            need(66+2*chars<=key,
                 "NTFS filename index record truncated");
            const auto ns=filename[65];
            if(ns!=2 && (le32(filename+56)&0x400)!=0)
                throw std::runtime_error("NTFS reparse point rejected");
            if(ns!=2) {
                const auto name=utf16_name(filename+66,chars);
                const auto reference=le64(e);
                const auto file_number=reference & 0xffffffffffffull;
                const auto sequence=static_cast<std::uint16_t>(reference>>48);
                receive(file_number,sequence,name,
                        (le32(filename+56)&0x10000000u)!=0);
            }
            at+=n;
        }
        need(terminal,"unterminated NTFS index node");
    };
    parse(root_bytes,16,0);
}
void NtfsVolume::walk(std::uint64_t file_number,std::uint16_t sequence,
                      const std::string& prefix,unsigned depth) {
    need(depth<=32 && visited_directories_.insert(file_number).second,
         "NTFS directory depth/cycle exceeds limits");
    const auto r=file_record(file_number);
    need((r.flags&3)==3 && (sequence==0 || r.sequence==sequence),
         "NTFS directory reference invalid");
    struct Child {std::uint64_t number;std::uint16_t sequence;std::string path;};
    std::vector<Child> children;
    directory_entries(r,[&](std::uint64_t id,std::uint16_t seq,
                            const std::string& name,bool directory) {
        need(id>=16 && files_.size()<kMaxFiles &&
             prefix.size()+name.size()+1<=1000,
             "invalid or excessive NTFS directory entries");
        const auto path=prefix.empty()?name:prefix+"/"+name;
        const auto item=file_record(id);
        need((seq==0 || item.sequence==seq) &&
             (item.flags&1)!=0 &&
             ((item.flags&2)!=0)==directory,
             "stale NTFS index FILE reference");
        VolumeFile f;f.path=path;f.directory=directory;
        f.attributes=directory?0x10u:0x20u;
        if(!directory) {
            const auto* data=attribute(item,kData);
            if(!data) {
                // File can legitimately have zero-byte content and no
                // unnamed data stream (skip non-regular metadata).
                f.size=0;
            } else {
                need(!(data->flags&0x4001u),
                     "compressed/encrypted NTFS file unsupported");
                f.size=data->data.length;
            }
        }
        need(!files_.count(path),"duplicate NTFS file path");
        files_.emplace(path,FileNode{f,id,seq});
        if(directory)children.push_back({id,seq,path});
    });
    for(const auto& child:children)
        walk(child.number,child.sequence,child.path,depth+1);
}
void NtfsVolume::scan_once() {
    if(scanned_)return;
    walk(5,0,"",0);
    scanned_=true;
}
std::vector<VolumeFile> NtfsVolume::list() {
    scan_once();
    std::vector<VolumeFile> result;result.reserve(files_.size());
    for(const auto& pair:files_) result.push_back(pair.second.info);
    return result;
}
void NtfsVolume::extract(std::string_view path,std::ostream& output) {
    scan_once();
    const auto it=files_.find(std::string(path));
    need(it!=files_.end() && !it->second.info.directory,
         "NTFS file is not a listed regular file");
    const auto record=file_record(it->second.file_number);
    need(it->second.sequence==0 || record.sequence==it->second.sequence,
         "NTFS MFT file changed during extraction");
    const auto* data=attribute(record,kData);
    if(!data) {
        need(it->second.info.size==0,"missing NTFS data stream");
        return;
    }
    need(!(data->flags&0x4001u) &&
         data->data.length==it->second.info.size,
         "NTFS unsupported or changed file stream");
    for(std::uint64_t at=0;at<data->data.length;) {
        const auto count=static_cast<std::size_t>(
            std::min<std::uint64_t>(64*1024,data->data.length-at));
        const auto block=stream_read(data->data,at,count);
        output.write(reinterpret_cast<const char*>(block.data()),
                     static_cast<std::streamsize>(block.size()));
        need(bool(output),"NTFS extraction cancelled/output failed");
        at+=count;
    }
}
} // namespace iso_wcx
