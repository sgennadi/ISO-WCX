// Read-only Image Structure report embedded in the WCX DLL.
// The archive parser is the single authority for filesystem entries.
// Optical metadata is probed only in byte-aligned 2048-sector ISO/UDF files;
// other image formats are described without guessing their raw layout.
#include "iso_wcx/image_report.hpp"
#include "iso_wcx/disc.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace iso_wcx {
namespace {
constexpr std::uint64_t sector_size=2048u;
constexpr std::size_t maximum_report_bytes=64u*1024u*1024u;
constexpr std::uint64_t first_descriptors=16, last_descriptors=64;
using Sector=std::array<std::uint8_t,2048>;

std::string safe_text(std::string_view raw) {
    std::string result;
    result.reserve(raw.size());
    constexpr char hex[]="0123456789ABCDEF";
    for(const unsigned char c:raw) {
        if(c=='\n')result+="\\n";
        else if(c=='\r')result+="\\r";
        else if(c=='\t')result+="\\t";
        else if(c<32 || c==127) {
            result+="\\x";
            result+=hex[c>>4];
            result+=hex[c&15];
        }else result+=static_cast<char>(c);
    }
    return result;
}
std::string filename_utf8(const std::filesystem::path& image) {
    const auto name=image.filename().u8string();
    return std::string(name.begin(),name.end());
}
std::uint16_t le16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(
        data[0] | (std::uint16_t(data[1])<<8));
}
std::uint32_t le32(const std::uint8_t* data) {
    return std::uint32_t(data[0]) |
           (std::uint32_t(data[1])<<8) |
           (std::uint32_t(data[2])<<16) |
           (std::uint32_t(data[3])<<24);
}
class Report {
public:
    void append(std::string_view line) {
        if(truncated_)return;
        if(line.size()>=maximum_report_bytes ||
           text_.size()>maximum_report_bytes-line.size()-1) {
            truncated_=true;return;
        }
        text_.append(line);
        text_.push_back('\n');
    }
    void append_kv(std::string_view name,std::string_view value) {
        std::string line(name);
        line.append(": ").append(value);
        append(line);
    }
    std::string finish() {
        if(truncated_)text_+="[TRUNCATED: 64 MiB report size limit]\n";
        return std::move(text_);
    }
    bool truncated() const noexcept {return truncated_;}
private:
    std::string text_;
    bool truncated_=false;
};
bool read_at(std::ifstream& input,std::uint64_t file_bytes,
             std::uint64_t offset,Sector& sector) {
    if(offset>file_bytes || file_bytes-offset<sector.size() ||
       offset>std::uint64_t(std::numeric_limits<std::streamoff>::max()))
        return false;
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset),std::ios::beg);
    if(!input)return false;
    input.read(reinterpret_cast<char*>(sector.data()),
               static_cast<std::streamsize>(sector.size()));
    return bool(input);
}
bool has_disc_signature(const Sector& data) {
    return data[6]==1 &&
           std::equal(data.begin()+1,data.begin()+6,
                      std::string_view("CD001").begin());
}
bool boot_catalog_valid(const Sector& data) {
    if(data[30]!=0x55 || data[31]!=0xaa)return false;
    std::uint32_t sum=0;
    for(std::size_t i=0;i<32;i+=2)
        sum+=le16(data.data()+i);
    return (sum&0xffffu)==0;
}
std::string pvd_volume_id(const Sector& data) {
    const std::string raw(reinterpret_cast<const char*>(data.data()+40),32);
    const auto last=raw.find_last_not_of(' ');
    return safe_text(last==std::string::npos?
                     std::string_view{}:
                     std::string_view(raw).substr(0,last+1));
}
void probe_optical(Report& report,const std::filesystem::path& image,
                   std::uint64_t size) {
    const auto extension=image.extension().wstring();
    std::wstring ext(extension);
    std::transform(ext.begin(),ext.end(),ext.begin(),[](wchar_t c){
        return (c>=L'A'&&c<=L'Z')?wchar_t(c+32):c;
    });
    if(ext!=L".iso" && ext!=L".udf"){
        report.append("Raw ISO9660/UDF sector probe: not applicable to this container");
        return;
    }
    std::ifstream input(image,std::ios::binary);
    if(!input)throw std::runtime_error("cannot open image for read-only metadata probe");
    Sector block{};
    bool nonzero=false;
    std::uint64_t checked=0;
    for(std::uint64_t offset=0;offset<32768u && offset<size;
        offset+=sector_size) {
        if(!read_at(input,size,offset,block))break;
        checked+=sector_size;
        if(std::any_of(block.begin(),block.end(),
                       [](std::uint8_t c){return c!=0;}))
            nonzero=true;
    }
    report.append_kv("System area (first 32 KiB) nonzero",
                     checked==32768u?(nonzero?"yes":"no"):"unknown");
    report.append_kv("2048-byte alignment",
                     size%sector_size==0?"yes":"no");
    report.append("VOLUME DESCRIPTORS (2048-byte sectors 16..63)");
    bool pvd=false,joliet=false,el_torito=false,nsr02=false,nsr03=false;
    std::uint32_t boot_catalog=0;
    for(std::uint64_t lba=first_descriptors;lba<last_descriptors;++lba){
        if(!read_at(input,size,lba*sector_size,block))break;
        const auto udf_id=std::string_view(
            reinterpret_cast<const char*>(block.data()+1),5);
        if(udf_id=="NSR02")nsr02=true;
        if(udf_id=="NSR03")nsr03=true;
        if(!has_disc_signature(block))continue;
        const auto type=block[0];
        if(type==1 && !pvd) {
            pvd=true;
            report.append("LBA "+std::to_string(lba)+" Type=1 Primary Volume Descriptor");
            report.append_kv("PVD Volume ID",pvd_volume_id(block));
            report.append_kv("PVD volume sectors",std::to_string(le32(block.data()+80)));
            report.append_kv("PVD logical block bytes",std::to_string(le16(block.data()+128)));
            report.append_kv("PVD sectors match physical ISO",
                             le32(block.data()+80)==size/sector_size?"yes":"no");
        }else if(type==2 && block[88]=='%' && block[89]=='/' &&
                 (block[90]=='@'||block[90]=='C'||block[90]=='E')) {
            joliet=true;
            report.append("LBA "+std::to_string(lba)+" Type=2 Joliet (UCS-2)");
            report.append_kv("Joliet volume sectors",std::to_string(le32(block.data()+80)));
        }else if(type==0 && !el_torito &&
                 std::string_view(reinterpret_cast<const char*>(block.data()+7),23)==
                     "EL TORITO SPECIFICATION") {
            el_torito=true;
            boot_catalog=le32(block.data()+71);
            report.append("LBA "+std::to_string(lba)+" Type=0 El Torito boot descriptor");
            report.append_kv("El Torito boot catalog LBA",std::to_string(boot_catalog));
        }else if(type==255){
            report.append("LBA "+std::to_string(lba)+" Type=255 Terminator");
        }
    }
    report.append_kv("ISO9660 PVD present",pvd?"yes":"no");
    report.append_kv("Joliet present",joliet?"yes":"no");
    report.append_kv("El Torito boot descriptor",el_torito?"yes":"no");
    report.append_kv("UDF VRS NSR02",nsr02?"detected":"not detected");
    report.append_kv("UDF VRS NSR03",nsr03?"detected":"not detected");
    bool candidate_anchor=false;
    const std::uint64_t sectors=size/sector_size;
    if(sectors>256) {
        std::array<std::uint64_t,3> positions={
            256,sectors-1,sectors-256
        };
        for(const auto lba:positions) {
            if(!read_at(input,size,lba*sector_size,block))continue;
            if(le16(block.data())==2 && le32(block.data()+12)==lba) {
                candidate_anchor=true;
                report.append("UDF candidate AVDP anchor at LBA "+std::to_string(lba));
            }
        }
    }
    report.append_kv("UDF anchor candidates",candidate_anchor?"detected":"not detected");
    if(el_torito && boot_catalog!=0 &&
       read_at(input,size,std::uint64_t(boot_catalog)*sector_size,block))
        report.append_kv("El Torito catalog validation",
                         boot_catalog_valid(block)?"checksum and signature OK":
                                                   "invalid or non-standard");
    report.append("WRITE SAFETY (diagnostic only)");
    if(nonzero)report.append("- Nonzero system area: ISO directory rebuilding may be blocked");
    if(el_torito)report.append("- El Torito: boot catalog and boot image locations must be preserved");
    if(nsr02||nsr03||candidate_anchor)
        report.append("- UDF detected: modifying ISO9660 alone may damage UDF metadata");
    report.append("- This is NOT a write-permission check. Readability does not imply safe deletion");
}

} // namespace

std::string build_image_structure_report(const std::filesystem::path& image){
    const auto bytes=std::filesystem::file_size(image);
    Disc disc(image); // Same parser and Unicode paths as Total Commander.
    Report report;
    report.append("ISO-WCX IMAGE STRUCTURE REPORT 1.0 (READ ONLY)");
    report.append_kv("Image filename",safe_text(filename_utf8(image)));
    report.append_kv("Image size bytes",std::to_string(bytes));
    report.append_kv("Detected filesystem / container",safe_text(disc.format()));
    report.append_kv("Files and directories listed",std::to_string(disc.items().size()));
    report.append("Image payloads copied: no");
    report.append("Image modified: no");
    report.append("");
    probe_optical(report,image,bytes);
    report.append("");
    report.append("DIRECTORY TREE (metadata only; UTF-8)");
    report.append("TYPE\tSIZE_BYTES\tATTR_HEX\tEXTENTS\tSOURCE_OFFSET_BYTES\tPATH");
    std::size_t listed=0;
    constexpr char hex[]="0123456789ABCDEF";
    for(const auto& item:disc.items()) {
        if(report.truncated())break;
        std::string row=item.directory?"DIR":"FILE";
        row+='\t';
        row+=std::to_string(item.size);
        row+="\t0x";
        for(int shift=28;shift>=0;shift-=4)
            row+=hex[(item.attributes>>shift)&15];
        row+='\t';
        row+=std::to_string(item.extents.size());
        row+='\t';
        if(item.extents.size()==1 && !item.extents.front().sparse)
            row+=std::to_string(item.extents.front().offset);
        else if(!item.inline_data.empty())row+="INLINE";
        else if(item.chd_audio_track)row+="SYNTHESIZED";
        else row+="-";
        row+='\t';
        row+=safe_text(item.path);
        report.append(row);
        ++listed;
    }
    report.append("Directory entries emitted: "+std::to_string(listed));
    return report.finish();
}
} // namespace iso_wcx
