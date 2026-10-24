#include "iso_wcx/ext_volume.hpp"
#include <algorithm>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace iso_wcx {
namespace {
constexpr std::size_t kMaxItems=100000;
constexpr std::uint64_t kMaxDirBytes=8ull*1024*1024;
constexpr std::uint64_t kMaxBlocks=0x100000000ull;
void need(bool ok,const char* message){
    if(!ok)throw std::runtime_error(message);
}
std::uint16_t u16(const std::uint8_t* p){
    return std::uint16_t(p[0])|(std::uint16_t(p[1])<<8);
}
std::uint32_t u32(const std::uint8_t* p){
    return std::uint32_t(u16(p))|(std::uint32_t(u16(p+2))<<16);
}
bool valid_utf8(std::string_view s) {
    for(std::size_t i=0;i<s.size();) {
        const auto c=static_cast<unsigned char>(s[i++]);
        if(c<128)continue;
        unsigned count{};std::uint32_t cp{};
        if(c>=0xc2&&c<=0xdf){count=1;cp=c&31;}
        else if(c>=0xe0&&c<=0xef){count=2;cp=c&15;}
        else if(c>=0xf0&&c<=0xf4){count=3;cp=c&7;}
        else return false;
        if(count>s.size()-i)return false;
        for(unsigned j=0;j<count;++j){
            const auto x=static_cast<unsigned char>(s[i++]);
            if((x&0xc0)!=0x80)return false;
            cp=(cp<<6)|(x&63);
        }
        if((count==1&&cp<128)||(count==2&&cp<2048)||
           (count==3&&cp<65536)||cp>0x10ffff||
           (cp>=0xd800&&cp<=0xdfff))return false;
    }
    return true;
}
}
ExtVolume::ExtVolume(VirtualDiskReader& disk,
    std::uint64_t start,std::uint64_t length)
    :disk_(disk),start_(start),length_(length) {
    need(start_<=disk_.size()&&length_<=disk_.size()-start_&&
         length_>=2048,"ext partition outside image");
    const auto sb=read(1024,1024);
    need(u16(sb.data()+56)==0xef53,"not an ext2/3/4 filesystem");
    const auto log=u32(sb.data()+24);
    need(log<=2,"unsupported ext block size");
    block_size_=1024u<<log;
    first_data_=u32(sb.data()+20);
    bpg_=u32(sb.data()+32);
    ipg_=u32(sb.data()+40);
    inode_count_=u32(sb.data());
    const auto incompat=u32(sb.data()+96);
    const auto rocompat=u32(sb.data()+100);
    // No journal replay, encrypted/inline files, meta_bg or bigalloc.
    // Only known ext-compatible group and extent options are enabled.
    const std::uint32_t supported=0x2u|0x40u|0x80u|0x200u|0x2000u;
    need(!(incompat&~supported)&&!(incompat&4u)&&!(rocompat&0x200u),
         "unsupported ext features or journal needs recovery");
    feature64_=(incompat&0x80u)!=0;
    desc_size_=feature64_?u16(sb.data()+254):32;
    if(!feature64_)desc_size_=32;
    need(desc_size_>=32&&desc_size_<=block_size_&&desc_size_%8==0,
         "invalid ext group descriptor size");
    inode_size_=u32(sb.data()+76)==0?128u:u16(sb.data()+88);
    need(inode_size_>=128&&inode_size_<=block_size_&&
         (inode_size_&3)==0,"invalid ext inode size");
    blocks_=u32(sb.data()+4);
    if(feature64_)blocks_|=std::uint64_t(u32(sb.data()+336))<<32;
    need(blocks_>=8&&blocks_<=kMaxBlocks&&
         blocks_<=length_/block_size_&&inode_count_>=2&&
         bpg_>0&&ipg_>0&&ipg_<=inode_count_&&
         first_data_<=1&&first_data_<blocks_,
         "invalid ext superblock geometry");
    const auto groups=(blocks_-first_data_+bpg_-1)/bpg_;
    need(groups>0&&groups<=65536&&
         groups<=(std::uint64_t(inode_count_)+ipg_-1)/ipg_,
         "unsupported ext group geometry");
    groups_=static_cast<std::uint32_t>(groups);
    descriptor_offset_=std::uint64_t(first_data_+1)*block_size_;
    need(descriptor_offset_+groups*desc_size_<=length_,
         "ext group descriptor table outside partition");
}
std::vector<std::uint8_t> ExtVolume::read(std::uint64_t offset,std::size_t n){
    need(n<=4u*1024u*1024u&&offset<=length_&&
         n<=length_-offset,"ext read outside partition");
    return disk_.read(start_+offset,n);
}
std::vector<std::uint8_t> ExtVolume::read_block(std::uint64_t block){
    need(block>0&&block<blocks_,"ext block outside volume");
    return read(block*block_size_,block_size_);
}
ExtVolume::Inode ExtVolume::read_inode(std::uint32_t number){
    need(number>=2&&number<=inode_count_,
         "ext inode number outside table");
    const auto index=std::uint64_t(number-1);
    const auto group=index/ipg_,position=index%ipg_;
    need(group<groups_,"ext inode group outside descriptors");
    const auto gd=read(descriptor_offset_+group*desc_size_,desc_size_);
    std::uint64_t table=u32(gd.data()+8);
    if(feature64_){
        need(gd.size()>=64,"truncated 64-bit ext descriptor");
        table|=std::uint64_t(u32(gd.data()+40))<<32;
    }
    need(table>0&&table<blocks_,"ext inode table outside volume");
    const auto at=table*block_size_+position*inode_size_;
    const auto b=read(at,inode_size_);
    Inode n;
    n.mode=u16(b.data());n.flags=u32(b.data()+32);
    n.size=u32(b.data()+4);
    if((n.mode&0xf000)==0x8000)
        n.size|=std::uint64_t(u32(b.data()+108))<<32;
    std::copy_n(b.data()+40,60,n.mapping.begin());
    need(!(n.flags&(0x00000004u|0x00000800u|
                    0x00100000u|0x10000000u|0x40000000u)),
         "compressed/encrypted/verity/inline/casefold inode unsupported");
    return n;
}
void ExtVolume::extent_tree(const std::uint8_t* node,
    std::size_t length,unsigned expected_depth,
    std::vector<Extent>& result,std::set<std::uint64_t>& visited){
    need(length>=12&&u16(node)==0xf30a,
         "invalid ext extent header");
    const auto entries=u16(node+2),maximum=u16(node+4);
    const auto depth=u16(node+6);
    need(depth==expected_depth&&depth<=5&&
         entries<=maximum&&maximum<=(length-12)/12&&
         result.size()+entries<=kMaxItems,
         "invalid ext extent tree dimensions");
    std::uint32_t prior=0;
    for(unsigned i=0;i<entries;++i){
        const auto* e=node+12+12*i;
        const auto logical=u32(e);
        need(i==0||logical>prior,
             "unordered ext extent entries");
        prior=logical;
        if(depth==0){
            const auto rawlen=u16(e+4);
            const bool uninit=rawlen>32768;
            const auto blocks=uninit?unsigned(rawlen-32768):
                                     unsigned(rawlen);
            const std::uint64_t physical=
                (std::uint64_t(u16(e+6))<<32)|u32(e+8);
            need(blocks>0&&std::uint64_t(logical)+blocks<=kMaxBlocks,
                 "invalid ext extent length");
            if(!uninit)
                need(physical>0&&physical<blocks_&&
                     blocks<=blocks_-physical,
                     "ext extent escapes filesystem");
            result.push_back({logical,blocks,physical,uninit});
        }else{
            const std::uint64_t location=
                (std::uint64_t(u16(e+8))<<32)|u32(e+4);
            need(visited.insert(location).second,
                 "cyclic ext extent index");
            const auto next=read_block(location);
            extent_tree(next.data(),next.size(),depth-1,result,visited);
        }
    }
}
std::vector<ExtVolume::Extent> ExtVolume::extents(const Inode& inode){
    std::vector<Extent> result;
    if((inode.flags&0x80000u)==0)return result;
    std::set<std::uint64_t> visited;
    const auto depth=u16(inode.mapping.data()+6);
    extent_tree(inode.mapping.data(),inode.mapping.size(),
                depth,result,visited);
    std::sort(result.begin(),result.end(),
        [](const auto&a,const auto&b){return a.logical<b.logical;});
    for(std::size_t i=1;i<result.size();++i)
        need(result[i-1].logical+result[i-1].blocks<=
             result[i].logical,"overlapping ext extents");
    return result;
}
std::uint64_t ExtVolume::indirect(std::uint64_t block,
    std::uint64_t index,unsigned depth){
    if(!block)return 0;
    const auto n=block_size_/4;
    std::uint64_t span=1;
    for(unsigned i=1;i<depth;++i)span*=n;
    const auto pos=index/span;
    need(pos<n,"ext indirect pointer index outside block");
    const auto b=read_block(block);
    const auto ptr=u32(b.data()+4*pos);
    if(!ptr)return 0;
    if(depth==1){
        need(ptr<blocks_,"ext data block exceeds filesystem");
        return ptr;
    }
    return indirect(ptr,index%span,depth-1);
}
std::uint64_t ExtVolume::mapped_block(const Inode& inode,
    std::uint64_t index,const std::vector<Extent>& tree){
    if(inode.flags&0x80000u){
        auto it=std::upper_bound(tree.begin(),tree.end(),index,
            [](std::uint64_t block,const Extent& e){
                return block<e.logical;
            });
        if(it==tree.begin())return 0;
        --it;
        if(index>=it->logical+it->blocks||it->uninitialized)return 0;
        return it->physical+(index-it->logical);
    }
    const auto* p=inode.mapping.data();
    const auto ptrs=block_size_/4;
    if(index<12){
        const auto b=u32(p+4*index);
        if(b)need(b<blocks_,"invalid ext direct block");
        return b;
    }
    index-=12;
    if(index<ptrs)return indirect(u32(p+48),index,1);
    index-=ptrs;
    const auto double_span=std::uint64_t(ptrs)*ptrs;
    if(index<double_span)return indirect(u32(p+52),index,2);
    index-=double_span;
    need(index<double_span*ptrs,"ext file exceeds indirect block tree");
    return indirect(u32(p+56),index,3);
}
std::vector<std::uint8_t> ExtVolume::directory(const Inode& inode){
    need((inode.mode&0xf000)==0x4000&&
         inode.size>0&&inode.size<=kMaxDirBytes,
         "ext directory invalid or over 8 MiB");
    const auto tree=extents(inode);
    std::vector<std::uint8_t> result;
    result.reserve(static_cast<std::size_t>(inode.size));
    const auto n=(inode.size+block_size_-1)/block_size_;
    for(std::uint64_t i=0;i<n;++i){
        const auto ptr=mapped_block(inode,i,tree);
        need(ptr!=0,"ext directory contains sparse/uninitialized block");
        const auto data=read_block(ptr);
        const auto count=std::min<std::uint64_t>(
            block_size_,inode.size-result.size());
        result.insert(result.end(),data.begin(),
            data.begin()+static_cast<std::ptrdiff_t>(count));
    }
    return result;
}
void ExtVolume::safe_name(std::string_view s){
    need(!s.empty()&&s.size()<=255&&s!="."&&s!=".."&&
         s.back()!=' '&&s.back()!='.'&&valid_utf8(s),
         "unsafe ext filename");
    for(unsigned char c:s)
        need(c>=32&&c!=127&&c!='/'&&c!='\\'&&
             c!=':'&&c!='*'&&c!='?'&&c!='"'&&
             c!='<'&&c!='>'&&c!='|',
             "unsafe ext filename character");
    std::string base(s.substr(0,s.find('.')));
    for(char& c:base)if(c>='a'&&c<='z')c=char(c-'a'+'A');
    need(base!="CON"&&base!="PRN"&&base!="AUX"&&
         base!="NUL"&&base!="CLOCK$",
         "reserved ext Windows name");
    if(base.size()==4&&
       (base.starts_with("COM")||base.starts_with("LPT")))
        need(base[3]<'1'||base[3]>'9',
             "reserved ext Windows port name");
}
void ExtVolume::walk(std::uint32_t number,
    const std::string& prefix,unsigned depth){
    need(depth<=32,"ext directory nesting exceeds limit");
    need(visited_inodes_.insert(number).second,"ext directory cycle");
    const auto inode=read_inode(number);
    const auto bytes=directory(inode);
    std::vector<std::pair<std::uint32_t,std::string>> children;
    for(std::size_t at=0;at+8<=bytes.size();) {
        const auto* p=bytes.data()+at;
        const auto next=u16(p+4);
        const auto len=p[6];
        const auto child=u32(p);
        need(next>=8&&(next&3)==0&&
             next<=bytes.size()-at&&len<=next-8,
             "invalid ext directory record");
        if(child&&len){
            const std::string name(
                reinterpret_cast<const char*>(p+8),len);
            if(name!="."&&name!=".."){
                safe_name(name);
                const auto value=read_inode(child);
                const auto kind=value.mode&0xf000;
                if(kind==0x4000||kind==0x8000){
                    const auto path=prefix.empty()?name:prefix+"/"+name;
                    need(path.size()<=1000&&files_.size()<kMaxItems&&
                         !nodes_.count(path),
                         "duplicate/excessive ext entries");
                    VolumeFile info;
                    info.path=path;
                    info.directory=kind==0x4000;
                    info.size=info.directory?0:value.size;
                    info.attributes=info.directory?0x10:0x20;
                    files_.push_back(info);
                    nodes_.emplace(path,FileNode{info,child});
                    if(info.directory)
                        children.emplace_back(child,path);
                }
            }
        }
        at+=next;
    }
    for(const auto& child:children)
        walk(child.first,child.second,depth+1);
}
void ExtVolume::scan_once(){
    if(scanned_)return;
    walk(2,"",0);
    scanned_=true;
}
std::vector<VolumeFile> ExtVolume::list(){
    scan_once();return files_;
}
void ExtVolume::extract(std::string_view path,std::ostream& output){
    scan_once();
    auto it=nodes_.find(std::string(path));
    need(it!=nodes_.end()&&!it->second.info.directory,
         "unknown ext regular file");
    const auto inode=read_inode(it->second.inode);
    need((inode.mode&0xf000)==0x8000,
         "ext file inode type changed");
    const auto tree=extents(inode);
    std::array<char,65536> zeros{};
    std::uint64_t remaining=inode.size;
    for(std::uint64_t i=0;remaining;++i){
        const auto count=std::min<std::uint64_t>(
            remaining,block_size_);
        const auto phys=mapped_block(inode,i,tree);
        if(phys){
            const auto block=read_block(phys);
            output.write(reinterpret_cast<const char*>(block.data()),
                         static_cast<std::streamsize>(count));
        } else {
            output.write(zeros.data(),static_cast<std::streamsize>(count));
        }
        need(bool(output),"ext extraction cancelled/output failed");
        remaining-=count;
    }
}
} // namespace iso_wcx
