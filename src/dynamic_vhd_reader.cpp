#include "iso_wcx/dynamic_vhd_reader.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace iso_wcx {
namespace {

constexpr std::uint32_t sector_size = 512u;
constexpr std::uint32_t maximum_block_size = 64u*1024u*1024u;
constexpr std::uint32_t maximum_bat_entries = 1024u*1024u;

void require(bool condition, const char* reason) {
    if(!condition) throw std::runtime_error(reason);
}

std::uint32_t be32(const std::uint8_t* p) noexcept {
    return (std::uint32_t(p[0])<<24) | (std::uint32_t(p[1])<<16) |
           (std::uint32_t(p[2])<<8) | std::uint32_t(p[3]);
}
std::uint64_t be64(const std::uint8_t* p) noexcept {
    return (std::uint64_t(be32(p))<<32) | be32(p+4);
}
bool all_zero(const std::uint8_t* first,std::size_t length) {
    return std::all_of(first,first+length,
                       [](std::uint8_t value){return value==0;});
}
bool power_of_two(std::uint32_t n) noexcept {
    return n && (n&(n-1))==0;
}
std::uint32_t vhd_checksum(const std::uint8_t* src,std::size_t length,
                           std::size_t checksum_offset) {
    std::uint32_t sum=0;
    for(std::size_t i=0;i<length;++i)
        if(i<checksum_offset || i>=checksum_offset+4) sum+=src[i];
    return ~sum;
}
std::uint64_t round_to_sector(std::uint64_t length) {
    require(length<=std::numeric_limits<std::uint64_t>::max()-511,
            "dynamic VHD sector alignment overflow");
    return (length+511)&~std::uint64_t(511);
}
}

DynamicVhdReader::DynamicVhdReader(
    const std::filesystem::path& image,
    std::uint64_t capacity,
    std::uint64_t physical_size,
    std::uint64_t dynamic_header_offset,
    const std::array<std::uint8_t,512>& footer)
    : capacity_(capacity),physical_size_(physical_size) {
    require(physical_size_>=2048 && (physical_size_%sector_size)==0 &&
            capacity_>=sector_size && (capacity_%sector_size)==0,
            "invalid dynamic VHD file or logical disk geometry");
    require(dynamic_header_offset>=sector_size &&
            (dynamic_header_offset%sector_size)==0 &&
            dynamic_header_offset<=physical_size_-sector_size-1024,
            "dynamic VHD header lies outside file");

    input_.open(image,std::ios::binary);
    require(input_.is_open(),"cannot open dynamic VHD container");
    // The first footer is the backup of the last footer, and is mandatory
    // here. If the copies disagree, choosing one could change the mapping.
    std::array<std::uint8_t,512> first_footer{};
    read_file(0,first_footer.data(),first_footer.size());
    require(first_footer==footer,
            "dynamic VHD backup and terminal footer differ");

    std::array<std::uint8_t,1024> header{};
    read_file(dynamic_header_offset,header.data(),header.size());
    require(std::string_view(reinterpret_cast<const char*>(header.data()),8)
                =="cxsparse",
            "dynamic VHD header signature missing");
    require(be64(header.data()+8)==UINT64_MAX &&
            be32(header.data()+24)==0x00010000u &&
            be32(header.data()+36)==vhd_checksum(header.data(),header.size(),36),
            "dynamic VHD header version, chain or checksum invalid");
    // Parent data is meaningful only for differencing disks. Refuse an
    // unexpected parent descriptor on a dynamic base disk.
    require(all_zero(header.data()+40,header.size()-40),
            "dynamic base VHD unexpectedly contains parent metadata");

    const auto bat_offset=be64(header.data()+16);
    const auto entries=be32(header.data()+28);
    block_size_=be32(header.data()+32);
    require(block_size_>=sector_size && block_size_<=maximum_block_size &&
            power_of_two(block_size_),
            "unsupported dynamic VHD block size");
    const auto blocks=(capacity_+block_size_-1)/block_size_;
    require(blocks>=1 && blocks<=maximum_bat_entries &&
            entries==blocks,
            "dynamic VHD BAT entry count does not match logical capacity");

    const auto metadata_end=dynamic_header_offset+header.size();
    const auto bat_bytes=std::uint64_t(entries)*4;
    const auto bat_extent=round_to_sector(bat_bytes);
    require((bat_offset%sector_size)==0 &&
            bat_offset>=metadata_end &&
            bat_offset<=physical_size_-sector_size &&
            bat_extent<=physical_size_-sector_size-bat_offset,
            "dynamic VHD BAT metadata overlaps footer or header");
    const auto metadata_limit=bat_offset+bat_extent;
    const auto sectors_per_block=block_size_/sector_size;
    const auto bitmap_bits_bytes=(std::uint64_t(sectors_per_block)+7)/8;
    const auto bitmap_extent=round_to_sector(bitmap_bits_bytes);
    require(bitmap_extent<=std::numeric_limits<std::uint32_t>::max() &&
            bitmap_extent<=maximum_block_size,
            "invalid dynamic VHD sector bitmap dimensions");
    bitmap_bytes_=static_cast<std::uint32_t>(bitmap_extent);

    std::vector<std::uint8_t> table(static_cast<std::size_t>(bat_bytes));
    read_file(bat_offset,table.data(),table.size());
    bat_.resize(entries);
    std::vector<std::pair<std::uint64_t,std::uint64_t>> extents;
    extents.reserve(entries);
    const auto physical_extent=std::uint64_t(block_size_)+bitmap_bytes_;
    for(std::uint32_t i=0;i<entries;++i) {
        const auto value=be32(table.data()+std::size_t(i)*4);
        bat_[i]=value;
        if(value==UINT32_MAX) continue;
        const auto position=std::uint64_t(value)*sector_size;
        require(position>=metadata_limit &&
                position<=physical_size_-sector_size &&
                physical_extent<=physical_size_-sector_size-position,
                "dynamic VHD allocated block overlaps metadata or footer");
        extents.emplace_back(position,position+physical_extent);
    }
    std::sort(extents.begin(),extents.end());
    for(std::size_t i=1;i<extents.size();++i)
        require(extents[i-1].second<=extents[i].first,
                "dynamic VHD BAT contains duplicate or overlapping blocks");

    // An allocated block's bitmap is limited to one block and is loaded
    // lazily on access. No virtual disk data is materialized in memory.
    cached_bitmap_.resize(bitmap_bytes_);
}

void DynamicVhdReader::read_file(
        std::uint64_t physical,std::uint8_t* dest,std::size_t count) {
    require(physical<=physical_size_ && count<=physical_size_-physical &&
            physical<=static_cast<std::uint64_t>(
                std::numeric_limits<std::streamoff>::max()),
            "dynamic VHD file read outside physical container");
    if(count==0)return;
    input_.clear();
    input_.seekg(static_cast<std::streamoff>(physical),std::ios::beg);
    require(bool(input_),"cannot seek to dynamic VHD metadata or payload");
    input_.read(reinterpret_cast<char*>(dest),
                static_cast<std::streamsize>(count));
    require(bool(input_),"dynamic VHD metadata or payload is truncated");
}

void DynamicVhdReader::load_bitmap(std::size_t block_index) {
    require(block_index<bat_.size() && bat_[block_index]!=UINT32_MAX,
            "cannot load bitmap of unallocated dynamic VHD block");
    if(cached_block_==block_index)return;
    const auto physical=std::uint64_t(bat_[block_index])*sector_size;
    read_file(physical,cached_bitmap_.data(),cached_bitmap_.size());
    cached_block_=block_index;
}

std::vector<std::uint8_t> DynamicVhdReader::read(
        std::uint64_t offset,std::size_t length) {
    require(length<=4u*1024u*1024u,
            "dynamic VHD read exceeds bounded 4 MiB window");
    require(offset<=capacity_ && length<=capacity_-offset,
            "dynamic VHD read outside logical disk");
    std::vector<std::uint8_t> output(length,0);
    std::size_t done=0;
    while(done<length) {
        const auto logical=offset+done;
        const auto block_index=static_cast<std::size_t>(logical/block_size_);
        require(block_index<bat_.size(),"dynamic VHD block index outside BAT");
        const auto inside=static_cast<std::uint32_t>(logical%block_size_);
        const auto sector_in_block=inside/sector_size;
        const auto in_sector=inside%sector_size;
        const auto chunk=std::min<std::size_t>(length-done,
                                              sector_size-in_sector);
        if(bat_[block_index]!=UINT32_MAX) {
            load_bitmap(block_index);
            const auto bit=std::size_t(sector_in_block);
            if((cached_bitmap_[bit/8] &
                    std::uint8_t(0x80u>>(bit%8)))!=0) {
                const auto physical=std::uint64_t(bat_[block_index])*sector_size+
                                    bitmap_bytes_+inside;
                read_file(physical,output.data()+done,chunk);
            }
        }
        done+=chunk;
    }
    return output;
}
} // namespace iso_wcx
