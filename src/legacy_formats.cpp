// Independent implementation of public El Torito and XDVDFS on-disk formats.
// No original ISO.WCX implementation or licensing-restricted source is used.
#include "iso_wcx/legacy_formats.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace iso_wcx {
namespace {
constexpr std::uint64_t sector = 2048;
constexpr std::size_t max_xbox_directory = 32u*1024u*1024u;
constexpr std::size_t max_entries = 100000;
void check(bool condition, const char* why) {
    if(!condition) throw std::runtime_error(why);
}
std::uint16_t le16(std::span<const std::uint8_t> b,std::size_t at) {
    check(at<=b.size() && b.size()-at>=2,"truncated descriptor word");
    return std::uint16_t(b[at]) | (std::uint16_t(b[at+1])<<8);
}
std::uint32_t le32(std::span<const std::uint8_t> b,std::size_t at) {
    check(at<=b.size() && b.size()-at>=4,"truncated descriptor dword");
    return std::uint32_t(le16(b,at)) | (std::uint32_t(le16(b,at+2))<<16);
}
void check_extent(std::uint64_t start,std::uint64_t length,std::uint64_t size) {
    check(start<=size && length<=size-start,"disc extent lies outside image");
}
std::uint32_t pack_dos(unsigned year,unsigned month,unsigned day,
                       unsigned hour,unsigned minute,unsigned second) {
    if(year<1980 || year>2107 || month<1 || month>12 || day<1 || day>31 ||
       hour>23 || minute>59 || second>59) return 0;
    using namespace std::chrono;
    const year_month_day ymd{std::chrono::year{static_cast<int>(year)},
                             std::chrono::month{month},std::chrono::day{day}};
    if(!ymd.ok())return 0;
    return ((year-1980)<<25) | (month<<21) | (day<<16) |
           (hour<<11) | (minute<<5) | (second/2);
}
void validate_name(std::string_view name) {
    check(!name.empty() && name!="." && name!=".." && name.size()<=240,
          "unsafe Xbox entry name");
    check(name.back()!=' ' && name.back()!='.',"unsafe Xbox trailing name");
    for(const unsigned char c:name) {
        check(c>=32 && c!=127 && c!='/' && c!='\\' && c!=':' && c!='*' &&
              c!='?' && c!='\"' && c!='<' && c!='>' && c!='|',"unsafe Xbox filename");
    }
    auto p=name.find('.');
    std::string base(name.substr(0,p));
    for(char& c:base)if(c>='a'&&c<='z')c=char(c-'a'+'A');
    check(base!="CON" && base!="PRN" && base!="AUX" && base!="NUL" && base!="CLOCK$",
          "reserved device name");
    if(base.size()==4 && (base.substr(0,3)=="COM"||base.substr(0,3)=="LPT"))
        check(base[3]<'1'||base[3]>'9',"reserved device filename");
}
std::string xbox_name(std::span<const std::uint8_t> b) {
    // XDVDFS names are commonly Windows-1252. Convert all byte values to
    // valid UTF-8 instead of passing an invalid native 8-bit path to WCX.
    static constexpr std::array<std::uint16_t,32> cp1252 = {
        0x20ac,0x0081,0x201a,0x0192,0x201e,0x2026,0x2020,0x2021,
        0x02c6,0x2030,0x0160,0x2039,0x0152,0x008d,0x017d,0x008f,
        0x0090,0x2018,0x2019,0x201c,0x201d,0x2022,0x2013,0x2014,
        0x02dc,0x2122,0x0161,0x203a,0x0153,0x009d,0x017e,0x0178};
    std::string result;
    for(std::uint8_t byte:b) {
        auto cp=byte>=128 && byte<160?cp1252[byte-128]:std::uint16_t(byte);
        check(cp>=32 && cp!=127 && (cp<128 || cp>=160),"control byte in Xbox name");
        if(cp<128)result+=char(cp);
        else if(cp<2048) {result+=char(0xc0|(cp>>6));result+=char(0x80|(cp&63));}
        else {result+=char(0xe0|(cp>>12));result+=char(0x80|((cp>>6)&63));result+=char(0x80|(cp&63));}
    }
    validate_name(result);
    return result;
}
}

std::uint32_t iso_dos_timestamp(std::span<const std::uint8_t> b) {
    if(b.size()<7)return 0;
    // ISO records store a local calendar date and a 15-minute GMT offset.
    // WCX's DOS date has no timezone field; preserve the written local date.
    return pack_dos(1900u+b[0],b[1],b[2],b[3],b[4],b[5]);
}
std::uint32_t iso_long_dos_timestamp(std::span<const std::uint8_t> b) {
    if(b.size()!=17)return 0;
    const auto digits=[&](std::size_t pos,unsigned count)->unsigned {
        unsigned result=0;
        for(unsigned i=0;i<count;++i) {
            const auto c=b[pos+i];
            if(c<'0' || c>'9')return 100000; // Invalid ASCII digit
            result=result*10+(c-'0');
        }
        return result;
    };
    const auto year=digits(0,4),month=digits(4,2),day=digits(6,2);
    const auto hour=digits(8,2),minute=digits(10,2),second=digits(12,2);
    const auto centiseconds=digits(14,2);
    const auto zone=static_cast<std::int8_t>(b[16]);
    if(centiseconds>99 || zone<-48 || zone>52)return 0;
    // The WCX DOS timestamp represents the recorded *local* calendar time,
    // so no timezone conversion is applied (same as the short ISO date).
    return pack_dos(year,month,day,hour,minute,second);
}
std::uint32_t udf_dos_timestamp(std::span<const std::uint8_t> b) {
    if(b.size()<12)return 0;
    // ECMA-167 timestamp: TypeAndTimezone, year, month, day, hour, minute,
    // seconds, centiseconds, hundreds-of-microseconds, microseconds.
    const auto type_and_zone=le16(b,0);
    if((type_and_zone>>12)>1)return 0;
    return pack_dos(le16(b,2),b[4],b[5],b[6],b[7],b[8]);
}
std::uint32_t xbox_filetime_dos(std::uint64_t ticks) {
    if(ticks<116444736000000000ULL)return 0;
    const std::uint64_t sec=ticks/10000000ULL-11644473600ULL;
    if(sec>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))return 0;
    using namespace std::chrono;
    const auto time=sys_seconds{seconds{static_cast<std::int64_t>(sec)}};
    const auto day=floor<days>(time);
    const year_month_day ymd{day};
    if(!ymd.ok())return 0;
    const auto tod=hh_mm_ss{time-day};
    return pack_dos(unsigned(int(ymd.year())),unsigned(ymd.month()),unsigned(ymd.day()),
                    unsigned(tod.hours().count()),unsigned(tod.minutes().count()),
                    unsigned(tod.seconds().count()));
}

std::vector<Item> el_torito_images(std::span<const std::uint8_t> boot_record,
                                   std::uint64_t image_bytes,const SectorReader& reader,
                                   std::uint32_t absolute_session_start) {
    std::vector<Item> files;
    if(boot_record.size()<75 || boot_record[0]!=0 ||
       std::string_view(reinterpret_cast<const char*>(boot_record.data()+1),5)!="CD001" ||
       boot_record[6]!=1 ||
       std::string_view(reinterpret_cast<const char*>(boot_record.data()+7),23)!="EL TORITO SPECIFICATION")
        return files;
    auto catalog_lba=le32(boot_record,71);
    if(catalog_lba==0)return files;
    // For a later session, catalogs may use absolute image LBAs.
    auto normalize=[&](std::uint32_t lba)->std::uint64_t {
        return absolute_session_start && lba>=absolute_session_start?
               static_cast<std::uint64_t>(lba-absolute_session_start):lba;
    };
    auto at=normalize(catalog_lba)*sector;
    check_extent(at,sector,image_bytes);
    const auto catalog=reader(at,sector);
    check(catalog.size()==sector,"incomplete El Torito catalog");
    if(catalog[0]!=1 || catalog[30]!=0x55 || catalog[31]!=0xaa)return files;
    unsigned checksum=0;
    for(std::size_t i=0;i<32;i+=2)checksum+=le16(catalog,i);
    if((checksum&0xffff)!=0)return files;
    unsigned platform=catalog[1];
    auto add=[&](std::size_t p,unsigned index) {
        check(p+32<=catalog.size(),"truncated boot image entry");
        if(catalog[p]!=0x88)return;
        const auto medium=catalog[p+1]&0x0f;
        if(medium>4)return;
        const auto lba=le32(catalog,p+8);const auto load_count=le16(catalog,p+6);
        if(lba==0 || !load_count)return;
        std::uint64_t bytes=std::uint64_t(load_count)*512;
        if(medium==1)bytes=1200*1024;
        if(medium==2)bytes=1440*1024;
        if(medium==3)bytes=2880*1024;
        const auto start=normalize(lba)*sector;
        check_extent(start,bytes,image_bytes);
        // El Torito no-emulation SectorCount describes only the BIOS boot-load
        // prefix. For EFI images with an actual FAT boot sector, the validated
        // BPB provides the full EFI System Partition image size.
        if(platform==0xef && medium==0 && image_bytes-start>=512) {
            try {
                const auto bpb=reader(start,512);
                if(bpb.size()==512 && (bpb[0]==0xeb || bpb[0]==0xe9) &&
                   bpb[510]==0x55 && bpb[511]==0xaa) {
                    const auto bytes_per_sector=le16(bpb,11);
                    const auto total16=le16(bpb,19);
                    const auto total=total16?total16:le32(bpb,32);
                    const std::uint64_t candidate=std::uint64_t(bytes_per_sector)*total;
                    if((bytes_per_sector==512 || bytes_per_sector==1024 ||
                        bytes_per_sector==2048 || bytes_per_sector==4096) &&
                       candidate>=bytes && candidate<=512u*1024u*1024u &&
                       candidate<=image_bytes-start)bytes=candidate;
                }
            } catch(const std::exception&) { /* Keep the advertised boot-load length. */ }
        }
        if(files.empty()) {
            Item directory;directory.path="boot.images";directory.directory=true;directory.attributes=0x10;
            files.push_back(std::move(directory));
            Item cat;cat.path="boot.images/boot.catalog";cat.size=sector;cat.attributes=0x20;
            cat.extents.push_back({at,sector,false});files.push_back(std::move(cat));
        }
        std::string platform_name=platform==0xef?"EFI":platform==0?"BIOS":platform==1?"PPC": "Other";
        Item item;item.path="boot.images/"+platform_name+"-"+std::to_string(index)+".img";
        item.size=bytes;item.attributes=0x20;
        item.extents.push_back({start,bytes,false});files.push_back(std::move(item));
    };
    add(32,0);
    unsigned boot_index=1;
    for(std::size_t p=64;p+32<=catalog.size() && boot_index<64;) {
        if(catalog[p]!=0x90 && catalog[p]!=0x91)break;
        const bool last=catalog[p]==0x91;
        platform=catalog[p+1];
        const auto count=le16(catalog,p+2);
        if(count==0 || count>64 || count> (catalog.size()-p-32)/32)break;
        p+=32;
        for(unsigned j=0;j<count;j++,p+=32) {
            if(catalog[p]==0x44)continue; // extension entry; never treated as bootable
            add(p,boot_index++);
        }
        if(last)break;
    }
    return files;
}

std::vector<Item> xbox_xdvdfs(std::uint64_t image_bytes,const SectorReader& reader) {
    std::vector<Item> files;
    constexpr std::string_view signature="MICROSOFT*XBOX*MEDIA";
    const auto start=32*sector;
    if(start>image_bytes || image_bytes-start<sector)return files;
    const auto descriptor=reader(start,sector);
    check(descriptor.size()==sector,"truncated XDVDFS volume descriptor");
    if(std::string_view(reinterpret_cast<const char*>(descriptor.data()),20)!=signature ||
       std::string_view(reinterpret_cast<const char*>(descriptor.data()+sector-20),20)!=signature)
        return files;
    const auto root_lba=le32(descriptor,20),root_size=le32(descriptor,24);
    check(root_size>0 && root_size<=max_xbox_directory,"invalid XDVDFS root size");
    const auto creation=(std::uint64_t(le32(descriptor,32))<<32)|le32(descriptor,28);
    const auto stamp=xbox_filetime_dos(creation);
    std::set<std::pair<std::uint32_t,std::uint32_t>> ancestors;
    auto walk=[&](auto&& self,std::uint32_t lba,std::uint32_t length,const std::string& prefix,
                  unsigned depth)->void {
        check(depth<64,"XDVDFS directory nesting too deep");
        check(length>0 && length<=max_xbox_directory,"invalid XDVDFS directory length");
        check_extent(std::uint64_t(lba)*sector,length,image_bytes);
        const auto key=std::make_pair(lba,length);
        check(ancestors.insert(key).second,"cyclic XDVDFS directories");
        const auto data=reader(std::uint64_t(lba)*sector,length);
        check(data.size()==length,"short XDVDFS directory read");
        std::set<std::uint32_t> seen;
        auto siblings=[&](auto&& visit,std::uint32_t word_offset,unsigned levels)->void {
            check(levels<max_entries,"cyclic XDVDFS sibling tree");
            const auto p=std::uint64_t(word_offset)*4;
            check(p<=data.size() && data.size()-p>=14,"XDVDFS node outside directory");
            check(seen.insert(word_offset).second,"cyclic XDVDFS tree links");
            const auto left=le16(data,static_cast<std::size_t>(p));
            const auto right=le16(data,static_cast<std::size_t>(p)+2);
            const auto child_lba=le32(data,static_cast<std::size_t>(p)+4);
            const auto child_size=le32(data,static_cast<std::size_t>(p)+8);
            const auto attributes=data[static_cast<std::size_t>(p)+12];
            const auto name_length=data[static_cast<std::size_t>(p)+13];
            check(name_length>0 && name_length<=240 && name_length<=data.size()-p-14,
                  "invalid XDVDFS node name length");
            const auto name=xbox_name({data.data()+static_cast<std::size_t>(p)+14,name_length});
            if(left)visit(visit,left,levels+1);
            Item item;item.path=prefix+name;item.directory=bool(attributes&0x10);
            item.size=item.directory?0:child_size;
            item.attributes=attributes;item.dos_time=stamp;
            if(child_size) {
                check_extent(std::uint64_t(child_lba)*sector,child_size,image_bytes);
                if(!item.directory)item.extents.push_back({std::uint64_t(child_lba)*sector,child_size,false});
            }
            check(files.size()<max_entries,"too many XDVDFS files");
            files.push_back(std::move(item));
            if(attributes&0x10) {
                check(child_size>0,"empty XDVDFS subdirectory table");
                self(self,child_lba,child_size,prefix+name+"/",depth+1);
            }
            if(right)visit(visit,right,levels+1);
        };
        siblings(siblings,0,0);
        ancestors.erase(key);
    };
    walk(walk,root_lba,root_size,"",0);
    return files;
}
} // namespace iso_wcx
