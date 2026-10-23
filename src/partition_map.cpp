#include "iso_wcx/partition_map.hpp"
#include "iso_wcx/virtual_disk.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
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
using Input = VirtualDiskReader;
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
// MBR Extended Boot Records: each EBR has one relative data extent and an
// optional link relative to the original extended-container start.
// Reject cycles and malformed chains rather than returning partial results.
struct MbrRecord {
    std::uint8_t type{};
    std::uint64_t first{};
    std::uint64_t length{};
};
bool is_extended(std::uint8_t type) noexcept {
    return type==0x05 || type==0x0f || type==0x85;
}
MbrRecord mbr_record(const std::uint8_t* entry) noexcept {
    return {entry[4],le32(entry+8),le32(entry+12)};
}
bool is_empty(const MbrRecord& entry) noexcept {
    return entry.type==0 && entry.first==0 && entry.length==0;
}
void read_extended(Input& disk,std::uint32_t logical_sector_size,
                   std::uint64_t base,std::uint64_t count,
                   std::vector<PartitionEntry>& out) {
    const auto total=disk.size()/logical_sector_size;
    require(base>0 && count>=2 && base<total && count<=total-base,
            "invalid extended MBR container bounds");
    const auto limit=base+count;
    std::vector<std::uint64_t> visited;
    std::uint64_t cursor=base;
    for(unsigned depth=0;depth<128;++depth) {
        require(cursor>=base && cursor<limit,
                "EBR location escapes extended partition");
        require(std::find(visited.begin(),visited.end(),cursor)==visited.end(),
                "cyclic extended partition chain");
        visited.push_back(cursor);
        auto ebr=disk.read(cursor*logical_sector_size,512);
        require(ebr[510]==0x55 && ebr[511]==0xaa,"invalid EBR signature");
        const auto data=mbr_record(ebr.data()+446);
        const auto link=mbr_record(ebr.data()+462);
        require(is_empty(mbr_record(ebr.data()+478)) &&
                is_empty(mbr_record(ebr.data()+494)),
                "EBR uses unsupported third/fourth entry");
        require(data.type!=0 && !is_extended(data.type) &&
                data.type!=0xee && data.first>=1 && data.length>0 &&
                data.first<limit-cursor,
                "invalid logical partition mapping in EBR");
        const auto start=cursor+data.first;
        require(data.length<=limit-start && data.length<=total-start,
                "logical partition exceeds extended container");
        out.push_back({PartitionTableKind::mbr,start,data.length,data.type,{}});
        if(is_empty(link)) {
            for(const auto sector:visited)
                for(const auto& entry:out)
                    require(!(sector>=entry.first_lba &&
                              sector-entry.first_lba<entry.sector_count),
                            "EBR metadata overlaps a logical partition");
            return;
        }
        require(is_extended(link.type) && link.first>0 && link.length>0 &&
                link.first<count,
                "invalid EBR next-link type or offset");
        const auto next=base+link.first;
        require(next<limit && next>cursor &&
                link.length<=limit-next,
                "EBR next-link outside extended container or out of order");
        cursor=next;
    }
    throw std::runtime_error("EBR chain exceeds 128 entries");
}

std::vector<PartitionEntry> read_gpt_copy(Input& disk,std::uint32_t sector,
                                          std::uint64_t header_lba,
                                          std::uint64_t other_header_lba) {
    const auto sectors=disk.size()/sector;
    require(sectors>=8,"virtual disk too small for GPT");
    require(header_lba<sectors && other_header_lba<sectors &&
            header_lba!=other_header_lba,"invalid GPT header location");
    auto h=disk.read(header_lba*sector,sector);
    const std::string_view signature(reinterpret_cast<const char*>(h.data()),8);
    require(signature=="EFI PART","protective MBR without GPT header");
    const auto size=le32(h.data()+12);
    require(size>=92 && size<=sector,"invalid GPT header length");
    const auto header_crc=le32(h.data()+16);
    std::fill(h.begin()+16,h.begin()+20,0);
    require(crc32_ieee(h.data(),size)==header_crc,"GPT header CRC32 mismatch");
    const auto current=le64(h.data()+24),backup=le64(h.data()+32);
    const auto usable_start=le64(h.data()+40),usable_end=le64(h.data()+48);
    require(current==header_lba && backup==other_header_lba,
            "inconsistent GPT primary/backup header LBAs");
    require(usable_start>=2 && usable_start<=usable_end && usable_end<sectors-1,
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
    const auto last_table_lba=array_lba+(table_bytes+sector-1)/sector-1;
    require(last_table_lba<sectors &&
            (last_table_lba<usable_start || array_lba>usable_end) &&
            !(array_lba<=header_lba && header_lba<=last_table_lba) &&
            !(array_lba<=other_header_lba && other_header_lba<=last_table_lba),
            "GPT entry array overlaps data or headers");
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

// UEFI keeps a second GPT header at the last logical block and a separate
// partition-entry array. Recover only if that copy is independently CRC-valid,
// bounds-checked and correctly points back at the primary header (LBA 1).
std::vector<PartitionEntry> read_gpt(Input& disk,std::uint32_t sector) {
    const auto count=disk.size()/sector;
    require(count>=8,"virtual disk too small for GPT");
    try {
        return read_gpt_copy(disk,sector,1,count-1);
    }catch(const std::exception& primary_error){
        try {
            return read_gpt_copy(disk,sector,count-1,1);
        }catch(const std::exception& secondary_error){
            throw std::runtime_error(
                std::string("GPT primary and backup invalid: primary: ")+
                primary_error.what()+"; backup: "+secondary_error.what());
        }
    }
}
} // namespace

std::vector<PartitionEntry> inspect_partition_table(
    VirtualDiskReader& disk,std::uint32_t sector) {
    require(sector==512 || sector==4096,"unsupported logical sector size");
    require(disk.size()%sector==0,"virtual disk size not sector aligned");
    const auto total=disk.size()/sector;
    auto mbr=disk.read(0,512);
    require(mbr[510]==0x55 && mbr[511]==0xaa,"invalid MBR signature");
    std::vector<PartitionEntry> result;
    bool protective=false;
    bool found_extended=false;
    std::uint64_t extended_start=0,extended_length=0;
    for(std::size_t i=0;i<4;++i) {
        const auto item=mbr_record(mbr.data()+446+i*16);
        if(item.type==0) {
            require(is_empty(item),"invalid empty MBR partition");
            continue;
        }
        require(item.first>=1 && item.length>0 && item.first<total &&
                item.length<=total-item.first,
                "MBR partition beyond image boundaries");
        if(item.type==0xee){
            require(!protective && item.first==1,"invalid protective GPT MBR");
            protective=true;
        }else if(is_extended(item.type)){
            require(!found_extended,"multiple extended MBR containers unsupported");
            found_extended=true;
            extended_start=item.first;
            extended_length=item.length;
        }else
            result.push_back({PartitionTableKind::mbr,item.first,item.length,item.type,{}});
    }
    if(protective){
        // Reject hybrid MBR by default rather than silently ignoring active
        // legacy partitions that may disagree with the GPT map.
        require(result.empty() && !found_extended,
                "hybrid protective GPT/MBR requires explicit handling");
        return read_gpt(disk,sector);
    }
    if(found_extended){
        for(const auto& p:result){
            require(p.first_lba+p.sector_count<=extended_start ||
                    extended_start+extended_length<=p.first_lba,
                    "primary partition overlaps the extended MBR container");
        }
        read_extended(disk,sector,extended_start,extended_length,result);
    }
    check_ranges(result);
    return result;
}

std::vector<PartitionEntry> inspect_partition_table(
    const std::filesystem::path& path,std::uint32_t sector) {
    VirtualDiskReader disk(path);
    return inspect_partition_table(disk,sector);
}
} // namespace iso_wcx
