#include "iso_wcx/legacy_formats.hpp"
#include "iso_wcx/disc.hpp"
#include "iso_wcx/rock_ridge.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Bytes=std::vector<std::uint8_t>;

void expect(bool ok,const char* reason) {
    if(!ok)throw std::runtime_error(reason);
}
void invalid(const Bytes& record,std::uint8_t skip=0) {
    bool caught=false,called=false;
    try {
        (void)iso_wcx::rock_ridge_entry(record,skip,
           [&](std::uint64_t,std::size_t)->Bytes {
               called=true;
               return Bytes{}; // any callback call is a test failure
           });
    }catch(const std::exception&){caught=true;}
    expect(caught && !called,"malformed Rock Ridge SUSP entry accepted or followed");
}
void lebe32(Bytes& b,std::size_t p,std::uint32_t v) {
    for(unsigned i=0;i<4;++i){
        b[p+i]=static_cast<std::uint8_t>(v>>(8*i));
        b[p+4+i]=static_cast<std::uint8_t>(v>>(24-8*i));
    }
}
Bytes entry(char a,char b,const Bytes& payload) {
    expect(payload.size()<=251,"SUSP entry too large");
    Bytes bfr{static_cast<std::uint8_t>(a),static_cast<std::uint8_t>(b),
              static_cast<std::uint8_t>(payload.size()+4),1};
    bfr.insert(bfr.end(),payload.begin(),payload.end());
    return bfr;
}
Bytes record(const Bytes& susp) {
    Bytes b(34,0);
    b[32]=1;b[33]='A';  // A one-byte ISO filename, no padding.
    b.insert(b.end(),susp.begin(),susp.end());
    expect(b.size()<=255,"ISO directory record exceeds 255 bytes");
    b[0]=static_cast<std::uint8_t>(b.size());
    return b;
}
Bytes px(std::uint32_t mode) {
    Bytes p(40,0);
    lebe32(p,0,mode);
    lebe32(p,8,1); // nlinks
    lebe32(p,16,42); // uid
    lebe32(p,24,24); // gid
    lebe32(p,32,7); // serial number
    return entry('P','X',p);
}
Bytes tf_short() {
    Bytes p{2,126,10,23,12,34,58,0};
    return entry('T','F',p);
}
Bytes tf_long() {
    Bytes p{0x82};
    constexpr char stamp[]="2026102312345800";
    for(unsigned i=0;i<16;++i)p.push_back(static_cast<std::uint8_t>(stamp[i]));
    p.push_back(0); // UTC offset in signed 15-minute units
    return entry('T','F',p);
}
Bytes concat(const Bytes& a,const Bytes& b) {
    Bytes result=a;result.insert(result.end(),b.begin(),b.end());return result;
}
Bytes ce(std::uint32_t block,std::uint32_t offset,std::uint32_t length) {
    Bytes p(24);
    lebe32(p,0,block);
    lebe32(p,8,offset);
    lebe32(p,16,length);
    return entry('C','E',p);
}
iso_wcx::RockRidgeEntry parse(const Bytes& rec) {
    return iso_wcx::rock_ridge_entry(rec,0,
        [](std::uint64_t,std::size_t)->Bytes {
            throw std::runtime_error("unexpected CE");
        });
}

void iso_record(Bytes& image,std::size_t pos,std::uint32_t lba,
                std::uint32_t size,std::uint8_t flags,const Bytes& name,
                const Bytes& susp = {}) {
    const auto length=33u+name.size()+(name.size()%2u==0 ? 1u : 0u)+susp.size();
    expect(length<=255 && pos<=image.size() && length<=image.size()-pos,
           "synthetic RRIP directory record exceeds image");
    auto* r=image.data()+pos;
    r[0]=static_cast<std::uint8_t>(length);
    for(unsigned j=0;j<4;++j) {
        r[2+j]=static_cast<std::uint8_t>(lba>>(j*8));
        r[6+j]=static_cast<std::uint8_t>(lba>>(24-j*8));
        r[10+j]=static_cast<std::uint8_t>(size>>(j*8));
        r[14+j]=static_cast<std::uint8_t>(size>>(24-j*8));
    }
    r[25]=flags;
    r[28]=1;r[31]=1;
    r[32]=static_cast<std::uint8_t>(name.size());
    std::copy(name.begin(),name.end(),r+33);
    std::copy(susp.begin(),susp.end(),r+33+name.size()+
              (name.size()%2u==0 ? 1u : 0u));
}
void disc_integration() {
    constexpr std::size_t sector=2048;
    Bytes image(36*sector,0);
    const auto descriptor=16*sector;
    image[descriptor]=1;
    std::copy_n("CD001",5,image.begin()+descriptor+1);
    image[descriptor+6]=1;
    iso_record(image,descriptor+156,20,sector,2,{0});
    const auto terminal=17*sector;
    image[terminal]=255;
    std::copy_n("CD001",5,image.begin()+terminal+1);
    image[terminal+6]=1;
    const auto sp=entry('S','P',{0xbe,0xef,0});
    Bytes er{10,0,0,1};
    constexpr char rr_id[]="RRIP_1991A";
    er.insert(er.end(),rr_id,rr_id+10);
    const auto root_susp=concat(sp,entry('E','R',er));
    std::size_t pos=20*sector;
    iso_record(image,pos,20,sector,2,{0},root_susp);
    pos+=image[pos];
    iso_record(image,pos,20,sector,2,{1});
    pos+=image[pos];
    Bytes nm{0};
    constexpr char rr_name[]="readme-linux.txt";
    nm.insert(nm.end(),rr_name,rr_name+sizeof(rr_name)-1);
    auto file_susp=concat(entry('N','M',nm),concat(px(0100444u),tf_short()));
    const std::string file_name="README.TXT;1";
    iso_record(image,pos,21,5,0,Bytes(file_name.begin(),file_name.end()),file_susp);
    pos+=image[pos];
    // A device node must not be exposed as a file that WCX might extract.
    iso_record(image,pos,21,5,0,Bytes{'D','E','V','I','C','E','.','T','X','T',';','1'},
               concat(entry('N','M',{0,'d','e','v','i','c','e'}),px(0020666u)));
    std::copy_n("world",5,image.begin()+21*sector);
    const auto file=std::filesystem::temp_directory_path()/
        ("iso-wcx-rrip-"+std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count())+".iso");
    struct Cleanup {
        std::filesystem::path file;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove(file,ec);
        }
    } cleanup{file};
    {
        std::ofstream out(file,std::ios::binary|std::ios::trunc);
        expect(bool(out),"cannot create synthetic RRIP image");
        out.write(reinterpret_cast<const char*>(image.data()),
                  static_cast<std::streamsize>(image.size()));
        expect(bool(out),"failed to write synthetic RRIP image");
    }
    const iso_wcx::Disc disc(file);
    expect(disc.format().find("Rock Ridge")!=std::string::npos,
           "RRIP not detected in integration image");
    const auto& items=disc.items();
    expect(items.size()==1 && items[0].path=="readme-linux.txt" &&
           items[0].size==5 && !items[0].directory,
           "RRIP PX special file was not excluded or NM not decoded");
    expect((items[0].attributes & 0x21u)==0x21u,
           "RRIP PX non-writable mode not mapped to DOS readonly");
    const auto stamp=iso_wcx::iso_dos_timestamp(
        std::array<std::uint8_t,7>{126,10,23,12,34,58,0});
    expect(items[0].dos_time==stamp,
           "RRIP TF modification date not passed into WCX Item");
}
}

int main() {
    try {
        const auto read_only=px(0100444u);
        auto r=parse(record(concat(read_only,tf_short())));
        expect(r.posix_mode==0100444u,"missing RRIP PX POSIX mode");
        expect(r.modification_dos_time==
               iso_wcx::iso_dos_timestamp({std::array<std::uint8_t,7>{
                   126,10,23,12,34,58,0}}),"RRIP short TF timestamp mismatch");

        r=parse(record(concat(px(0100644u),tf_long())));
        expect(r.posix_mode==0100644u,"RRIP writable mode lost");
        expect(r.modification_dos_time==
               iso_wcx::iso_long_dos_timestamp({std::array<std::uint8_t,17>{
                   '2','0','2','6','1','0','2','3','1','2','3','4','5','8','0','0',0}}),
               "RRIP long TF timestamp mismatch");
        expect(r.modification_dos_time.has_value(),
               "RRIP TF long modification date missing");
        r=parse(record(px(0120777u)));
        expect(((*r.posix_mode)&0170000u)==0120000u,
               "POSIX special-file mode not available to caller");

        auto corrupt=read_only;
        corrupt[8]^=1; // corrupt BE half of mode integer
        invalid(record(corrupt));
        corrupt=read_only;
        corrupt[20]^=1; // corrupt mirrored nlinks/uid field
        invalid(record(corrupt));
        corrupt=read_only;
        corrupt.pop_back();
        corrupt[2]=static_cast<std::uint8_t>(corrupt.size());
        invalid(record(corrupt));
        invalid(record(concat(read_only,read_only)));
        invalid(record(concat(tf_short(),tf_short())));
        corrupt=tf_short();
        corrupt[2]=static_cast<std::uint8_t>(corrupt.size()-1);
        invalid(record(corrupt));

        // The CE carries 3 mirrored little/big endian integers. Test both a
        // valid reference and the previously unverified BE halves.
        const auto continuation=tf_short();
        const auto source=ce(31,8,static_cast<std::uint32_t>(continuation.size()));
        auto from_ce=iso_wcx::rock_ridge_entry(record(source),0,
            [&](std::uint64_t offset,std::size_t length)->Bytes {
                expect(offset==31u*2048u+8u && length==continuation.size(),
                       "CE logical reference mismatch");
                return continuation;
            });
        expect(from_ce.modification_dos_time==
               iso_wcx::iso_dos_timestamp({std::array<std::uint8_t,7>{
                   126,10,23,12,34,58,0}}),
               "RRIP TF not decoded through continuation");
        corrupt=source;
        corrupt[8]^=1; // mirrored CE block high-endian byte
        invalid(record(corrupt));
        corrupt=source;
        corrupt[16]^=1; // mirrored CE offset high-endian byte
        invalid(record(corrupt));
        corrupt=source;
        corrupt[24]^=1; // mirrored CE length high-endian byte
        invalid(record(corrupt));


        // Unknown TF fields are accepted only when all advertised bytes exist.
        auto malformed_long=tf_long();
        malformed_long[2]=5; // declares too-short TF descriptor
        invalid(record(malformed_long));
        const auto invalid_calendar=std::array<std::uint8_t,17>{
            '2','0','2','6','0','2','3','0','1','2','3','4','5','8','0','0',0};
        expect(iso_wcx::iso_long_dos_timestamp(invalid_calendar)==0,
               "invalid RRIP 17-byte date accepted");
        auto invalid_timezone=std::array<std::uint8_t,17>{
            '2','0','2','6','1','0','2','3','1','2','3','4','5','8','0','0',127};
        expect(iso_wcx::iso_long_dos_timestamp(invalid_timezone)==0,
               "out of range ISO timezone accepted");
        disc_integration();
        std::cout<<"PASS: RRIP PX mirrored metadata, TF 7/17-byte timestamps and mirrored CE validation\n";
        return 0;
    }catch(const std::exception& error) {
        std::cerr<<"RRIP metadata failure: "<<error.what()<<"\n";
        return 1;
    }
}
