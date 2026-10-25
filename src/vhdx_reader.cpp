#include "iso_wcx/vhdx_reader.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace iso_wcx {
namespace {
constexpr std::uint64_t MiB = 1024ull * 1024ull;
constexpr std::uint64_t MaxCapacity = 64ull * 1024ull * 1024ull * 1024ull * 1024ull;
using Guid = std::array<std::uint8_t, 16>;
using Bytes = std::vector<std::uint8_t>;

// Microsoft [MS-VHDX] GUIDs encoded as the on-disk little-endian byte array.
constexpr Guid BatId = {0x66,0x77,0xc2,0x2d,0x23,0xf6,0x00,0x42,
                        0x9d,0x64,0x11,0x5e,0x9b,0xfd,0x4a,0x08};
constexpr Guid MetaId = {0x06,0xa2,0x7c,0x8b,0x90,0x47,0x9a,0x4b,
                         0xb8,0xfe,0x57,0x5f,0x05,0x0f,0x88,0x6e};
constexpr Guid ParamsId = {0x37,0x67,0xa1,0xca,0x36,0xfa,0x43,0x4d,
                           0xb3,0xb6,0x33,0xf0,0xaa,0x44,0xe7,0x6b};
constexpr Guid SizeId = {0x24,0x42,0xa5,0x2f,0x1b,0xcd,0x76,0x48,
                         0xb2,0x11,0x5d,0xbe,0xd8,0x3b,0xf4,0xb8};
constexpr Guid SectorId = {0x1d,0xbf,0x41,0x81,0x6f,0xa9,0x09,0x47,
                           0xba,0x47,0xf2,0x33,0xa8,0xfa,0xab,0x5f};
constexpr Guid PhysicalId = {0xc7,0x48,0xa3,0xcd,0x5d,0x44,0x71,0x44,
                             0x9c,0xc9,0xe9,0x88,0x52,0x51,0xc5,0x56};
constexpr Guid DiskId = {0xab,0x12,0xca,0xbe,0xe6,0xb2,0x23,0x45,
                         0x93,0xef,0xc3,0x09,0xe0,0x00,0xc7,0x46};
constexpr Guid ParentId = {0x2d,0x5f,0xd3,0xa8,0x0b,0xb3,0x4d,0x45,
                           0xab,0xf7,0xd3,0xd8,0x48,0x34,0xab,0x0c};

void require(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
}
std::uint16_t u16(const std::uint8_t* p) noexcept {
    return std::uint16_t(p[0]) | (std::uint16_t(p[1]) << 8);
}
std::uint32_t u32(const std::uint8_t* p) noexcept {
    return std::uint32_t(u16(p)) | (std::uint32_t(u16(p+2)) << 16);
}
std::uint64_t u64(const std::uint8_t* p) noexcept {
    return std::uint64_t(u32(p)) | (std::uint64_t(u32(p+4)) << 32);
}
Guid guid(const std::uint8_t* p) {
    Guid value{};
    std::copy_n(p, value.size(), value.begin());
    return value;
}
bool begins(const std::uint8_t* p, std::string_view signature) {
    return std::memcmp(p, signature.data(), signature.size()) == 0;
}
bool power_two(std::uint32_t value) noexcept {
    return value && !(value & (value-1));
}
std::uint32_t crc32c(const std::uint8_t* bytes, std::size_t length) noexcept {
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t i=0; i<length; ++i) {
        const auto octet=(i>=4 && i<8) ? std::uint8_t{0} : bytes[i];
        crc ^= octet;
        for (unsigned bit=0; bit<8; ++bit)
            crc=(crc>>1) ^ ((crc&1) ? 0x82f63b78u : 0u);
    }
    return ~crc;
}
bool valid_crc(const Bytes& data) noexcept {
    return data.size() >= 8 && u32(data.data()+4)==crc32c(data.data(),data.size());
}
bool overlap(std::uint64_t first,std::uint64_t first_len,
             std::uint64_t second,std::uint64_t second_len) noexcept {
    // The calling code has already bounded all extent ends.
    return first<second+second_len && second<first+first_len;
}
struct Header {
    std::uint64_t seq{};
    std::uint64_t log_offset{};
    std::uint32_t log_length{};
    bool log_dirty{};
    Bytes contents;
};
std::optional<Header> parse_header(const Bytes& bytes) {
    if (bytes.size()!=4096 || !begins(bytes.data(),"head") || !valid_crc(bytes) ||
        u16(bytes.data()+64)!=0 || u16(bytes.data()+66)!=1 ||
        !std::all_of(bytes.begin()+80,bytes.end(),
                     [](std::uint8_t v){return v==0;}))
        return std::nullopt;
    Header header;
    header.seq=u64(bytes.data()+8);
    header.log_length=u32(bytes.data()+68);
    header.log_offset=u64(bytes.data()+72);
    header.log_dirty=std::any_of(bytes.begin()+48,bytes.begin()+64,
                               [](std::uint8_t byte){return byte!=0;});
    header.contents=bytes;
    return header;
}
struct Region {
    std::uint64_t offset{};
    std::uint64_t size{};
    friend bool operator==(const Region&,const Region&)=default;
};
struct Regions {
    Region bat;
    Region meta;
    std::vector<Region> optional;
    friend bool operator==(const Regions&,const Regions&)=default;
};
std::optional<Regions> parse_regions(const Bytes& bytes,std::uint64_t physical_size) {
    if (bytes.size()!=65536 || !begins(bytes.data(),"regi") || !valid_crc(bytes) ||
        u32(bytes.data()+12)!=0)
        return std::nullopt;
    const auto count=u32(bytes.data()+8);
    if (count>2047) return std::nullopt;
    std::optional<Region> bat,meta;
    std::vector<Region> seen;
    std::vector<Region> optional;
    seen.reserve(count);
    for (std::uint32_t i=0;i<count;++i) {
        const auto* item=bytes.data()+16+std::size_t(i)*32;
        const auto offset=u64(item+16);
        const auto length=u32(item+24);
        const auto flags=u32(item+28);
        if(flags>1 || offset<MiB || offset%MiB || !length ||
           length%MiB || offset>physical_size || length>physical_size-offset)
            return std::nullopt;
        const Region current{offset,length};
        for (const auto& previous:seen)
            if (overlap(current.offset,current.size,previous.offset,previous.size))
                return std::nullopt;
        seen.push_back(current);
        const auto id=guid(item);
        if(id==BatId) {
            if(bat) return std::nullopt;
            bat=Region{offset,length};
        } else if(id==MetaId) {
            if(meta) return std::nullopt;
            meta=Region{offset,length};
        } else if(flags==1)
            return std::nullopt;
        else optional.push_back(current);
    }
    if(!bat || !meta) return std::nullopt;
    return Regions{*bat,*meta,std::move(optional)};
}
} // namespace

VhdxReader::VhdxReader(const std::filesystem::path& image) {
    require(std::filesystem::is_regular_file(image) &&
            !std::filesystem::is_symlink(image),
            "VHDX must be a regular non-symlink image");
    file_.open(image,std::ios::binary|std::ios::ate);
    require(file_.is_open(),"cannot open VHDX");
    const auto end=file_.tellg();
    require(end>=0,"cannot size VHDX");
    file_size_=static_cast<std::uint64_t>(end);
    parse();
}
std::vector<std::uint8_t> VhdxReader::file_read(
    std::uint64_t offset,std::size_t length) {
    require(length<=4u*1024u*1024u &&
            offset<=file_size_ && length<=file_size_-offset &&
            offset<=static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()),
            "VHDX physical read out of bounds");
    Bytes data(length);
    if(!length) return data;
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(offset),std::ios::beg);
    require(bool(file_),"cannot seek VHDX");
    file_.read(reinterpret_cast<char*>(data.data()),
               static_cast<std::streamsize>(length));
    require(bool(file_),"truncated VHDX read");
    return data;
}
void VhdxReader::parse() {
    require(file_size_>=4*MiB,"VHDX too small");
    const auto magic=file_read(0,8);
    require(begins(magic.data(),"vhdxfile"),"invalid VHDX signature");

    const auto one=parse_header(file_read(0x10000,4096));
    const auto two=parse_header(file_read(0x20000,4096));
    require(one || two,"both VHDX headers invalid");
    require(!(one && two && one->seq==two->seq &&
              one->contents!=two->contents),
            "VHDX header copies conflict at identical sequence");
    const Header& active=(one && (!two || one->seq>=two->seq))?*one:*two;
    require(!active.log_dirty,"VHDX log replay required; clean base images only");
    if(active.log_length || active.log_offset) {
        require(active.log_length && active.log_offset>=MiB &&
                active.log_length%MiB==0 && active.log_offset%MiB==0 &&
                active.log_offset<=file_size_ &&
                active.log_length<=file_size_-active.log_offset,
                "invalid VHDX transaction log extent");
    }

    log_offset_=active.log_offset;
    log_length_=active.log_length;

    const auto regions1=parse_regions(file_read(0x30000,65536),file_size_);
    const auto regions2=parse_regions(file_read(0x40000,65536),file_size_);
    require(regions1 || regions2,"both VHDX region tables invalid");
    if(regions1 && regions2) require(*regions1==*regions2,
                                   "conflicting valid VHDX region tables");
    const auto regions=regions1?*regions1:*regions2;
    // VHDX region tables may include optional vendor-defined areas. Do not
    // allow payload BAT entries to alias them as ordinary disk contents.
    optional_regions_=regions.optional;
    for (const auto& region:optional_regions_)
        require(!overlap(region.offset,region.size,0,MiB) &&
                (!active.log_length ||
                 !overlap(region.offset,region.size,
                          active.log_offset,active.log_length)),
                "optional VHDX region overlaps reserved header or transaction log");
    require(!overlap(regions.bat.offset,regions.bat.size,0,MiB) &&
            !overlap(regions.meta.offset,regions.meta.size,0,MiB),
            "VHDX regions overlap header section");
    if(active.log_length)
        require(!overlap(regions.bat.offset,regions.bat.size,
                         active.log_offset,active.log_length) &&
                !overlap(regions.meta.offset,regions.meta.size,
                         active.log_offset,active.log_length),
                "VHDX region overlaps transaction log");
    bat_offset_=regions.bat.offset;
    bat_length_=regions.bat.size;
    metadata_offset_=regions.meta.offset;
    metadata_length_=regions.meta.size;
    require(metadata_length_>=65536,"VHDX metadata truncated");
    const auto table=file_read(metadata_offset_,65536);
    require(begins(table.data(),"metadata") &&
            u16(table.data()+8)==0 &&
            std::all_of(table.begin()+12,table.begin()+32,
                        [](std::uint8_t v){return v==0;}),
            "VHDX metadata table signature or reserved bytes invalid");
    const auto count=u16(table.data()+10);
    require(count<=2047,"VHDX metadata table count invalid");

    std::optional<std::uint32_t> block_size,logical_sector,physical_sector;
    std::optional<std::uint64_t> virtual_size;
    bool has_disk_id=false;
    std::vector<Region> item_regions;
    item_regions.reserve(count);
    for (unsigned i=0;i<count;++i) {
        const auto* entry=table.data()+32+i*32;
        const auto id=guid(entry);
        const auto offset=u32(entry+16);
        const auto len=u32(entry+20);
        const auto flags=u32(entry+24);
        require((flags&~7u)==0,"unknown VHDX metadata flags");
        require(offset>=65536 && offset<=metadata_length_ &&
                len && len<=MiB && len<=metadata_length_-offset,
                "invalid VHDX metadata extent");
        for (const auto& previous:item_regions)
            require(!overlap(offset,len,previous.offset,previous.size),
                    "overlapping VHDX metadata item extents");
        item_regions.push_back(Region{offset,len});
        if (id==ParentId)
            throw std::runtime_error("differencing VHDX parent locator unsupported");
        if (id==ParamsId) {
            require(!block_size && len==8,"duplicate/corrupt VHDX File Parameters");
            const auto val=file_read(metadata_offset_+offset,len);
            block_size=u32(val.data());
            const auto params_flags=u32(val.data()+4);
            require((params_flags&~3u)==0 && !(params_flags&2u),
                    "VHDX has unsupported file parameter flags or parent");
        } else if(id==SizeId) {
            require(!virtual_size && len==8,"duplicate/corrupt VHDX Virtual Disk Size");
            const auto val=file_read(metadata_offset_+offset,len);
            virtual_size=u64(val.data());
        } else if(id==SectorId) {
            require(!logical_sector && len==4,"duplicate/corrupt VHDX sector size");
            const auto val=file_read(metadata_offset_+offset,len);
            logical_sector=u32(val.data());
        } else if(id==PhysicalId) {
            require(!physical_sector && len==4,"duplicate/corrupt VHDX physical sector");
            const auto val=file_read(metadata_offset_+offset,len);
            physical_sector=u32(val.data());
        } else if(id==DiskId) {
            require(!has_disk_id && len==16,"duplicate/corrupt VHDX disk ID");
            has_disk_id=true;
        } else if(flags&4u) {
            throw std::runtime_error("unknown required VHDX metadata item");
        }
    }
    require(block_size && logical_sector && physical_sector && virtual_size &&
            has_disk_id,"missing required VHDX metadata");
    require(*block_size>=MiB && *block_size<=256*MiB && power_two(*block_size),
            "invalid VHDX payload block size");
    require((*logical_sector==512 || *logical_sector==4096) &&
            (*physical_sector==512 || *physical_sector==4096) &&
            *physical_sector>=*logical_sector,
            "invalid VHDX sector sizes");
    require(*virtual_size>=*logical_sector && *virtual_size<=MaxCapacity &&
            *virtual_size%*logical_sector==0,
            "invalid VHDX virtual disk size");
    logical_size_=*virtual_size;
    block_size_=*block_size;
    sector_size_=*logical_sector;
    chunk_ratio_=((1ull<<23)*sector_size_)/block_size_;
    require(chunk_ratio_>0,"invalid VHDX BAT chunk ratio");
    const auto blocks=(logical_size_+block_size_-1)/block_size_;
    const auto entries=blocks+(blocks-1)/chunk_ratio_;
    require(entries<=bat_length_/8,"VHDX BAT is too short");
}

std::vector<std::uint8_t> VhdxReader::read(
    std::uint64_t offset,std::size_t length) {
    require(length<=4u*1024u*1024u &&
            offset<=logical_size_ && length<=logical_size_-offset,
            "VHDX virtual read outside logical capacity");
    Bytes out(length,0);
    std::size_t done=0;
    while(done<length) {
        const auto logical=offset+done;
        const auto block=logical/block_size_;
        const auto local=logical%block_size_;
        const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(
            length-done,block_size_-local));
        const auto bat_index=block+block/chunk_ratio_;
        require(bat_index<bat_length_/8,"VHDX BAT index out of range");
        const auto entry_bytes=file_read(bat_offset_+bat_index*8,8);
        const auto entry=u64(entry_bytes.data());
        require((entry&0xffff8u)==0,"reserved VHDX BAT bits set");
        const auto state=unsigned(entry&7u);
        switch(state) {
        case 0: // Not present (base disk, zero fill).
        case 1: // Undefined (nonallocated; deterministic zero fill).
        case 2: // Explicit zero block.
        case 3: // Unmapped block.
            require((entry>>20)==0,"unallocated BAT entry has nonzero offset");
            break;
        case 6: {
            const auto physical=(entry>>20)*MiB;
            require(physical>=MiB && physical<=file_size_ &&
                    block_size_<=file_size_-physical,
                    "VHDX BAT payload extent out of file bounds");
            require(!overlap(physical,block_size_,0,MiB) &&
                    !overlap(physical,block_size_,bat_offset_,bat_length_) &&
                    !overlap(physical,block_size_,metadata_offset_,metadata_length_) &&
                    (!log_length_ ||
                     !overlap(physical,block_size_,log_offset_,log_length_)),
                    "VHDX payload overlaps reserved region or transaction log");
            for (const auto& region:optional_regions_)
                require(!overlap(physical,block_size_,region.offset,region.size),
                        "VHDX payload aliases optional vendor region");
            const auto bytes=file_read(physical+local,count);
            std::copy(bytes.begin(),bytes.end(),out.begin()+done);
            break;
        }
        default:
            throw std::runtime_error("partially present/reserved VHDX BAT block unsupported");
        }
        done+=count;
    }
    return out;
}

} // namespace iso_wcx
