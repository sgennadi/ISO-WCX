#include "iso_wcx/exfat_volume.hpp"
#include <algorithm>
#include <array>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace iso_wcx {
namespace {
constexpr std::uint64_t kDirectoryLimit = 8ull * 1024 * 1024;
constexpr unsigned kDepthLimit = 32;
constexpr std::size_t kFileLimit = 100000;
constexpr std::size_t kChainLimit = 1024 * 1024;
void must(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
std::uint16_t le16(const std::uint8_t* p) noexcept {
    return std::uint16_t(p[0]) | (std::uint16_t(p[1]) << 8);
}
std::uint32_t le32(const std::uint8_t* p) noexcept {
    return std::uint32_t(le16(p)) | (std::uint32_t(le16(p+2)) << 16);
}
std::uint64_t le64(const std::uint8_t* p) noexcept {
    return std::uint64_t(le32(p)) | (std::uint64_t(le32(p+4)) << 32);
}
std::uint32_t rotate_add(std::uint32_t value, std::uint8_t byte) noexcept {
    return ((value >> 1) | (value << 31)) + byte;
}
void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 128) out.push_back(static_cast<char>(cp));
    else if (cp < 2048) {
        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 63)));
    } else if (cp < 65536) {
        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
        out.push_back(static_cast<char>(0x80 | (cp & 63)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 63)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
        out.push_back(static_cast<char>(0x80 | (cp & 63)));
    }
}
} // namespace

ExfatVolume::ExfatVolume(VirtualDiskReader& disk, std::uint64_t offset,
                         std::uint64_t length)
    : disk_(disk), start_(offset), length_(length) {
    must(start_ <= disk_.size() && length_ <= disk_.size() - start_ &&
         length_ >= 12 * 512, "exFAT partition outside image");
    const auto boot = disk_.read(start_, 512);
    must(boot[0] == 0xeb && boot[1] == 0x76 && boot[2] == 0x90 &&
        std::equal(boot.begin()+3,boot.begin()+11, "EXFAT   ") &&
        boot[510] == 0x55 && boot[511] == 0xaa, "not an exFAT volume");
    must(std::all_of(boot.begin()+11,boot.begin()+64,
        [](auto b){return b==0;}), "invalid exFAT reserved boot fields");
    must(le16(boot.data()+104) == 0x100 && boot[110] == 1 &&
        (le16(boot.data()+106) & ~3u) == 0,
        "unsupported exFAT revision/flags/TexFAT");
    must(boot[108]>=9 && boot[108]<=12 && boot[109]<=7,
         "unsupported exFAT sector/cluster shift");
    sector_bytes_ = 1u << boot[108];
    cluster_bytes_ = sector_bytes_ << boot[109];
    must(cluster_bytes_ <= 1024*1024, "exFAT cluster exceeds 1 MiB limit");
    const std::uint64_t volume_sectors=le64(boot.data()+72);
    must(volume_sectors >= 24 && volume_sectors <= length_/sector_bytes_,
         "exFAT volume exceeds partition");
    volume_bytes_ = volume_sectors*sector_bytes_;
    const auto fat_sector=le32(boot.data()+80);
    const auto fat_sectors=le32(boot.data()+84);
    const auto heap_sector=le32(boot.data()+88);
    clusters_=le32(boot.data()+92);
    root_cluster_=le32(boot.data()+96);
    must(fat_sector>=24 && fat_sectors>0 && heap_sector>fat_sector &&
         fat_sectors <= heap_sector-fat_sector &&
         clusters_>0 && clusters_ < 0xfffffff0u &&
         heap_sector < volume_sectors &&
         clusters_ <= (volume_sectors-heap_sector)/(cluster_bytes_/sector_bytes_) &&
         root_cluster_ >= 2 && root_cluster_-2 < clusters_,
         "invalid exFAT metadata layout");
    fat_start_=std::uint64_t(fat_sector)*sector_bytes_;
    fat_bytes_=std::uint64_t(fat_sectors)*sector_bytes_;
    heap_start_=std::uint64_t(heap_sector)*sector_bytes_;
    must((std::uint64_t(clusters_)+2)*4 <= fat_bytes_,
         "exFAT FAT is too short for cluster count");
    // exFAT checksum excludes VolumeFlags and PercentInUse bytes.
    const auto region=read(0,12u*sector_bytes_);
    std::uint32_t checksum=0;
    for(std::size_t i=0;i<11u*sector_bytes_;++i)
        if(i!=106 && i!=107 && i!=112)
            checksum=rotate_add(checksum,region[i]);
    for(std::size_t i=11u*sector_bytes_;i<region.size();i+=4)
        must(le32(region.data()+i)==checksum,"exFAT boot checksum mismatch");
}
std::vector<std::uint8_t> ExfatVolume::read(std::uint64_t offset,std::size_t bytes) {
    must(bytes<=4u*1024u*1024u && offset <= volume_bytes_ &&
         bytes <= volume_bytes_ - offset && offset <= length_ &&
         bytes <= length_-offset,"exFAT read outside volume");
    return disk_.read(start_+offset,bytes);
}
std::uint64_t ExfatVolume::offset_of(std::uint32_t cluster) const {
    must(cluster>=2 && cluster-2 < clusters_, "exFAT cluster outside heap");
    const auto offset=heap_start_ + std::uint64_t(cluster-2)*cluster_bytes_;
    must(offset<=volume_bytes_ && cluster_bytes_<=volume_bytes_-offset,
         "exFAT cluster exceeds volume");
    return offset;
}
std::uint32_t ExfatVolume::fat_next(std::uint32_t cluster) {
    must(cluster>=2 && cluster-2<clusters_, "invalid exFAT FAT cluster");
    const auto at=std::uint64_t(cluster)*4;
    must(at<=fat_bytes_ && 4<=fat_bytes_-at,"exFAT FAT entry outside table");
    return le32(read(fat_start_+at,4).data());
}
std::vector<std::uint32_t> ExfatVolume::cluster_chain(
    std::uint32_t first,std::uint64_t length,bool contiguous,bool until_end) {
    must(first>=2 && first-2<clusters_,"invalid exFAT first cluster");
    const auto needed=length/cluster_bytes_+(length%cluster_bytes_!=0);
    must(needed>0 && needed<=clusters_ && needed<=kChainLimit,
         "exFAT stream chain exceeds bounds");
    std::vector<std::uint32_t> chain;
    const auto count=until_end ?
        std::min<std::uint64_t>(clusters_,kDirectoryLimit/cluster_bytes_) : needed;
    chain.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(count,65536)));
    std::set<std::uint32_t> seen;
    auto current=first;
    for(std::uint64_t i=0;i<count;++i) {
        must(current>=2 && current-2<clusters_,"exFAT chain points outside heap");
        if(!contiguous) must(seen.insert(current).second,"exFAT FAT chain cycle");
        chain.push_back(current);
        if(contiguous) {
            if(i+1<count) must(current<clusters_+1,
                              "exFAT contiguous stream overflows heap");
            ++current;
        } else {
            const auto next=fat_next(current);
            if(next>=0xfffffff8u) {
                must(until_end || i+1==needed,
                     "exFAT FAT chain ends prematurely");
                return chain;
            }
            must(next>=2 && next-2<clusters_,"exFAT invalid FAT successor");
            current=next;
        }
    }
    if(until_end)
        throw std::runtime_error("exFAT directory chain too long or unterminated");
    if(!contiguous) {
        const auto next=fat_next(chain.back());
        must(next>=0xfffffff8u,
             "exFAT FAT chain exceeds declared stream length");
    }
    return chain;
}
void ExfatVolume::verify_allocated(std::uint32_t cluster) {
    must(!bitmap_chain_.empty(),"exFAT allocation bitmap is missing");
    const auto bit=std::uint64_t(cluster-2);
    must(cluster>=2 && bit<clusters_,"exFAT allocation bit outside bitmap");
    const auto byte=bit/8;
    must(byte<bitmap_length_,"exFAT bitmap too small");
    const auto bitmap_cluster=byte/cluster_bytes_;
    must(bitmap_cluster<bitmap_chain_.size(),"exFAT bitmap chain short");
    const auto value=read(offset_of(bitmap_chain_[static_cast<std::size_t>(bitmap_cluster)])+
                          byte%cluster_bytes_,1)[0];
    must((value & (1u << (bit%8))) != 0,
         "exFAT stream references unallocated cluster");
}
std::vector<std::uint8_t> ExfatVolume::directory_bytes(
    std::uint32_t first,std::uint64_t bytes,bool contiguous,bool root) {
    must(root || (bytes>0 && bytes<=kDirectoryLimit),
         "exFAT directory size outside allowed range");
    if(root) bytes=cluster_bytes_;
    auto chain=cluster_chain(first,bytes,contiguous,root);
    std::vector<std::uint8_t> contents;
    must(std::uint64_t(chain.size())*cluster_bytes_<=kDirectoryLimit,
         "exFAT directory exceeds 8 MiB limit");
    contents.reserve(chain.size()*cluster_bytes_);
    for(const auto c:chain) {
        if(!bitmap_chain_.empty()) verify_allocated(c);
        auto block=read(offset_of(c),cluster_bytes_);
        contents.insert(contents.end(),block.begin(),block.end());
    }
    return contents;
}
std::uint32_t ExfatVolume::dos_datetime(std::uint16_t date,std::uint16_t time) {
    return (std::uint32_t(date)<<16)|time;
}
std::string ExfatVolume::filename(const std::vector<std::uint16_t>& units) {
    std::string output;
    for(std::size_t i=0;i<units.size();++i) {
        auto cp=std::uint32_t(units[i]);
        must(cp!=0 && cp!=0xffff,
             "exFAT filename includes terminator within length");
        if(cp>=0xd800 && cp<=0xdbff) {
            must(i+1<units.size(),
                 "truncated exFAT UTF-16 surrogate pair");
            const auto lo=units[++i];
            must(lo>=0xdc00 && lo<=0xdfff,
                 "invalid exFAT UTF-16 surrogate pair");
            cp=0x10000+((cp-0xd800)<<10)+(lo-0xdc00);
        } else must(cp<0xdc00 || cp>0xdfff,"unexpected exFAT surrogate");
        append_utf8(output,cp);
    }
    safe_component(output);
    return output;
}
void ExfatVolume::safe_component(std::string_view s) {
    must(!s.empty() && s.size()<=1000 && s!="." && s!=".." &&
         s.back()!=' ' && s.back()!='.',"unsafe exFAT filename");
    for(unsigned char c:s)
        must(c>=32 && c!=127 && c!='/' && c!='\\' && c!=':' &&
             c!='*' && c!='?' && c!='"' && c!='<' && c!='>' &&
             c!='|',"unsafe exFAT character");
    auto stem=std::string(s.substr(0,s.find('.')));
    for(auto& c:stem)if(c>='a'&&c<='z')c=char(c-'a'+'A');
    must(stem!="CON" && stem!="PRN" && stem!="AUX" &&
         stem!="NUL" && stem!="CLOCK$","reserved exFAT device filename");
    if(stem.size()==4 && (stem.substr(0,3)=="COM" || stem.substr(0,3)=="LPT"))
        must(stem[3]<'1'||stem[3]>'9',"reserved exFAT port name");
}
void ExfatVolume::parse_directory(const std::string& prefix,
    std::uint32_t cluster,std::uint64_t length,bool contiguous,
    unsigned depth,bool root) {
    must(depth<=kDepthLimit,"exFAT directory nesting too deep");
    must(visited_dirs_.insert(cluster).second,
         "exFAT circular directory reference");
    const auto contents=directory_bytes(cluster,length,contiguous,root);
    struct Subdirectory {
        std::string path;std::uint32_t cluster;
        std::uint64_t length;bool contiguous;
    };
    std::vector<Subdirectory> children;
    for(std::size_t at=0;at+32<=contents.size();) {
        const auto* e=contents.data()+at;
        const auto type=e[0];
        if(type==0) break;
        if((type&0x80)==0) {at+=32;continue;}
        if(type==0x81 && root) {
            must(bitmap_cluster_==0 && e[1]==0,
                 "unsupported multiple exFAT bitmap/TexFAT");
            bitmap_cluster_=le32(e+20);
            bitmap_length_=le64(e+24);
            must(bitmap_length_ >= (std::uint64_t(clusters_)+7)/8 &&
                 bitmap_length_ <= (std::uint64_t(clusters_)+7)/8+cluster_bytes_,
                 "invalid exFAT allocation bitmap length");
            at+=32;continue;
        }
        if(type!=0x85){at+=32;continue;}
        const auto count=static_cast<std::size_t>(e[1]);
        must(count>=2 && count<=18 &&
             (count+1)*32<=contents.size()-at,
             "invalid exFAT file-entry set length");
        std::uint16_t checksum=0;
        for(std::size_t n=0;n<(count+1)*32;++n)
            if(n!=2 && n!=3)
                checksum=static_cast<std::uint16_t>(
                    (checksum>>1)|(checksum<<15)) + contents[at+n];
        must(checksum==le16(e+2),
             "exFAT file-entry set checksum mismatch");
        const auto* stream=e+32;
        must(stream[0]==0xc0 && (stream[1]&~3u)==0,
             "missing/unsupported exFAT stream extension");
        const auto name_length=static_cast<std::size_t>(stream[3]);
        must(name_length>=1 && name_length<=255 &&
             count==1+(name_length+14)/15,
             "exFAT filename entry count mismatch");
        const auto valid_length=le64(stream+8);
        const auto size=le64(stream+24);
        const auto first=le32(stream+20);
        must(valid_length<=size,
             "exFAT valid data exceeds allocation size");
        std::vector<std::uint16_t> chars;
        chars.reserve(name_length);
        for(std::size_t i=0;i<count-1;++i) {
            const auto* name=e+64+i*32;
            must(name[0]==0xc1 && name[1]==0,
                 "invalid exFAT filename entry");
            for(unsigned j=0;j<15 && chars.size()<name_length;++j)
                chars.push_back(le16(name+2+j*2));
        }
        const auto name=filename(chars);
        const bool is_dir=(le16(e+4)&0x10u)!=0;
        if(is_dir)
            must(size>0 && valid_length==size,
                 "invalid exFAT directory valid-data length");
        if(size)
            must(first>=2 && first-2<clusters_,
                 "invalid exFAT data cluster");
        else must(!is_dir && first==0,
                  "zero-sized exFAT entry has invalid cluster");
        must(prefix.size()+name.size()+1<=1000,
             "exFAT relative path too long");
        auto path=prefix.empty()?name:prefix+"/"+name;
        must(nodes_.size()<kFileLimit && !nodes_.count(path),
             "duplicate or excessive exFAT file entries");
        Node node;
        node.info.path=path;
        node.info.size=is_dir?0:size;
        node.info.first_cluster=first;
        node.info.directory=is_dir;
        node.info.attributes=le16(e+4);
        node.info.dos_time=dos_datetime(le16(e+14),le16(e+12));
        node.length=size;
        node.valid_length=valid_length;
        node.cluster=first;
        node.contiguous=(stream[1]&2)!=0;
        files_.push_back(node.info);
        nodes_.emplace(path,node);
        if(is_dir) children.push_back({path,first,size,node.contiguous});
        at+=(count+1)*32;
    }
    if(root) {
        must(bitmap_cluster_>=2,"exFAT root has no allocation bitmap");
        bitmap_chain_=cluster_chain(bitmap_cluster_,bitmap_length_,false);
        for(auto c:bitmap_chain_)verify_allocated(c);
        for(auto c:cluster_chain(root_cluster_,cluster_bytes_,false,true))
            verify_allocated(c);
    }
    for(const auto& child:children)
        parse_directory(child.path,child.cluster,child.length,
                        child.contiguous,depth+1,false);
}
void ExfatVolume::scan_once() {
    if(scanned_)return;
    parse_directory("",root_cluster_,0,false,0,true);
    scanned_=true;
}
std::vector<VolumeFile> ExfatVolume::list() {
    scan_once();
    return files_;
}
void ExfatVolume::extract(std::string_view path,std::ostream& output) {
    scan_once();
    const auto it=nodes_.find(std::string(path));
    must(it!=nodes_.end() && !it->second.info.directory,
         "exFAT file is not a previously listed regular file");
    const auto& n=it->second;
    if(n.length==0) return;
    auto chain=cluster_chain(n.cluster,n.length,n.contiguous);
    for(auto c:chain)verify_allocated(c);
    std::uint64_t remaining=n.length;
    std::uint64_t valid=n.valid_length;
    std::array<char,65536> zero{};
    for(auto c:chain) {
        std::uint64_t take=std::min<std::uint64_t>(remaining,cluster_bytes_);
        auto on_disk=static_cast<std::size_t>(
            std::min<std::uint64_t>(take,valid));
        if(on_disk) {
            const auto block=read(offset_of(c),on_disk);
            output.write(reinterpret_cast<const char*>(block.data()),
                         static_cast<std::streamsize>(block.size()));
        }
        auto zero_bytes=take-on_disk;
        while(zero_bytes) {
            const auto chunk=static_cast<std::size_t>(
                std::min<std::uint64_t>(zero_bytes,zero.size()));
            output.write(zero.data(),static_cast<std::streamsize>(chunk));
            zero_bytes-=chunk;
        }
        must(bool(output),"exFAT output stream cancelled or failed");
        remaining-=take;
        valid-=on_disk;
    }
    must(remaining==0,"exFAT stream length mismatch");
}
} // namespace iso_wcx
