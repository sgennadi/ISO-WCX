#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {
constexpr uint64_t sector = 2048;
using Bytes = std::vector<uint8_t>;
uint32_t le32(const Bytes& b, size_t p) {
    if (p > b.size() || b.size()-p < 4) throw std::runtime_error("truncated integer");
    return uint32_t(b[p]) | (uint32_t(b[p+1])<<8) | (uint32_t(b[p+2])<<16) | (uint32_t(b[p+3])<<24);
}
uint16_t be16(const Bytes& b, size_t p) {
    if (p > b.size() || b.size()-p < 2) throw std::runtime_error("truncated UCS-2");
    return uint16_t((uint16_t(b[p])<<8) | b[p+1]);
}
std::string utf8(uint32_t c) {
    if(c<=0x7f) return std::string(1,char(c));
    if(c<=0x7ff) return {char(0xc0|(c>>6)),char(0x80|(c&63))};
    if(c<=0xffff) return {char(0xe0|(c>>12)),char(0x80|((c>>6)&63)),char(0x80|(c&63))};
    return {char(0xf0|(c>>18)),char(0x80|((c>>12)&63)),char(0x80|((c>>6)&63)),char(0x80|(c&63))};
}
std::string name(const Bytes& b,size_t pos,size_t len,bool joliet) {
    if(pos>b.size()||len>b.size()-pos) throw std::runtime_error("invalid name");
    std::string s;
    if(joliet) {
        if(len%2) throw std::runtime_error("odd UCS-2 length");
        for(size_t i=0;i<len;i+=2) {
            uint32_t c=be16(b,pos+i);
            if(c>=0xd800&&c<=0xdbff) {
                if(i+4>len) throw std::runtime_error("invalid surrogate");
                uint32_t low=be16(b,pos+i+2);
                if(low<0xdc00||low>0xdfff) throw std::runtime_error("invalid surrogate");
                c=0x10000+((c-0xd800)<<10)+(low-0xdc00);i+=2;
            } else if(c>=0xdc00&&c<=0xdfff) throw std::runtime_error("invalid surrogate");
            s+=utf8(c);
        }
    } else s.assign(reinterpret_cast<const char*>(b.data()+pos),len);
    auto semicolon=s.find(';');
    if(semicolon!=std::string::npos) s.erase(semicolon);
    while(!s.empty()&&s.back()=='.') s.pop_back();
    if(s.empty()||s=="."||s==".."||s.find('/')!=std::string::npos||s.find('\\')!=std::string::npos||
       s.find(':')!=std::string::npos||s.find('\0')!=std::string::npos)
        throw std::runtime_error("unsafe filename");
    return s;
}
class Image {
    std::ifstream file;
    uint64_t length;
public:
    explicit Image(const fs::path& p):file(p,std::ios::binary|std::ios::ate) {
        if(!file) throw std::runtime_error("cannot open image");
        auto end=file.tellg();
        if(end<0) throw std::runtime_error("cannot size image");
        length=static_cast<uint64_t>(end);
    }
    Bytes read(uint64_t off,size_t count) {
        if(off>length||count>length-off||
            off>static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()))
            throw std::runtime_error("extent outside image");
        Bytes data(count);
        file.clear();file.seekg(static_cast<std::streamoff>(off));
        if(!file||!file.read(reinterpret_cast<char*>(data.data()),static_cast<std::streamsize>(count)))
            throw std::runtime_error("cannot read image");
        return data;
    }
    uint64_t size() const {return length;}
};
struct Record { uint32_t lba;uint32_t size;bool directory;std::string name; };
Record parse(const Bytes& b,size_t p,bool joliet) {
    if(p>=b.size()||b[p]<34||b[p]>b.size()-p) throw std::runtime_error("invalid directory record");
    uint8_t len=b[p], n=b[p+32];
    if(size_t(33)+n>len) throw std::runtime_error("invalid filename field");
    return {le32(b,p+2),le32(b,p+10),bool(b[p+25]&2), (n==1 && (b[p+33]==0 || b[p+33]==1)) ? std::string(".") : name(b,p+33,n,joliet)};
}
void extract(Image& im,uint64_t off,uint32_t size,const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    if(!out) throw std::runtime_error("cannot create output file");
    while(size) {
        size_t n=std::min<uint32_t>(size,1024*1024);
        auto block=im.read(off,n);
        out.write(reinterpret_cast<const char*>(block.data()),static_cast<std::streamsize>(n));
        if(!out) throw std::runtime_error("output write failure");
        off+=n;size-=static_cast<uint32_t>(n);
    }
}
void walk(Image& im,const Record& dir,bool joliet,const std::string& prefix,
          const fs::path* destination,unsigned depth,uint64_t& count) {
    if(depth>=64) throw std::runtime_error("directory depth exceeded");
    if(dir.size>128*1024*1024) throw std::runtime_error("oversized directory");
    auto content=im.read(uint64_t(dir.lba)*sector,dir.size);
    for(size_t p=0;p<content.size();) {
        if(!content[p]) {
            size_t next=((p/sector)+1)*sector;
            if(next<=p) throw std::runtime_error("record overflow");
            p=next;continue;
        }
        size_t len=content[p];
        if(len<34||len>content.size()-p||(p%sector)+len>sector)
            throw std::runtime_error("record crosses sector");
        uint8_t n=content[p+32];
        if(n==1&&(content[p+33]==0||content[p+33]==1)){p+=len;continue;}
        Record item=parse(content,p,joliet);
        std::string rel=prefix+item.name;
        std::cout<<(item.directory?"DIR  ":"FILE ")<<item.size<<" "<<rel<<"\n";
        if(++count>1000000) throw std::runtime_error("too many directory records");
        if(item.directory) {
            if(destination) fs::create_directories(*destination/fs::u8path(rel));
            walk(im,item,joliet,rel+"/",destination,depth+1,count);
        } else if(destination) extract(im,uint64_t(item.lba)*sector,item.size,*destination/fs::u8path(rel));
        p+=len;
    }
}
}
int main(int argc,char** argv) {
    if(argc!=2&&argc!=4) {std::cerr<<"Usage: iso9660_reader IMAGE.iso [--extract OUTPUT]\n";return 1;}
    if(argc==4&&std::string(argv[2])!="--extract") {std::cerr<<"unknown option\n";return 1;}
    try {
        Image im(fs::u8path(argv[1]));
        Bytes selected;bool joliet=false;
        for(uint64_t i=16;i<16+256;++i) {
            Bytes d=im.read(i*sector,sector);
            if(std::string(reinterpret_cast<const char*>(d.data()+1),5)!="CD001"||d[6]!=1)
                throw std::runtime_error("not ISO9660");
            if(d[0]==2&&d[88]=='%'&&d[89]=='/'&&(d[90]=='@'||d[90]=='C'||d[90]=='E')) {
                selected=d;joliet=true;
            } else if(d[0]==1&&!joliet) selected=d;
            if(d[0]==255) break;
        }
        if(selected.empty()) throw std::runtime_error("no primary volume descriptor");
        Record root=parse(selected,156,joliet);
        if(!root.directory) throw std::runtime_error("root is not a directory");
        uint64_t count=0;
        fs::path output;
        const fs::path* dest=nullptr;
        if(argc==4) {output=fs::u8path(argv[3]);fs::create_directories(output);dest=&output;}
        walk(im,root,joliet,"",dest,0,count);
    } catch(const std::exception& ex) {std::cerr<<"ISO error: "<<ex.what()<<"\n";return 2;}
    return 0;
}
