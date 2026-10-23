#include "iso_wcx/partition_map.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace iso_wcx {
namespace {

void require(bool ok,const char* why) {
    if(!ok)throw std::runtime_error(why);
}
std::uint32_t le32(const std::uint8_t* p) noexcept {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1])<<8) |
           (std::uint32_t(p[2])<<16) | (std::uint32_t(p[3])<<24);
}
std::uint64_t le64(const std::uint8_t* p) noexcept {
    return std::uint64_t(le32(p)) | (std::uint64_t(le32(p+4))<<32);
}

// IEEE CRC-32, used by the UEFI GPT header and GPT partition entry array.
std::uint32_t crc32_ieee(const std::uint8_t* data,std::size_t length) noexcept {
    static const auto table=[] {
        std::array<std::uint32_t,256> t{};
        for(std::uint32_t i=0;i<256;++i) {
            auto crc=i;
            for(unsigned b=0;b<8;++b)
                crc=(crc>>1)^((crc&1u)?0xedb88320u:0u);
            t[i]=crc;
        }
        return t;
    }();
    std::uint32_t crc=0xffffffffu;
    for(std::size_t i=0;i<length;++i)
        crc=table[(crc^data[i])&0xffu]^(crc>>8);
    return crc^0xffffffffu;
}
class Input {
public:
    explicit Input(const std::filesystem::path& path) {
        require(std::filesystem::is_regular_file(path),"partition source is not a regular file");
        bytes_=std::filesystem::file_size(path);
        require(bytes_>=512 && bytes_<=static_cast<std::uint64_t>(
                std::numeric_limits<std::streamoff>::max()),"invalid virtual-disk size");
        input_.open(path,std::ios::binary);
        require(input_.is_open(),"cannot open raw virtual-disk image");
    }
    [[nodiscard]] std::uint64_t size() const noexcept {return bytes_;}
    [[nodiscard]] std::vector<std::uint8_t> read(std::uint64_t at,std::size_t length) {
        require(at<=bytes_ && length<=bytes_-at,"partition data outside image");
        require(length<=4u*1024u*1024u,"partition metadata size limit exceeded");
        std::vector<std::uint8_t> data(length);
        input_.clear();
        input_.seekg(static_cast<std::streamoff>(at),std::ios::beg);
        require(bool(input_),"cannot seek in raw virtual disk");
        if(length)input_.read(reinterpret_cast<char*>(data.data()),
                              static_cast<std::streamsize>(length));
        require(bool(input_),"truncated raw virtual disk");
        return data;
    }
private:
    std::ifstream input_;
    std::uint64_t bytes_{};
};
void check_ranges(std::vector<PartitionEntry>& result) {
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){
        return a.first_lba<b.first_lba;
    });
    for(std::size_t i=1;i<result.size();++i) {
        const auto& prev=result[i-1];
        require(prev.first_lba<=std::numeric_limits<std::uint64_t>::max()-prev.sector_count,
                "partition extent overflow");
        require(prev.first_lba+prev.sector_count<=result[i].first_lba,
                "overlapping virtual-disk partitions");
    }
}
std::vector<PartitionEntry> read_gpt(Input& disk,std::uint32_t sector) {
    const auto sectors=disk.size()/sector;
    require(sectors>=8,"virtual disk too small for GPT");
    auto h=disk.read(sector,sector);
    const std::string_view signature(reinterpret_cast<const char*>(h.data()),8);
    require(signature=="EFI PART","protective MBR without GPT header");
    const auto size=le32(h.data()+12);
    require(size>=92 && size<=sector,"invalid GPT header length");
    const auto header_crc=le32(h.data()+16);
    std::fill(h.begin()+16,h.begin()+20,0);
    require(crc32_ieee(h.data(),size)==header_crc,"GPT header CRC32 mismatch");
    const auto current=le64(h.data()+24),backup=le64(h.data()+32);
    const auto usable_start=le64(h.data()+40),usable_end=le64(h.data()+48);
    require(current==1 && backup<sectors && backup!=current,
            "invalid GPT primary/backup header LBAs");
    require(usable_start>=2 && usable_start<=usable_end && usable_end<sectors,
            "invalid GPT usable sector bounds");
    const auto array_lba=le64(h.data()+72);
    const auto count=le32(h.data()+80),entry_size=le32(h.data()+84);
    require(count>=1 && count<=4096 && entry_size>=128 &&
            entry_size<=4096 && (entry_size%128)==0,
            "unsupported GPT partition array dimensions");
    const auto table_bytes=std::uint64_t(count)*entry_size;
    require(table_bytes<=4u*1024u*1024u &&
            array_lba>=2 && array_lba<sectors &&
            table_bytes<=disk.size()-array_lba*sector,
            "GPT partition array outside image");
    auto table=disk.read(array_lba*sector,static_cast<std::size_t>(table_bytes));
    require(crc32_ieee(table.data(),table.size())==le32(h.data()+88),
            "GPT partition entry array CRC32 mismatch");
    std::vector<PartitionEntry> partitions;
    for(std::uint32_t i=0;i<count;++i) {
        const auto* e=table.data()+std::size_t(i)*entry_size;
        bool empty=true;
        for(unsigned k=0;k<16;++k)if(e[k]) {empty=false;break;}
        if(empty)continue;
        const auto first=le64(e+32),last=le64(e+40);
        require(first>=usable_start && first<=last && last<=usable_end,
                "GPT partition outside usable sector range");
        PartitionEntry p;
        p.scheme=PartitionTableKind::gpt;
        p.first_lba=first;
        p.sector_count=last-first+1;
        std::copy_n(e,16,p.gpt_type.begin());
        partitions.push_back(std::move(p));
    }
    check_ranges(partitions);
    return partitions;
}
} // namespace

std::vector<PartitionEntry> inspect_partition_table(
    const std::filesystem::path& raw_image,std::uint32_t sector) {
    require(sector==512 || sector==4096,"unsupported logical sector size");
    Input disk(raw_image);
    require(disk.size()%sector==0,"virtual disk size not sector aligned");
    const auto total=disk.size()/sector;
    auto mbr=disk.read(0,512);
    require(mbr[510]==0x55 && mbr[511]==0xaa,"invalid MBR signature");
    std::vector<PartitionEntry> result;
    bool protective=false;
    for(std::size_t i=0;i<4;++i) {
        const auto* e=mbr.data()+446+i*16;
        const auto type=e[4];
        const auto start=std::uint64_t(le32(e+8)),count=std::uint64_t(le32(e+12));
        if(!type) {
            require(start==0 && count==0,"invalid empty MBR partition");
            continue;
        }
        if(type==0xee){protective=true;continue;}
        require(type!=0x05 && type!=0x0f && type!=0x85,
                "extended MBR partitions not supported");
        require(start>=1 && count>0 && start<total && count<=total-start,
                "MBR partition beyond image boundaries");
        result.push_back({PartitionTableKind::mbr,start,count,type,{}});
    }
    if(protective)return read_gpt(disk,sector);
    check_ranges(result);
    return result;
}
} // namespace iso_wcx
