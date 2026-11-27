#include "iso_wcx/archive_edit.hpp"
#include "iso_wcx/udf_edit_inspection.hpp"
#include "iso_wcx/disc.hpp"
#include "iso_wcx/edit_transaction.hpp"
#include "iso_wcx/optical_edit_guard.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <iterator>
#include <memory>
#include <optional>
#include <queue>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/fs.h>
#endif

namespace iso_wcx {
namespace {
using Bytes=std::vector<std::uint8_t>;
constexpr std::uint64_t block=2048;
constexpr std::uint64_t max_image=0xffffffffull*block;
constexpr std::size_t max_dir=8u*1024u*1024u;
constexpr unsigned max_depth=32;
constexpr std::size_t max_items=100000;

void need(bool condition,const char* text) {
    if(!condition)throw EditUnsupported(text);
}
std::uint16_t u16(const Bytes& b,std::size_t at) {
    need(at<=b.size() && b.size()-at>=2,"truncated UDF uint16");
    return std::uint16_t(b[at]) | (std::uint16_t(b[at+1])<<8);
}
std::uint32_t u32(const Bytes& b,std::size_t at) {
    need(at<=b.size() && b.size()-at>=4,"truncated UDF uint32");
    return std::uint32_t(u16(b,at)) | (std::uint32_t(u16(b,at+2))<<16);
}
std::uint64_t u64(const Bytes& b,std::size_t at) {
    return std::uint64_t(u32(b,at)) | (std::uint64_t(u32(b,at+4))<<32);
}
void put16(Bytes& b,std::size_t at,std::uint16_t n) {
    need(at<=b.size() && b.size()-at>=2,"short UDF uint16 write");
    b[at]=static_cast<std::uint8_t>(n);
    b[at+1]=static_cast<std::uint8_t>(n>>8);
}
void put32(Bytes& b,std::size_t at,std::uint32_t n) {
    put16(b,at,static_cast<std::uint16_t>(n));
    put16(b,at+2,static_cast<std::uint16_t>(n>>16));
}
void put64(Bytes& b,std::size_t at,std::uint64_t n) {
    put32(b,at,static_cast<std::uint32_t>(n));
    put32(b,at+4,static_cast<std::uint32_t>(n>>32));
}
std::uint16_t crc16(const std::uint8_t* data,std::size_t len) {
    std::uint16_t crc=0;
    for(std::size_t i=0;i<len;++i) {
        crc^=std::uint16_t(data[i])<<8;
        for(unsigned bit=0;bit<8;++bit)
            crc=static_cast<std::uint16_t>(
                (crc&0x8000u)?(crc<<1)^0x1021u:(crc<<1));
    }
    return crc;
}
void tag_check(const Bytes& b,std::size_t offset,std::uint16_t expected) {
    need(offset<=b.size() && b.size()-offset>=16 &&
         u16(b,offset)==expected,"unexpected UDF tag identifier");
    const auto version=u16(b,offset+2);
    need(version==2 || version==3,"unsupported UDF descriptor version");
    std::uint32_t sum=0;
    for(std::size_t j=0;j<16;++j)
        if(j!=4)sum+=b[offset+j];
    need((sum&255)==b[offset+4],"UDF tag checksum mismatch");
    const auto length=u16(b,offset+10);
    need(length<=b.size()-(offset+16),"UDF tag CRC length outside descriptor");
    if(length)need(crc16(b.data()+offset+16,length)==u16(b,offset+8),
                   "UDF descriptor CRC16 mismatch");
}
void tag_update(Bytes& b,std::size_t at,std::size_t descriptor_bytes) {
    need(at<=b.size() && descriptor_bytes>=16 &&
         descriptor_bytes<=b.size()-at &&
         descriptor_bytes-16<=65535,"UDF tag update outside record");
    const auto count=static_cast<std::uint16_t>(descriptor_bytes-16);
    put16(b,at+10,count);
    put16(b,at+8,crc16(b.data()+at+16,count));
    std::uint32_t sum=0;
    for(std::size_t i=0;i<16;++i)
        if(i!=4)sum+=b[at+i];
    b[at+4]=static_cast<std::uint8_t>(sum);
}
void utf8(std::string& text,std::uint32_t c) {
    need(c!=0 && c<=0x10ffff && !(c>=0xd800 && c<=0xdfff),
         "invalid UDF Unicode scalar");
    if(c<=0x7f)text.push_back(static_cast<char>(c));
    else if(c<=0x7ff) {
        text.push_back(static_cast<char>(0xc0|(c>>6)));
        text.push_back(static_cast<char>(0x80|(c&63)));
    } else if(c<=0xffff) {
        text.push_back(static_cast<char>(0xe0|(c>>12)));
        text.push_back(static_cast<char>(0x80|((c>>6)&63)));
        text.push_back(static_cast<char>(0x80|(c&63)));
    } else {
        text.push_back(static_cast<char>(0xf0|(c>>18)));
        text.push_back(static_cast<char>(0x80|((c>>12)&63)));
        text.push_back(static_cast<char>(0x80|((c>>6)&63)));
        text.push_back(static_cast<char>(0x80|(c&63)));
    }
}
std::string name(const Bytes& b,std::size_t p,std::size_t n) {
    need(n>=2 && p<=b.size() && n<=b.size()-p,
         "invalid UDF file identifier length");
    const auto compression=b[p++];
    --n;
    std::string result;
    if(compression==8) {
        for(std::size_t i=0;i<n;++i)utf8(result,b[p+i]);
    } else {
        need(compression==16 && n%2==0,"invalid UDF CS0 filename");
        for(std::size_t i=0;i<n;i+=2) {
            std::uint32_t cp=(std::uint32_t(b[p+i])<<8)|b[p+i+1];
            if(cp>=0xd800 && cp<=0xdbff) {
                need(i+3<n,"truncated UDF filename surrogate");
                const auto low=(std::uint32_t(b[p+i+2])<<8)|b[p+i+3];
                need(low>=0xdc00 && low<=0xdfff,
                     "invalid UDF filename surrogate");
                cp=0x10000+((cp-0xd800)<<10)+low-0xdc00;
                i+=2;
            }
            utf8(result,cp);
        }
    }
    need(result.find('/')==std::string::npos,
         "UDF file identifier contains a path separator");
    return edit_path(result);
}

struct Location {
    std::uint64_t at=0,bytes=0;
};
bool intersects(Location a,Location b) {
    return a.bytes && b.bytes &&
           a.at<b.at+b.bytes && b.at<a.at+a.bytes;
}
// A file's 64-bit InformationLength can span multiple 30-bit ShortADs.
struct DataRun {
    std::uint64_t at{},recorded{};
    std::uint32_t logical{},blocks{};
};
struct Fe {
    Bytes bytes;
    std::uint64_t at{},data_at{},size{},capacity{},recorded{};
    std::size_t base{},ads{},ad_offset{};
    unsigned storage{};
    std::uint8_t type{};
    std::vector<DataRun> runs;
};
struct File {
    Fe fe;
    std::uint64_t fid_at{};
    std::size_t fid_bytes{};
    std::uint64_t parent_at{};
    bool parent_embedded{};
};
struct Interval {
    Location range;
    std::uint64_t owner;
};
struct Bitmap {
    Bytes bytes;
    std::uint64_t at{};
    std::uint32_t bits{},byte_count{},logical{};
};
// Native UDF 4/14.11 Unallocated Space Entry (tag 263). The allocator
// operates on a temporary, fully validated bitmap derived from the sorted
// ShortAD table, but commits an updated TABLE, not a fabricated on-disk
// Space Bitmap Descriptor. All writes remain inside the original USE sector.
struct UnallocatedTable {
    Bytes bytes;
    std::uint64_t at{};
    std::uint32_t declared_length{};
};
struct Integrity {
    Bytes bytes;
    std::uint64_t at{};
    std::uint32_t impl_bytes{};
    std::uint64_t next_id{};
};
// UDF 4/14.12: each set bit represents an unallocated logical block.
// Bitmap descriptors may span multiple sectors (bounded by max_dir).
bool bitmap_free(const Bitmap& bitmap,std::uint32_t logical) {
    need(logical<bitmap.bits,"UDF bitmap logical address outside partition");
    return (bitmap.bytes[24+logical/8]&(1u<<(logical%8)))!=0;
}
void bitmap_reserve(Bitmap& bitmap,std::uint32_t logical) {
    need(bitmap_free(bitmap,logical),
         "UDF bitmap claims requested block is already allocated");
    bitmap.bytes[24+logical/8]&=
        static_cast<std::uint8_t>(~(1u<<(logical%8)));
}

struct ReadonlyAppendPlan {
    std::uint64_t original_size{};
    std::vector<std::pair<std::uint64_t,Bytes>> descriptors;
};

// This signal is consumed only by the standalone edit dispatcher.
// Inspection and all bridge callers still see an explicit Type-2 refusal.
struct VatWriteRequired : EditUnsupported {
    using EditUnsupported::EditUnsupported;
};

class Type1 {
public:
    explicit Type1(const std::filesystem::path& path,bool allow_bridge=false,
                   bool bounded_partitioned_bridge=false)
        :file_(path,std::ios::binary),size_(std::filesystem::file_size(path)),
         bounded_partitioned_bridge_(bounded_partitioned_bridge) {
        need(!bounded_partitioned_bridge || allow_bridge,
             "partitioned UDF writes require an ISO bridge validator");
        need(file_.good() && size_%block==0 && size_>=300*block &&
             size_<max_image,"unsupported UDF image size/sector geometry");
        const auto system=read(0,16*block);
        need(bounded_partitioned_bridge ?
                 (system[510]==0x55 && system[511]==0xaa) :
                 std::all_of(system.begin(),system.end(),
                             [](std::uint8_t x){return x==0;}),
             "UDF system-area hybrid image requires validated partition protection");
        bool vrs=false;
        for(std::uint64_t i=16;i<256 && (i+1)*block<=size_;++i) {
            const auto b=read(i*block,8);
            if(std::equal(b.begin()+1,b.begin()+6,"CD001") && !allow_bridge)
                throw EditUnsupported("ISO/UDF Bridge writes need protected ISO validation");
            if(std::equal(b.begin()+1,b.begin()+6,"NSR02") ||
               std::equal(b.begin()+1,b.begin()+6,"NSR03"))
                vrs=true;
        }
        need(vrs,"UDF-only Type-1 editing requires a volume recognition sequence");
        const auto anchor=read(256*block,block);
        tag_check(anchor,0,2);
        need(u32(anchor,12)==256,"unexpected UDF anchor tag location");
        const auto len=u32(anchor,16);
        const auto start=u32(anchor,20);
        need(len>=block && len<=16*1024*1024 &&
             std::uint64_t(start)*block<size_ &&
             len<=size_-std::uint64_t(start)*block,
             "invalid UDF main descriptor sequence");
        Bytes pd,lvd;
        for(std::uint64_t i=0;i<(len+block-1)/block;++i) {
            const auto b=read((std::uint64_t(start)+i)*block,block);
            const auto tag=u16(b,0);
            tag_check(b,0,tag);
            if(tag==5) {need(pd.empty(),"multiple UDF partitions unsupported");pd=b;}
            if(tag==6) {need(lvd.empty(),"multiple UDF logical volumes unsupported");lvd=b;}
            if(tag==8) break;
        }
        // Identify recognized Type-2 maps before falling back to the
        // generic Type-1 rejection. This is a read-only diagnostic, not
        // permission to write through virtual or remapped logical LBAs.
        if(!lvd.empty()) {
            const auto bytes=u32(lvd,264), count=u32(lvd,268);
            if(bytes<=lvd.size()-440 && count<=64) {
                std::size_t at=440;
                for(std::uint32_t index=0;index<count;++index) {
                    const auto end=std::size_t(440)+bytes;
                    if(at>end || end-at<2)break;
                    const auto length=lvd[at+1];
                    if(length<2 || length>end-at)break;
                    if(lvd[at]==2 && length==64) {
                        const std::string_view identifier(
                            reinterpret_cast<const char*>(lvd.data()+at+5),23);
                        if(identifier.substr(0,22)=="*UDF Virtual Partition")
                            throw VatWriteRequired("UDF VAT virtual partition requires a validated "
                                "standalone sequential append transaction");
                        if(identifier=="*UDF Sparable Partition")
                            throw EditUnsupported("UDF Sparable Partition is read-only; "
                                "packet-aware allocator not yet supported");
                        if(identifier=="*UDF Metadata Partition")
                            throw EditUnsupported("UDF Metadata Partition is read-only; "
                                "metadata/mirror allocator not yet supported");
                    }
                    at+=length;
                }
            }
        }
        need(!pd.empty() && !lvd.empty() && u32(lvd,212)==block &&
             u32(lvd,264)==6 && u32(lvd,268)==1 &&
             lvd[440]==1 && lvd[441]==6 &&
             u16(lvd,444)==u16(pd,22),
             "only single physical UDF Type-1 partition maps are writable");
        partition_=u32(pd,188);
        partition_blocks_=u32(pd,192);
        partition_access_type_=u32(pd,184);
        need(partition_blocks_>2 && partition_<size_/block &&
             partition_blocks_<=size_/block-partition_,
             "UDF partition bounds outside image");
        // UDF permits two independent free-space encodings: a bitmap
        // (tag 264) and an Unallocated Space Table/Entry (tag 263).
        // Only ONE explicitly validated unallocated set may authorize F5
        // creation. Freed-space sets, VAT and ambiguous mixed layouts are
        // intentionally read-only. Do not guess free blocks from zero data.
        const auto table_extent=u32(pd,56);
        const auto table_length=table_extent&0x3fffffffu;
        const auto table_logical=u32(pd,60);
        const auto bitmap_extent=u32(pd,64);
        const auto bitmap_length=bitmap_extent&0x3fffffffu;
        const auto bitmap_logical=u32(pd,68);
        const auto bitmap_span=
            (std::uint64_t(bitmap_length)+block-1u)/block*block;
        const auto conflicting_allocators=
            u32(pd,72)!=0 || u32(pd,80)!=0 || u32(pd,88)!=0;
        // Only numeric partition metadata is exposed in an anonymous WCX
        // failure. No paths, filenames, SIDs or image contents.
        allocation_status_=
            " (unallocated_table_bytes="+std::to_string(table_length)+
            ", unallocated_bitmap_bytes="+std::to_string(bitmap_length)+
            ", integrity_table_bytes="+std::to_string(u32(pd,72)&0x3fffffffu)+
            ", freed_table_bytes="+std::to_string(u32(pd,80)&0x3fffffffu)+
            ", freed_bitmap_bytes="+std::to_string(u32(pd,88)&0x3fffffffu)+
            ", partition_access_type="+std::to_string(u32(pd,184))+")";
        if(table_length || bitmap_length) {
            need((u32(pd,184)==3 || u32(pd,184)==4),
                 "UDF partition is not declared rewritable/overwritable");
            need(!conflicting_allocators &&
                 !(table_length && bitmap_length),
                 "UDF has conflicting or multiple free-space allocators");
        }
        if(bitmap_length) {
            need((bitmap_extent>>30u)==0 &&
                 bitmap_length>=24 && bitmap_span<=max_dir &&
                 bitmap_logical<partition_blocks_ &&
                 bitmap_span/block<=partition_blocks_-bitmap_logical,
                 "unsupported UDF bitmap free-space extent");
            Bitmap bm;
            bm.logical=bitmap_logical;
            bm.at=physical(bitmap_logical);
            bm.bytes=read(bm.at,static_cast<std::size_t>(bitmap_span));
            tag_check(bm.bytes,0,264);
            need(u32(bm.bytes,12)==bitmap_logical,
                 "incorrect UDF bitmap descriptor location");
            bm.bits=u32(bm.bytes,16);
            bm.byte_count=u32(bm.bytes,20);
            need(bm.bits==partition_blocks_ && bm.bits>0 &&
                 bm.byte_count==(std::uint64_t(bm.bits)+7u)/8u &&
                 24ull+bm.byte_count<=bitmap_length &&
                 24ull+bm.byte_count<=bitmap_span,
                 "invalid or oversized UDF bitmap size/count");
            if(bm.bits%8u) {
                const auto forbidden=static_cast<std::uint8_t>(
                    0xffu << (bm.bits%8u));
                need((bm.bytes[24u+bm.byte_count-1u]&forbidden)==0,
                     "UDF bitmap has free bits beyond partition end");
            }
            const auto tagged=16u+u16(bm.bytes,10);
            need(tagged>=24u && tagged<=24ull+bm.byte_count,
                 "UDF bitmap has unsupported descriptor CRC coverage");
            bitmap_=std::move(bm);
            reserved_.push_back({{bitmap_->at,bitmap_span},0});
        } else if(table_length) {
            // UDF 2.60 2.3.7 limits one USE to a logical block.
            // Reject AED continuation or long/multiple allocation tables;
            // table rewriting can never spill into another descriptor.
            need((table_extent>>30u)==0 && table_length>=40u &&
                 table_length<=block && table_logical<partition_blocks_,
                 "unsupported UDF Unallocated Space Table extent");
            const auto at=physical(table_logical);
            UnallocatedTable table;
            table.at=at;
            table.declared_length=table_length;
            table.bytes=read(at,block);
            tag_check(table.bytes,0,263);
            need(u32(table.bytes,12)==table_logical &&
                 table.bytes[27]==1u && (u16(table.bytes,34)&7u)==0u,
                 "unsupported UDF Unallocated Space Entry header");
            const auto descriptor_bytes=u32(table.bytes,36);
            need(descriptor_bytes%8u==0u &&
                 descriptor_bytes<=table_length-40u &&
                 descriptor_bytes<=block-40u &&
                 16u+u16(table.bytes,10)>=40u+descriptor_bytes,
                 "unsupported UDF Unallocated Space Entry ShortAD layout");
            // A synthesized bitmap is in-memory only. It is never written
            // as a tag-264 descriptor on a table-backed UDF volume.
            const auto bit_count=(std::uint64_t(partition_blocks_)+7u)/8u;
            need(24u+bit_count<=max_dir,
                 "UDF Unallocated Space Table exceeds virtual map safety budget");
            Bitmap virtual_map;
            virtual_map.bits=partition_blocks_;
            virtual_map.byte_count=static_cast<std::uint32_t>(bit_count);
            virtual_map.bytes.assign(
                static_cast<std::size_t>(24u+bit_count),0u);
            std::uint32_t previous_end=0;
            for(std::size_t p=40u;p<40u+descriptor_bytes;p+=8u) {
                const auto raw=u32(table.bytes,p);
                const auto extent_bytes=raw&0x3fffffffu;
                const auto first=u32(table.bytes,p+4u);
                need((raw>>30u)==1u && extent_bytes>0u &&
                     extent_bytes%block==0u &&
                     first<partition_blocks_ &&
                     extent_bytes/block<=partition_blocks_-first &&
                     (p==40u || first>previous_end),
                     "UDF Unallocated Space Table has invalid or overlapping ShortAD");
                const auto count=extent_bytes/block;
                for(std::uint32_t q=0;q<count;++q) {
                    const auto logical=first+q;
                    virtual_map.bytes[24u+logical/8u] |=
                        static_cast<std::uint8_t>(1u<<(logical%8u));
                }
                previous_end=first+static_cast<std::uint32_t>(count);
            }
            // No unknown bytes may be overwritten in the existing USE
            // capacity during a future transactional descriptor rewrite.
            need(std::all_of(
                    table.bytes.begin()+static_cast<std::ptrdiff_t>(
                        40u+descriptor_bytes),
                    table.bytes.begin()+static_cast<std::ptrdiff_t>(
                        table_length),
                    [](std::uint8_t x){return x==0;}),
                 "UDF Unallocated Space Table contains nonzero unused metadata");
            table_=std::move(table);
            bitmap_=std::move(virtual_map);
            reserved_.push_back({{at,block},0});
        }
        if(bitmap_) {
            const auto integrity_length=u32(lvd,432);
            const auto integrity_location=u32(lvd,436);
            // UDF integrity *sequence* can contain an LVID plus a
            // Terminating Descriptor and unused sectors. A real mkudffs
            // Type-1 image commonly allocates more than one block here.
            need(integrity_length>=block &&
                 integrity_length<=16u*1024u*1024u &&
                 integrity_length%block==0 &&
                 integrity_location<size_/block &&
                 integrity_length<=size_-std::uint64_t(integrity_location)*block,
                 "UDF LVID sequence outside optical image");
            const auto integrity_count=integrity_length/block;
            Integrity integrity;
            bool found_lvid=false;
            bool terminated=false;
            for(std::uint64_t i=0;i<integrity_count;++i) {
                const auto lba=std::uint64_t(integrity_location)+i;
                auto descriptor=read(lba*block,block);
                const auto type=u16(descriptor,0);
                if(type==0) {
                    // Padding may appear only after an LVID or terminator.
                    need(found_lvid,
                         "empty UDF integrity sequence before LVID");
                    break;
                }
                need(!terminated,"UDF descriptor follows integrity terminator");
                tag_check(descriptor,0,type);
                need(u32(descriptor,12)==lba,
                     "UDF integrity descriptor tag-location mismatch");
                if(type==8) {
                    terminated=true;
                    break;
                }
                need(type==9 && !found_lvid,
                     "unsupported chained/multiple UDF integrity descriptors");
                integrity.at=lba*block;
                integrity.bytes=std::move(descriptor);
                found_lvid=true;
            }
            need(found_lvid &&
                 u32(integrity.bytes,28)==1 &&
                 u32(integrity.bytes,72)==1,
                 "UDF allocator requires a closed single-partition LVID");
            integrity.impl_bytes=u32(integrity.bytes,76);
            need((integrity.impl_bytes==0 || integrity.impl_bytes>=40) &&
                 88ull+integrity.impl_bytes<=block &&
                 u32(integrity.bytes,84)==partition_blocks_,
                 "invalid UDF LVID partition/integrity counters");
            integrity.next_id=u64(integrity.bytes,40);
            need(integrity.next_id>=16 &&
                 integrity.next_id<std::numeric_limits<std::uint64_t>::max(),
                 "unsupported UDF LVID unique-ID sequence");
            integrity_=std::move(integrity);
            // Every integrity-sequence sector remains reserved even when
            // only the first descriptor is modified.
            reserved_.push_back({{std::uint64_t(integrity_location)*block,
                                   integrity_length},0});
        }
        // The Logical Volume Contents Use field is a long_ad. Its top
        // two extent-length bits describe recording status, not size.
        // Reject unmapped/unrecorded or redirected FSD extents before
        // interpreting their logical location as a physical Type-1 sector.
        const auto fsd_extent=u32(lvd,248);
        const auto fsd_type=fsd_extent>>30u;
        const auto fsd_size=fsd_extent&0x3fffffffu;
        const auto fsd_partition_ref=u16(lvd,256);
        if(fsd_type!=0)
            throw EditUnsupported(
                "unsupported UDF file-set extent type (type="+
                std::to_string(fsd_type)+"; only recorded extent 0 writable)");
        // A valid File Set Descriptor *sequence* can occupy two 2048-byte
        // logical blocks (4096 total): FSD followed by a Terminating
        // Descriptor, or one zero-padded block. The original single-block
        // restriction rejected real ISO/UDF images before edit preflight.
        // Never accept arbitrary data in the second block or confuse it
        // with another FSD/root directory descriptor.
        if(fsd_size<416 || fsd_size>2u*block ||
           (fsd_size>block && fsd_size!=2u*block))
            throw EditUnsupported(
                "unsupported UDF file-set descriptor length (bytes="+
                std::to_string(fsd_size)+
                "; expected 416..2048 or a complete 4096-byte sequence)");
        if(fsd_partition_ref!=0)
            throw EditUnsupported(
                "unsupported UDF file-set partition reference (number="+
                std::to_string(fsd_partition_ref)+
                "; only the single physical Type-1 partition 0 writable)");
        const auto fsd_logical=u32(lvd,252);
        const auto fsd_blocks=(std::uint64_t(fsd_size)+block-1u)/block;
        need(fsd_logical<partition_blocks_ &&
             fsd_blocks<=partition_blocks_-fsd_logical,
             "UDF file-set sequence exceeds Type-1 partition");
        const auto fsd_start=physical(fsd_logical);
        const auto fsd=read(fsd_start,block);
        tag_check(fsd,0,256);
        if(fsd_blocks==2) {
            const auto tail=read(fsd_start+block,block);
            const bool blank=std::all_of(
                tail.begin(),tail.end(),[](std::uint8_t c){return c==0;});
            if(!blank) {
                need(u16(tail,0)==8,
                     "unsupported UDF file-set sequence tail (expected terminator or zero block)");
                tag_check(tail,0,8);
                const auto used=16u+u16(tail,10);
                need(std::all_of(tail.begin()+used,tail.end(),
                         [](std::uint8_t c){return c==0;}),
                     "unsupported UDF file-set terminator trailing data");
            }
        }
        reserved_.push_back({{256*block,block},0});
        reserved_.push_back({{std::uint64_t(start)*block,len},0});
        // Reserve both sectors, including the terminator/padding, in the
        // allocator and the before/after layout protection checks.
        reserved_.push_back({{fsd_start,fsd_blocks*block},0});
        walk(u32(fsd,404),u16(fsd,408),"",0);
        if(bitmap_) {
            std::uint64_t free_bits=0;
            for(std::uint32_t i=0;i<bitmap_->bits;++i)
                if(bitmap_free(*bitmap_,i))++free_bits;
            need(integrity_ &&
                 u32(integrity_->bytes,80)==free_bits,
                 "UDF free-space allocator and LVID counters disagree");
            // A bitmap advertising an existing object or descriptor as
            // free is contradictory: fail closed before any new writes.
            for(const auto& occupied:reserved_) {
                const auto first=occupied.range.at;
                const auto last=first+occupied.range.bytes;
                if(first>=std::uint64_t(partition_)*block &&
                   last<=(std::uint64_t(partition_)+partition_blocks_)*block) {
                    const auto logical=static_cast<std::uint32_t>(
                        (first-std::uint64_t(partition_)*block)/block);
                    const auto count=(occupied.range.bytes+block-1)/block;
                    for(std::uint64_t i=0;i<count;++i)
                        need(!bitmap_free(*bitmap_,logical+
                             static_cast<std::uint32_t>(i)),
                             "UDF allocation map overlaps a referenced file or descriptor");
                }
            }
        }
    }

    [[nodiscard]] const std::map<std::string,File>& files() const noexcept {
        return files_;
    }
    [[nodiscard]] const std::vector<Interval>& intervals() const noexcept {
        return reserved_;
    }
    [[nodiscard]] const std::map<std::string,Fe>& directories() const noexcept {
        return directory_files_;
    }
    // Root has no parent link and is never deletable.
    [[nodiscard]] const std::map<std::string,File>& directory_links() const noexcept {
        return directory_links_;
    }
    [[nodiscard]] Bytes directory_payload(const Fe& directory) {
        return directory_data(directory);
    }
    [[nodiscard]] bool bounded_partitioned_bridge() const noexcept {
        return bounded_partitioned_bridge_;
    }
    [[nodiscard]] bool has_bitmap() const noexcept {
        return bitmap_.has_value() && !table_.has_value();
    }
    [[nodiscard]] bool has_allocation_map() const noexcept {
        return bitmap_.has_value();
    }
    [[nodiscard]] bool has_unallocated_table() const noexcept {
        return table_.has_value();
    }
    [[nodiscard]] std::uint32_t partition_access_type() const noexcept {
        return partition_access_type_;
    }
    [[nodiscard]] std::string missing_allocation_map_reason() const {
        if(partition_access_type_==1)
            return "UDF partition access type 1 is read-only and has "
                   "no unallocated-space bitmap or table; adding files "
                   "requires a coordinated ISO/UDF image reauthoring"+
                   allocation_status_;
        if(partition_access_type_==2)
            return "UDF partition access type 2 is write-once and has "
                   "no unallocated-space bitmap or table; the native "
                   "Type-1 in-place allocator cannot create files"+
                   allocation_status_;
        return "UDF create needs a validated unallocated bitmap or "
               "Unallocated Space Table"+allocation_status_;
    }
    // ISO/UDF Bridge: ISO-owned sectors may appear free in the UDF bitmap.
    // Exclude them only during this transaction, without misreporting the
    // original bitmap or the independent LVID free-block accounting.
    void protect_external_ranges(const EditProtectedRanges& physical) {
        external_blocks_.clear();
        const auto base=std::uint64_t(partition_)*block;
        const auto limit=base+std::uint64_t(partition_blocks_)*block;
        need(physical.size()<=max_items*2u,
             "ISO/UDF protected range count exceeds safety limit");
        for(const auto& [at,length]:physical) {
            need(at<=size_ && length<=size_-at,
                 "ISO/UDF protected range exceeds image size");
            if(!length)continue;
            const auto first=std::max(at,base);
            const auto last=std::min(at+length,limit);
            if(first>=last)continue;
            const auto start=(first-base)/block;
            const auto end=(last-base+block-1u)/block;
            need(end<=partition_blocks_ && start<end,
                 "ISO/UDF protected range has invalid partition bounds");
            external_blocks_.emplace_back(
                static_cast<std::uint32_t>(start),
                static_cast<std::uint32_t>(end));
        }
        std::sort(external_blocks_.begin(),external_blocks_.end());
        std::vector<std::pair<std::uint32_t,std::uint32_t>> merged;
        for(const auto& range:external_blocks_) {
            if(!merged.empty() && range.first<=merged.back().second)
                merged.back().second=std::max(merged.back().second,range.second);
            else merged.push_back(range);
        }
        external_blocks_=std::move(merged);
    }
    [[nodiscard]] bool overlaps_external(std::uint64_t at,
                                          std::uint64_t length) const {
        if(!length)return false;
        need(at<=size_ && length<=size_-at,
             "UDF candidate exceeds image boundary");
        const auto base=std::uint64_t(partition_)*block;
        const auto limit=base+std::uint64_t(partition_blocks_)*block;
        const auto first=std::max(at,base);
        const auto last=std::min(at+length,limit);
        if(first>=last)return false;
        const auto begin=static_cast<std::uint32_t>((first-base)/block);
        const auto end=static_cast<std::uint32_t>((last-base+block-1u)/block);
        const auto it=std::lower_bound(
            external_blocks_.begin(),external_blocks_.end(),begin,
            [](const auto& interval,std::uint32_t pos) {
                return interval.second<=pos;
            });
        return it!=external_blocks_.end() && it->first<end;
    }
    [[nodiscard]] bool allocatable_block(std::uint32_t logical) const {
        need(bitmap_.has_value(),"UDF allocator requires bitmap");
        return bitmap_free(*bitmap_,logical) &&
               !overlaps_external(
                   (std::uint64_t(partition_)+logical)*block,block);
    }
    [[nodiscard]] bool is_free(std::uint32_t logical) const {
        need(bitmap_.has_value(),"UDF no bitmap for free-space verification");
        return bitmap_free(*bitmap_,logical);
    }
    [[nodiscard]] std::uint64_t image_size() const noexcept {return size_;}
    [[nodiscard]] std::uint32_t part_start() const noexcept {return partition_;}
    // A single ShortAD requires one contiguous allocation. Reserve a
    // validated run before writing any staged file content; bitmap changes
    // stay in memory until the entire image transaction is committed.
    [[nodiscard]] std::uint32_t allocate_blocks(std::uint32_t count) {
        need(bitmap_.has_value(),
             "UDF creation requires a validated bitmap or Unallocated Space Table");
        need(count>0 && count<=partition_blocks_,
             "UDF requested allocation exceeds partition");
        auto& bitmap=*bitmap_;
        for(std::uint32_t start=allocation_start_;start<bitmap.bits;) {
            if(!allocatable_block(start)){++start;continue;}
            std::uint32_t run=0;
            while(run<count && run<bitmap.bits-start &&
                  allocatable_block(start+run))++run;
            if(run==count) {
                const Location candidate{
                    (std::uint64_t(partition_)+start)*block,
                    std::uint64_t(count)*block};
                for(const auto& object:reserved_)
                    need(!intersects(candidate,object.range),
                         "UDF bitmap contradicts an occupied extent");
                for(std::uint32_t i=0;i<count;++i)
                    bitmap_reserve(bitmap,start+i);
                reserved_.push_back({candidate,0});
                return start;
            }
            start+=run+1;
        }
        throw EditUnsupported(external_blocks_.empty()?
            "UDF bitmap has no sufficiently long free run":
            "UDF Bridge has no safe contiguous run outside immutable ISO/boot extents");
    }
    [[nodiscard]] std::uint32_t allocate_block() {
        return allocate_blocks(1);
    }
    // Keep only the largest candidate free runs. UDF's 30-bit ShortAD length
    // is split on aligned block boundaries; one FE has finitely many AD slots.
    [[nodiscard]] std::vector<DataRun> allocate_file_data(
        std::uint64_t length,std::size_t ad_limit) {
        need(bitmap_.has_value() && integrity_.has_value() && length>0 &&
             ad_limit>0,"UDF multi-extent allocator is unavailable");
        constexpr std::uint32_t max_extent_blocks=0x3fffffffu/block;
        const auto blocks_needed=(length+block-1u)/block;
        need(blocks_needed<=partition_blocks_ &&
             blocks_needed<=u32(integrity_->bytes,80),
             "UDF large file exceeds available partition blocks");
        using Candidate=std::pair<std::uint32_t,std::uint32_t>; // count, LBA
        std::priority_queue<Candidate,std::vector<Candidate>,
                            std::greater<Candidate>> best;
        const auto& bitmap=*bitmap_;
        for(std::uint32_t at=allocation_start_;at<bitmap.bits;) {
            if(!allocatable_block(at)) {++at;continue;}
            const auto start=at;
            do {++at;} while(at<bitmap.bits && allocatable_block(at));
            const Candidate candidate{at-start,start};
            if(best.size()<ad_limit) best.push(candidate);
            else if(candidate.first>best.top().first) {
                best.pop();best.push(candidate);
            }
        }
        std::vector<Candidate> available;
        available.reserve(best.size());
        while(!best.empty()) {available.push_back(best.top());best.pop();}
        std::sort(available.begin(),available.end(),
                  [](const Candidate& a,const Candidate& b) {
                      return a.first>b.first ||
                             (a.first==b.first && a.second<b.second);
                  });
        std::vector<DataRun> planned;
        std::uint64_t remaining_blocks=blocks_needed,remaining_bytes=length;
        for(const auto& [free_blocks,free_lba]:available) {
            std::uint32_t used=0;
            while(remaining_blocks && used<free_blocks) {
                need(planned.size()<ad_limit,
                     "UDF fragmented file exceeds FE ShortAD capacity");
                const auto count=static_cast<std::uint32_t>(
                    std::min<std::uint64_t>(
                        {remaining_blocks,free_blocks-used,max_extent_blocks}));
                const auto bytes=std::min(remaining_bytes,
                                          std::uint64_t(count)*block);
                need(count>0 && bytes>0 && bytes<=0x3fffffffull,
                     "invalid UDF ShortAD file run");
                const auto logical=free_lba+used;
                planned.push_back({physical(logical),bytes,logical,count});
                remaining_blocks-=count;
                remaining_bytes-=bytes;
                used+=count;
            }
            if(!remaining_blocks)break;
        }
        need(remaining_blocks==0 && remaining_bytes==0,
             external_blocks_.empty()?
             "UDF file cannot fit in the free bitmap/ShortAD slots":
             "UDF Bridge fragmented file cannot fit outside immutable ISO/boot extents");
        for(const auto& run:planned) {
            const Location target{run.at,std::uint64_t(run.blocks)*block};
            for(const auto& occupied:reserved_)
                need(!intersects(target,occupied.range),
                     "UDF bitmap disagrees with an occupied run");
        }
        for(const auto& run:planned) {
            for(std::uint32_t i=0;i<run.blocks;++i)
                bitmap_reserve(*bitmap_,run.logical+i);
            reserved_.push_back({{run.at,std::uint64_t(run.blocks)*block},0});
        }
        return planned;
    }
    // Reclaim a directory's superseded contiguous ShortAD allocation.
    // Reject hard links, aliasing and any overlap with unrelated descriptors.
    // This is only called after a separate replacement run was reserved.
    void release_directory_blocks(std::uint64_t owner,
                                  std::uint64_t data_at,
                                  std::uint32_t count) {
        need(bitmap_.has_value(),"UDF directory release requires a validated allocation map");
        const auto start=logical_of(data_at);
        need(count>0 && count<=partition_blocks_-start,
             "UDF directory release exceeds partition");
        const Location target{data_at,std::uint64_t(count)*block};
        unsigned ownership=0;
        for(const auto& interval:reserved_) {
            if(!intersects(target,interval.range))continue;
            need(interval.owner==owner &&
                 interval.range.at==target.at &&
                 interval.range.bytes==target.bytes,
                 "UDF old directory sectors overlap another extent");
            ++ownership;
        }
        need(ownership==1,"UDF directory allocation is not uniquely owned");
        for(std::uint32_t i=0;i<count;++i)
            need(!bitmap_free(*bitmap_,start+i),
                 "UDF old directory sector was already free");
        for(std::uint32_t i=0;i<count;++i)
            bitmap_->bytes[24u+(start+i)/8u] |=
                static_cast<std::uint8_t>(1u<<((start+i)%8u));
        reserved_.erase(std::remove_if(reserved_.begin(),reserved_.end(),
            [&](const Interval& interval) {
                return interval.owner==owner &&
                       interval.range.at==target.at &&
                       interval.range.bytes==target.bytes;
            }),reserved_.end());
    }
    // Reclaim a uniquely owned regular file or EMPTY directory. Its parent
    // FID must first be tombstoned. Blocks stay unavailable in this batch.
    [[nodiscard]] std::vector<Location> reclaim_entry_blocks(
            const File& file,bool directory,bool preserve=false) {
        need(bitmap_ && integrity_,"UDF reclamation needs a closed allocation map/LVID");
        const auto owner=file.fe.at;
        const auto found=references_.find(owner);
        need(file.fe.type==(directory?4u:5u) &&
             found!=references_.end() && found->second==1 &&
             !deleted_icbs_.count(owner),
             "UDF entry has aliases or dangling deleted FID references");
        std::vector<Location> reclaimed{{owner,block}};
        for(const auto& run:file.fe.runs) {
            need(run.blocks && run.recorded &&
                 run.recorded<=std::uint64_t(run.blocks)*block,
                 "UDF reclaim has invalid ShortAD sector geometry");
            reclaimed.push_back({run.at,std::uint64_t(run.blocks)*block});
        }
        // Preflight the ENTIRE set before changing any bitmap bit.
        for(std::size_t i=0;i<reclaimed.size();++i) {
            const auto& range=reclaimed[i];
            need(range.bytes && range.bytes%block==0,
                 "UDF reclaim has non-sector-aligned extent");
            const auto first=logical_of(range.at);
            const auto count=range.bytes/block;
            need(count<=partition_blocks_-first,
                 "UDF reclaim exceeds partition");
            for(std::size_t j=0;j<i;++j)
                need(!intersects(range,reclaimed[j]),
                     "UDF reclaim extents overlap");
            unsigned matches=0;
            for(const auto& occupied:reserved_) {
                if(!intersects(range,occupied.range))continue;
                need(occupied.owner==owner &&
                     occupied.range.at==range.at &&
                     occupied.range.bytes==range.bytes,
                     "UDF reclaim crosses another referenced descriptor");
                ++matches;
            }
            need(matches==1,"UDF reclaim extent lacks unique ownership");
            for(std::uint64_t j=0;j<count;++j)
                need(!bitmap_free(*bitmap_,first+static_cast<std::uint32_t>(j)),
                     "UDF reclaim sector is already free");
        }
        // A coordinated ROM deletion unlinks the FID but leaves the
        // former FE and shared data immutable. Ownership was still
        // checked above; no reclaimed sectors enter the virtual bitmap.
        if(preserve)return {};
        for(const auto& range:reclaimed) {
            const auto first=logical_of(range.at);
            for(std::uint64_t j=0;j<range.bytes/block;++j) {
                const auto logical=first+static_cast<std::uint32_t>(j);
                bitmap_->bytes[24u+logical/8u] |=
                    static_cast<std::uint8_t>(1u<<(logical%8u));
            }
        }
        reserved_.erase(std::remove_if(reserved_.begin(),reserved_.end(),
            [&](const Interval& interval) {
                return interval.owner==owner &&
                       std::any_of(reclaimed.begin(),reclaimed.end(),
                            [&](const Location& range) {
                                return interval.range.at==range.at &&
                                       interval.range.bytes==range.bytes;
                            });
            }),reserved_.end());
        return reclaimed;
    }
    [[nodiscard]] std::pair<std::uint64_t,Bytes> updated_bitmap() const {
        need(bitmap_.has_value(),"UDF allocation map is absent");
        if(table_) {
            // Only serialize the virtual bitmap back into the ORIGINAL
            // tag-263 UnallocatedSpaceEntry ShortAD array, not tag 264.
            // Fail closed if the updated free-run list won't fit within
            // that descriptor's preexisting, bounded partition extent.
            auto b=table_->bytes;
            const auto span=table_->declared_length;
            need(span>=40u && span<=block && b.size()==block,
                 "UDF Unallocated Space Entry staging length changed");
            std::fill(b.begin()+40,
                      b.begin()+static_cast<std::ptrdiff_t>(span),
                      std::uint8_t{0});
            constexpr std::uint32_t max_run=0x3fffffffu/block;
            std::size_t cursor=40;
            for(std::uint32_t first=0;first<bitmap_->bits;) {
                if(!bitmap_free(*bitmap_,first)){++first;continue;}
                std::uint32_t count=0;
                while(count<max_run && count<bitmap_->bits-first &&
                      bitmap_free(*bitmap_,first+count))++count;
                need(count>0 && cursor<=span && span-cursor>=8u,
                     "UDF Unallocated Space Table has no room for a new free extent");
                put32(b,cursor,0x40000000u|
                    static_cast<std::uint32_t>(std::uint64_t(count)*block));
                put32(b,cursor+4u,first);
                cursor+=8u;
                first+=count;
            }
            put32(b,36,static_cast<std::uint32_t>(cursor-40u));
            tag_update(b,0,cursor);
            tag_check(b,0,263);
            return {table_->at,std::move(b)};
        }
        auto b=bitmap_->bytes;
        // Preserve original bitmap tag's CRC coverage. The bitmap's data
        // bit changes may lie outside that descriptor CRC by design.
        tag_update(b,0,16u+u16(b,10));
        return {bitmap_->at,std::move(b)};
    }
    // The closed LVID is changed once for the entire transaction.
    // Free space is signed: deletions can reclaim more blocks than creations
    // reserve. NextUniqueID is never decremented or recycled.
    [[nodiscard]] std::pair<std::uint64_t,Bytes>
    updated_integrity_delta(std::uint32_t files,std::uint32_t dirs,
                            std::uint32_t deleted_files,
                            std::uint32_t deleted_dirs,
                            std::uint64_t allocated,
                            std::uint64_t reclaimed) const {
        need(integrity_.has_value(),"UDF LVID is absent");
        auto b=integrity_->bytes;
        const auto entries=std::uint64_t(files)+dirs;
        need(entries<=
                 std::numeric_limits<std::uint64_t>::max()-integrity_->next_id,
             "UDF unique-ID sequence overflow");
        const auto previous_free=std::uint64_t(u32(b,80));
        need(allocated<=previous_free &&
             reclaimed<=std::numeric_limits<std::uint32_t>::max() &&
             reclaimed<=std::numeric_limits<std::uint32_t>::max()-
                 (previous_free-allocated),
             "UDF LVID free-space delta is invalid");
        put32(b,80,static_cast<std::uint32_t>(
            previous_free-allocated+reclaimed));
        put64(b,40,integrity_->next_id+entries);
        if(integrity_->impl_bytes>=40) {
            const auto old_files=u32(b,120);
            const auto old_dirs=u32(b,124);
            need(deleted_files<=old_files &&
                 std::uint64_t(old_files-deleted_files)+files<=
                     std::numeric_limits<std::uint32_t>::max() &&
                 deleted_dirs<=old_dirs &&
                 std::uint64_t(old_dirs-deleted_dirs)+dirs<=
                     std::numeric_limits<std::uint32_t>::max(),
                 "UDF LVID file/directory count would overflow or underflow");
            put32(b,120,old_files-deleted_files+files);
            put32(b,124,old_dirs-deleted_dirs+dirs);
        }
        tag_update(b,0,88u+integrity_->impl_bytes);
        return {integrity_->at,std::move(b)};
    }
    [[nodiscard]] std::pair<std::uint64_t,Bytes>
    updated_integrity_batch(std::uint32_t files,std::uint32_t dirs,
                            std::uint32_t allocated_blocks) const {
        return updated_integrity_delta(files,dirs,0u,0u,allocated_blocks,0u);
    }
    [[nodiscard]] std::pair<std::uint64_t,Bytes>
    updated_integrity(bool directory,std::uint32_t reserved_blocks=1) const {
        return updated_integrity_batch(directory?0u:1u,
                                       directory?1u:0u,reserved_blocks);
    }
    [[nodiscard]] std::uint64_t new_unique_id(std::uint64_t index=0) const {
        need(integrity_.has_value() &&
             index<std::numeric_limits<std::uint64_t>::max()-integrity_->next_id,
             "UDF integrity unique-ID overflow");
        return integrity_->next_id+index;
    }
    [[nodiscard]] std::uint64_t absolute(std::uint32_t logical) const {
        return physical(logical);
    }
    [[nodiscard]] std::uint32_t logical_of(std::uint64_t at) const {
        need(at>=std::uint64_t(partition_)*block &&
             (at-std::uint64_t(partition_)*block)%block==0 &&
             (at-std::uint64_t(partition_)*block)/block<partition_blocks_,
             "UDF object not in Type-1 partition");
        return static_cast<std::uint32_t>(
            (at-std::uint64_t(partition_)*block)/block);
    }
    // Read-only mastering: only newly appended sectors are allocatable.
    // The virtual bitmap is never serialized into a read-only partition.
    // Relocate descriptor sequences and integrity data beyond the expanded
    // partition, preserving every original payload offset and ISO boot LBA.
    ReadonlyAppendPlan prepare_readonly_append(
            const std::vector<ArchiveChange>& changes,
            const EditProtectedRanges& protected_ranges,
            std::uint64_t iso_metadata_blocks=0,
            std::uint64_t mbr_append_alignment_blocks=0,
            std::uint64_t gpt_append_trailer_blocks=0) {
        need(partition_access_type_==1 && !bitmap_ && !changes.empty() &&
             changes.size()<=max_items,"unsupported read-only UDF mastering");
        ReadonlyAppendPlan plan;
        plan.original_size=size_;
        const auto old_sectors=size_/block;
        std::uint64_t extra=1;
        std::set<std::string> parents;
        for(const auto& change:changes) {
            const auto key=edit_path(change.path);
            const bool existing_file=files_.count(key)!=0;
            const bool existing_directory=directory_links_.count(key)!=0;
            need(!change.remove || existing_file || existing_directory,
                 "read-only UDF mastering cannot delete a nonexistent entry");
            need(change.remove ||
                 (!existing_directory && (!existing_file || !change.directory)),
                 "read-only UDF mastering cannot replace an existing directory");
            const auto slash=key.rfind('/');
            const auto parent=slash==std::string::npos?
                std::string{}:key.substr(0,slash);
            if(parents.insert(parent).second) {
                const auto it=directory_files_.find(parent);
                if(it!=directory_files_.end())
                    extra+=(it->second.size+block-1)/block+1;
            }
            // One FE, plus a conservative allowance for new FIDs and
            // directory relocation. File bytes are allocated separately.
            extra+=3;
            if(!change.remove && !change.directory) {
                need(std::filesystem::is_regular_file(
                        std::filesystem::symlink_status(change.source)),
                     "UDF mastering source must be a regular file");
                const auto bytes=std::filesystem::file_size(change.source);
                need(bytes<max_image,"UDF mastering source too large");
                extra+=(bytes+block-1)/block;
            }
            need(extra<max_image/block-old_sectors,
                 "UDF mastering exceeds optical address space");
        }
        const auto new_end=old_sectors+extra;
        const auto new_blocks=new_end-partition_;
        need(24+(new_blocks+7)/8<=max_dir,
             "UDF mastering virtual allocation map exceeds safety budget");
        auto anchor=read(256*block,block);
        const auto vds_bytes=u32(anchor,16);
        const auto vds_start=u32(anchor,20);
        need(vds_bytes%block==0 && vds_bytes>=3*block &&
             vds_bytes<=16u*1024u*1024u,
             "unsupported read-only UDF descriptor sequence");
        std::vector<Bytes> sequence;
        Bytes lvid;
        bool pd_found=false,lvd_found=false,terminated=false;
        for(std::uint64_t j=0;j<vds_bytes/block;++j) {
            auto d=read((std::uint64_t(vds_start)+j)*block,block);
            const auto type=u16(d,0);
            tag_check(d,0,type);
            need(u32(d,12)==vds_start+j,
                 "UDF mastering descriptor location mismatch");
            if(type==5) {
                need(!pd_found && u32(d,184)==1 &&
                     u32(d,188)==partition_ && u32(d,192)==partition_blocks_,
                     "ambiguous read-only UDF partition");
                for(std::size_t p=56;p<96;p+=4)
                    need(u32(d,p)==0,
                         "read-only UDF has conflicting partition allocation data");
                put32(d,192,static_cast<std::uint32_t>(new_blocks));
                pd_found=true;
            } else if(type==6) {
                need(!lvd_found,"multiple UDF mastering logical volumes");
                const auto n=u32(d,432),at=u32(d,436);
                need(n>=block && n%block==0 && n<=16u*1024u*1024u &&
                     at<old_sectors && n/block<=old_sectors-at,
                     "read-only UDF access type 1 mastering requires a bounded integrity sequence");
                lvid=read(std::uint64_t(at)*block,block);
                tag_check(lvid,0,9);
                need(u32(lvid,12)==at && u32(lvid,28)==1 &&
                     u32(lvid,32)==0 && u32(lvid,36)==0 &&
                     u32(lvid,72)==1,
                     "read-only UDF mastering requires closed single-partition integrity");
                const auto impl=u32(lvid,76);
                need((impl==0 || impl>=40) && 88ull+impl<=block &&
                     (u32(lvid,84)==partition_blocks_ ||
                      u32(lvid,84)==0xffffffffu),
                     "invalid read-only UDF integrity counters");
                bool ended=false;
                for(std::uint64_t j=1;j<n/block;++j) {
                    auto tail=read((std::uint64_t(at)+j)*block,block);
                    if(std::all_of(tail.begin(),tail.end(),
                            [](std::uint8_t c){return c==0;})) {
                        ended=true;
                        continue;
                    }
                    need(!ended && u16(tail,0)==8,
                         "unsupported chained read-only UDF integrity sequence");
                    tag_check(tail,0,8);
                    need(u32(tail,12)==std::uint64_t(at)+j,
                         "invalid UDF integrity terminator location");
                    ended=true;
                }
                lvd_found=true;
            } else if(type==7) {
                // No volume-space free extents are advertised after mastering.
                const auto count=u32(d,20);
                need(count<=(block-24)/8,
                     "unsupported UDF unallocated volume space descriptor");
                put32(d,20,0);
                std::fill(d.begin()+24,d.end(),std::uint8_t{0});
            } else need(type==1 || type==4 || type==8,
                        "unsupported UDF mastering volume descriptor");
            sequence.push_back(std::move(d));
            if(type==8) {terminated=true;break;}
        }
        need(pd_found && lvd_found && terminated,
             "incomplete UDF mastering descriptor sequence");
        // The coordinator's new ISO directory/path-table area lies
        // strictly after the UDF partition, but before the relocated VDS.
        // It is never represented by a free bit in the UDF virtual map.
        need(iso_metadata_blocks<=32768 &&
             iso_metadata_blocks<max_image/block-new_end,
             "ISO metadata reservation exceeds mastering budget");
        const auto main=new_end+iso_metadata_blocks;
        const auto reserve=main+vds_bytes/block;
        const auto integrity_lba=reserve+vds_bytes/block;
        auto last=integrity_lba+2+256;
        if(mbr_append_alignment_blocks) {
            // Keep the UDF physical-end anchors at N and N-256 after
            // rounding to the validated MBR CHS/optical-sector alignment.
            need(mbr_append_alignment_blocks<=65536 &&
                 old_sectors%mbr_append_alignment_blocks==0,
                 "invalid MBR/UDF append alignment");
            const auto aligned=((last+1+mbr_append_alignment_blocks-1)/
                 mbr_append_alignment_blocks)*mbr_append_alignment_blocks;
            need(aligned<=0xffffffffull/4,
                 "MBR/UDF growth exceeds 32-bit partition length");
            last=aligned-1;
        }
        // GPT reserves the last physical sector for its backup header.
        // The second UDF anchor must be exactly N-256 relative to the
        // FINAL disk, and must remain before the GPT trailer. ECMA-167
        // allows the two-anchor combination 256 and N-256.
        need(!gpt_append_trailer_blocks ||
             (!mbr_append_alignment_blocks && gpt_append_trailer_blocks<256),
             "GPT backup leaves no room for an independent UDF end anchor");
        need(last+1<max_image/block &&
             gpt_append_trailer_blocks<max_image/block-(last+1),
             "UDF mastering descriptor addresses exceed optical limit");
        for(std::size_t j=0;j<sequence.size();++j) {
            auto d=sequence[j];
            const auto type=u16(d,0);
            auto crc_bytes=std::size_t(16u+u16(d,10));
            if(type==5)crc_bytes=std::max<std::size_t>(crc_bytes,512);
            if(type==6) {
                put32(d,432,2*block);
                put32(d,436,static_cast<std::uint32_t>(integrity_lba));
                crc_bytes=std::max<std::size_t>(crc_bytes,446);
            }
            if(type==7)crc_bytes=24;
            for(const auto start:{main,reserve}) {
                auto copy=d;
                put32(copy,12,static_cast<std::uint32_t>(start+j));
                tag_update(copy,0,crc_bytes);
                plan.descriptors.emplace_back((start+j)*block,std::move(copy));
            }
        }
        // The main/reserve copies are both reachable from all original
        // valid anchors, as well as the new physical-end anchor pair.
        std::set<std::uint64_t> anchors{256};
        if(gpt_append_trailer_blocks)
            anchors.insert(last+gpt_append_trailer_blocks-256);
        else {
            anchors.insert(last-256);
            anchors.insert(last);
        }
        // Physical-end discovery is independent of any ISO namespace.
        // Standalone UDF and padded bridge volumes share this backend.
        std::set<std::uint64_t> candidates{256,512,old_sectors-1,
                                           old_sectors-257,old_sectors-256};
        for(const auto& [at,length]:protected_ranges)
            if(length==block && at%block==0 && at>=256*block &&
               at<plan.original_size)candidates.insert(at/block);
        for(const auto lba:candidates) {
            if(lba>=old_sectors)continue;
            const auto at=lba*block;
            auto d=read(at,block);
            if(u16(d,0)!=2 || u32(d,12)!=lba)continue;
            tag_check(d,0,2);
            need(u32(d,16)==u32(anchor,16) && u32(d,20)==u32(anchor,20) &&
                 u32(d,24)==u32(anchor,24) && u32(d,28)==u32(anchor,28),
                 "inconsistent UDF anchor copies");
            for(const auto& occupied:reserved_)
                need(!occupied.owner ||
                     !intersects({at,block},occupied.range),
                     "UDF mastering anchor overlaps a referenced file or directory");
            anchors.insert(lba);
        }
        for(const auto at:anchors) {
            auto d=anchor;
            put32(d,12,static_cast<std::uint32_t>(at));
            put32(d,16,vds_bytes);
            put32(d,20,static_cast<std::uint32_t>(main));
            put32(d,24,vds_bytes);
            put32(d,28,static_cast<std::uint32_t>(reserve));
            tag_update(d,0,std::max<std::size_t>(512,16u+u16(d,10)));
            plan.descriptors.emplace_back(at*block,std::move(d));
        }
        Bytes td(block,0);
        put16(td,0,8);put16(td,2,u16(lvid,2));
        put32(td,12,static_cast<std::uint32_t>(integrity_lba+1));
        tag_update(td,0,512);
        plan.descriptors.emplace_back((integrity_lba+1)*block,std::move(td));
        Integrity integrity;
        integrity.at=integrity_lba*block;
        integrity.impl_bytes=u32(lvid,76);
        auto next=std::max<std::uint64_t>(16,u64(lvid,40));
        auto consider=[&](const Fe& fe) {
            const auto id=u64(fe.bytes,u16(fe.bytes,0)==266?200u:160u);
            need(id<std::numeric_limits<std::uint64_t>::max(),
                 "UDF mastering unique-ID overflow");
            next=std::max(next,id+1);
        };
        for(const auto& [key,file]:files_) {(void)key;consider(file.fe);}
        for(const auto& [key,dir]:directory_files_) {(void)key;consider(dir);}
        need(next<std::numeric_limits<std::uint64_t>::max(),
             "UDF mastering unique-ID overflow");
        integrity.next_id=next;
        put64(lvid,40,next);
        put32(lvid,12,static_cast<std::uint32_t>(integrity_lba));
        put32(lvid,80,static_cast<std::uint32_t>(extra));
        put32(lvid,84,static_cast<std::uint32_t>(new_blocks));
        integrity.bytes=std::move(lvid);
        integrity_=std::move(integrity);
        Bitmap virtual_map;
        virtual_map.bits=static_cast<std::uint32_t>(new_blocks);
        virtual_map.byte_count=(virtual_map.bits+7u)/8u;
        virtual_map.bytes.assign(24u+virtual_map.byte_count,0);
        allocation_start_=static_cast<std::uint32_t>(old_sectors-partition_);
        for(auto j=allocation_start_;j<virtual_map.bits;++j)
            virtual_map.bytes[24u+j/8u]|=static_cast<std::uint8_t>(1u<<(j%8u));
        bitmap_=std::move(virtual_map);
        partition_blocks_=static_cast<std::uint32_t>(new_blocks);
        size_=(last+1)*block;
        return plan;
    }
    void close() {file_.close();}
private:
    std::ifstream file_;
    std::uint64_t size_{};
    bool bounded_partitioned_bridge_{};
    std::uint32_t partition_{},partition_blocks_{};
    std::uint32_t partition_access_type_{},allocation_start_{};
    std::map<std::string,File> files_;
    std::map<std::string,Fe> directory_files_;
    std::map<std::string,File> directory_links_;
    std::optional<Bitmap> bitmap_;
    std::optional<UnallocatedTable> table_;
    std::optional<Integrity> integrity_;
    std::string allocation_status_;
    // Sorted, merged half-open Type-1 partition sectors owned by ISO.
    std::vector<std::pair<std::uint32_t,std::uint32_t>> external_blocks_;
    std::map<std::uint64_t,std::size_t> references_;
    std::set<std::uint64_t> deleted_icbs_;
    std::set<std::uint32_t> directories_;
    std::vector<Interval> reserved_;

    Bytes read(std::uint64_t offset,std::size_t bytes) {
        need(offset<=size_ && bytes<=size_-offset && bytes<=max_dir,
             "UDF file reader out of image bounds");
        Bytes result(bytes);
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(offset));
        file_.read(reinterpret_cast<char*>(result.data()),
                   static_cast<std::streamsize>(bytes));
        need(bool(file_),"UDF image read failed");
        return result;
    }
    std::uint64_t physical(std::uint32_t logical) const {
        need(logical<partition_blocks_,"UDF extent outside partition");
        return (std::uint64_t(partition_)+logical)*block;
    }
    Fe parse_fe(std::uint32_t lba,std::uint16_t map) {
        need(map==0,"UDF nonphysical map index is read-only");
        const auto offset=physical(lba);
        Fe fe;fe.at=offset;fe.bytes=read(offset,block);
        const auto type=u16(fe.bytes,0);
        need(type==261||type==266,"unsupported UDF file entry descriptor");
        tag_check(fe.bytes,0,type);
        fe.type=fe.bytes[27];
        need(fe.type==4 || fe.type==5,
             "UDF special files/symlinks are not writable");
        fe.storage=u16(fe.bytes,34)&7u;
        need(fe.storage==0 || fe.storage==3,
             "UDF LongAD/extended allocation is read-only");
        fe.size=u64(fe.bytes,56);
        fe.base=type==261?176:216;
        const auto ea=u32(fe.bytes,type==261?168:208);
        fe.ads=u32(fe.bytes,type==261?172:212);
        need(ea<=block-fe.base && fe.ads<=block-fe.base-ea,
             "UDF file-entry AD outside descriptor");
        fe.ad_offset=fe.base+ea;
        if(fe.storage==3) {
            need(fe.size<=fe.ads,"UDF embedded file size exceeds AD length");
            fe.data_at=offset+fe.ad_offset;
            fe.capacity=block-fe.ad_offset;
            need(fe.size<=fe.capacity,"UDF embedded data outside descriptor");
        } else {
            need(fe.ads>=8 && fe.ads%8==0,
                 "UDF ShortAD array is truncated or empty");
            for(std::size_t p=fe.ad_offset;p<fe.ad_offset+fe.ads;p+=8) {
                const auto raw=u32(fe.bytes,p);
                const auto length=std::uint64_t(raw&0x3fffffffu);
                const auto lba_data=u32(fe.bytes,p+4);
                need((raw>>30)==0 && length>0,
                     "unrecorded/sparse/empty UDF ShortAD is read-only");
                const auto sectors=(length+block-1u)/block;
                need(lba_data<partition_blocks_ &&
                     sectors<=partition_blocks_-lba_data,
                     "UDF ShortAD data outside partition");
                const auto at=physical(lba_data);
                need(sectors*block<=size_-at,
                     "UDF ShortAD extent exceeds image");
                const Location candidate{at,sectors*block};
                for(const auto& prior:fe.runs)
                    need(!intersects(candidate,
                         {prior.at,std::uint64_t(prior.blocks)*block}),
                         "UDF file has overlapping ShortAD descriptors");
                need(fe.recorded<=std::numeric_limits<std::uint64_t>::max()-length &&
                     fe.capacity<=std::numeric_limits<std::uint64_t>::max()-sectors*block,
                     "UDF file extent length overflow");
                fe.runs.push_back({at,length,lba_data,
                                   static_cast<std::uint32_t>(sectors)});
                fe.recorded+=length;
                fe.capacity+=sectors*block;
            }
            need(fe.size<=fe.recorded,
                 "UDF file InformationLength exceeds ShortAD data");
            fe.data_at=fe.runs.front().at;
            need(fe.type!=4 || fe.runs.size()==1,
                 "multi-extent UDF directories are not yet writable");
        }
        return fe;
    }
    Bytes directory_data(const Fe& fe) {
        need(fe.type==4 && fe.size<=max_dir,
             "unsupported oversized or invalid UDF directory");
        return read(fe.data_at,static_cast<std::size_t>(fe.size));
    }
    void walk(std::uint32_t lba,std::uint16_t map,
              const std::string& prefix,unsigned depth) {
        need(depth<=max_depth && directories_.insert(lba).second &&
             directories_.size()<=max_items,
             "UDF directory depth, alias or cycle unsupported");
        const auto directory=parse_fe(lba,map);
        need(directory.type==4,"UDF FID references a non-directory");
        need(directory_files_.emplace(prefix,directory).second,
             "duplicate UDF directory path");
        reserved_.push_back({{directory.at,block},directory.at});
        for(const auto& run:directory.runs)
            reserved_.push_back({{run.at,std::uint64_t(run.blocks)*block},
                                  directory.at});
        const auto data=directory_data(directory);
        for(std::size_t p=0;p<data.size();) {
            const auto remaining=data.size()-p;
            if(remaining<38) {
                need(std::all_of(data.begin()+static_cast<std::ptrdiff_t>(p),
                                 data.end(),
                                 [](std::uint8_t n){return n==0;}),
                     "trailing incomplete UDF FID");
                break;
            }
            if(std::all_of(data.begin()+static_cast<std::ptrdiff_t>(p),
                           data.begin()+static_cast<std::ptrdiff_t>(p+16),
                           [](std::uint8_t n){return n==0;})) {
                const auto padding=std::min<std::size_t>(
                    block-p%block,remaining);
                need(std::all_of(data.begin()+static_cast<std::ptrdiff_t>(p),
                                 data.begin()+static_cast<std::ptrdiff_t>(p+padding),
                                 [](std::uint8_t n){return n==0;}),
                     "invalid UDF sector padding");
                p+=padding;
                continue;
            }
            tag_check(data,p,257);
            const auto n=std::size_t(data[p+19]);
            const auto impl=std::size_t(u16(data,p+36));
            need(impl<=data.size()-p-38 && n<=data.size()-p-38-impl,
                 "UDF directory FID name out of bounds");
            const auto len=(38+impl+n+3u)&~std::size_t(3);
            // A FID may cross a sector boundary; its tag location is the
            // logical sector containing the start of the descriptor.
            need(len>=40 && len<=remaining,
                 "UDF FID exceeds directory logical length");
            const auto characteristics=data[p+18];
            const auto child_lba=u32(data,p+24);
            const auto child_map=u16(data,p+28);
            // A tombstone in an older image may still point at an FE.
            // Never reclaim an FE which such a FID could still reference.
            if(n && (characteristics&0x04u) &&
               u32(data,p+20)!=0 && child_map==0 &&
               child_lba<partition_blocks_)
                deleted_icbs_.insert(physical(child_lba));
            if(n && !(characteristics&0x0cu)) {
                const auto filename=name(data,p+38+impl,n);
                const auto path=prefix.empty()?filename:prefix+"/"+filename;
                need(path.size()<=2048 && files_.size()<max_items,
                     "too many UDF file entries");
                auto child=parse_fe(child_lba,child_map);
                ++references_[child.at];
                need(references_[child.at]==1,
                     "hard-linked/multiple referenced UDF entries read-only");
                if(child.type==4) {
                    // Recursive walk owns the FE and all ShortAD sectors.
                    // A duplicate parent reservation would reject safe reclaim.
                    File link;
                    link.fe=std::move(child);
                    link.fid_at=directory.data_at+p;
                    link.fid_bytes=len;
                    link.parent_at=directory.at;
                    link.parent_embedded=directory.storage==3;
                    need(directory_links_.emplace(path,std::move(link)).second,
                         "duplicate UDF directory parent link");
                    walk(child_lba,child_map,path,depth+1);
                } else {
                    reserved_.push_back({{child.at,block},child.at});
                    for(const auto& run:child.runs)
                        reserved_.push_back({{run.at,std::uint64_t(run.blocks)*block},
                                              child.at});
                    File file;
                    file.fe=std::move(child);
                    file.fid_at=directory.data_at+p;
                    file.fid_bytes=len;
                    file.parent_at=directory.at;
                    file.parent_embedded=directory.storage==3;
                    need(files_.emplace(path,std::move(file)).second,
                         "duplicate UDF file path");
                }
            }
            p+=len;
        }
    }
};

// Zero only in the transaction's staged image. Large ShortAD extents are
// scrubbed with one bounded buffer; any I/O failure aborts the transaction.
void staged_zero(std::fstream& file,std::uint64_t at,std::uint64_t count,
                 const EditProgress& progress) {
    constexpr std::size_t chunk_size=1024u*1024u;
    const Bytes zeros(chunk_size,0);
    while(count) {
        edit_tick(progress);
        const auto portion=static_cast<std::size_t>(
            std::min<std::uint64_t>(count,zeros.size()));
        file.clear();
        file.seekp(static_cast<std::streamoff>(at));
        need(bool(file),"cannot seek UDF reclaimed sector");
        file.write(reinterpret_cast<const char*>(zeros.data()),
                   static_cast<std::streamsize>(portion));
        need(bool(file),"cannot wipe UDF reclaimed sector");
        at+=portion;
        count-=portion;
    }
}
void staged_write(std::fstream& file,std::uint64_t at,const Bytes& b) {
    file.clear();
    file.seekp(static_cast<std::streamoff>(at));
    need(bool(file),"cannot seek staged UDF descriptor");
    file.write(reinterpret_cast<const char*>(b.data()),
               static_cast<std::streamsize>(b.size()));
    need(bool(file),"cannot write staged UDF descriptor");
}
Bytes staged_read(std::fstream& file,std::uint64_t at,std::size_t n) {
    Bytes out(n);
    file.flush();
    need(bool(file),"cannot flush staged UDF descriptor");
    file.clear();
    file.seekg(static_cast<std::streamoff>(at));
    file.read(reinterpret_cast<char*>(out.data()),
              static_cast<std::streamsize>(n));
    need(bool(file),"cannot read staged UDF descriptor");
    return out;
}
void copy(std::ifstream& in,std::fstream& out,std::uint64_t n,
          const EditProgress& progress) {
    Bytes buffer(1024*1024);
    while(n) {
        edit_tick(progress);
        const auto count=static_cast<std::size_t>(
            std::min<std::uint64_t>(n,buffer.size()));
        in.read(reinterpret_cast<char*>(buffer.data()),
                static_cast<std::streamsize>(count));
        need(bool(in),"UDF replacement source truncated");
        out.write(reinterpret_cast<const char*>(buffer.data()),
                  static_cast<std::streamsize>(count));
        need(bool(out),"staged UDF data write failed");
        n-=count;
    }
}
// Verify through the independently parsed Disc extent map. This avoids
// extracting a second multi-gigabyte copy just to compare staged contents.
// Clone an image into the newly truncated transaction file without eagerly
// writing every all-zero sector. This materially reduces disk consumption for
// large sparse UDF test images; holes read back as zeros. Never use this for
// replacement data in previously allocated blocks (which may contain stale
// content). The explicit final byte preserves exact logical image length.
void copy_image_sparse(std::ifstream& in,std::fstream& out,std::uint64_t n,
                       const EditProgress& progress) {
    need(n>0 && n<=static_cast<std::uint64_t>(
        std::numeric_limits<std::streamoff>::max()),
         "unsupported sparse UDF staging image size");
    Bytes buffer(1024*1024);
    const auto full_size=n;
    std::uint64_t at=0;
    std::uint8_t last=0;
    while(n) {
        edit_tick(progress);
        const auto count=static_cast<std::size_t>(
            std::min<std::uint64_t>(n,buffer.size()));
        in.read(reinterpret_cast<char*>(buffer.data()),
                static_cast<std::streamsize>(count));
        need(bool(in),"UDF source image truncated during staging");
        last=buffer[count-1u];
        if(!std::all_of(buffer.begin(),
                        buffer.begin()+static_cast<std::ptrdiff_t>(count),
                        [](std::uint8_t value){return value==0;})) {
            out.clear();
            out.seekp(static_cast<std::streamoff>(at));
            need(bool(out),"cannot seek staged sparse UDF image");
            out.write(reinterpret_cast<const char*>(buffer.data()),
                      static_cast<std::streamsize>(count));
            need(bool(out),"cannot write staged sparse UDF image");
        }
        at+=count;
        n-=count;
    }
    out.clear();
    out.seekp(static_cast<std::streamoff>(full_size-1u));
    need(bool(out),"cannot complete sparse UDF image length");
    out.put(static_cast<char>(last));
    need(bool(out),"cannot finalize sparse UDF staging image");
}
// Best-effort filesystem copy-on-write stage. Never use hardlinks:
// modifications to the staged destination must not touch the original.
// Failed/unsupported clones are removed before the sparse-copy fallback.
bool try_clone_udf_stage(const std::filesystem::path& original,
                         const std::filesystem::path& staged,
                         std::uint64_t size) {
    #ifdef _WIN32
    char* force_sparse=nullptr;
    std::size_t force_sparse_length=0;
    const errno_t env_status=_dupenv_s(&force_sparse,&force_sparse_length,"ISO_WCX_UDF_FORCE_SPARSE_STAGE");
    const bool forced=(env_status==0 && force_sparse!=nullptr);
    std::free(force_sparse);
    if(forced)return false;
#else
    if(std::getenv("ISO_WCX_UDF_FORCE_SPARSE_STAGE")!=nullptr)return false;
#endif
#ifdef __linux__
    const int source=::open(original.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(source<0)return false;
    const int dest=::open(staged.c_str(),
                          O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(dest<0) {
        ::close(source);
        return false;
    }
    struct stat st{};
    const bool source_ok=::fstat(source,&st)==0 && S_ISREG(st.st_mode) &&
                         st.st_size>=0 &&
                         static_cast<std::uint64_t>(st.st_size)==size;
    const bool cloned=source_ok && ::ioctl(dest,FICLONE,source)==0;
    const bool closed_dest=::close(dest)==0;
    ::close(source);
    if(cloned && closed_dest &&
       std::filesystem::file_size(staged)==size)return true;
    std::error_code ec;
    std::filesystem::remove(staged,ec);
    need(!ec,"cannot discard incomplete UDF reflink stage");
    return false;
#elif defined(_WIN32)
    // ReFS and supported NTFS block cloning require aligned offsets and
    // lengths. 2-KiB-only tail images fall back to ordinary sparse copy.
    constexpr std::uint64_t alignment=64u*1024u;
    constexpr std::uint64_t chunk=64u*1024u*1024u;
    if(size==0 || size%alignment!=0 ||
       size>static_cast<std::uint64_t>(
           std::numeric_limits<LONGLONG>::max()))return false;
    const HANDLE source=CreateFileW(
        original.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(source==INVALID_HANDLE_VALUE)return false;
    const HANDLE dest=CreateFileW(
        staged.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,
        CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(dest==INVALID_HANDLE_VALUE) {
        CloseHandle(source);
        return false;
    }
    LARGE_INTEGER end{};
    end.QuadPart=static_cast<LONGLONG>(size);
    bool cloned=SetFilePointerEx(dest,end,nullptr,FILE_BEGIN)!=0 &&
                SetEndOfFile(dest)!=0;
    for(std::uint64_t offset=0;cloned && offset<size;) {
        const auto length=std::min<std::uint64_t>(size-offset,chunk);
        DUPLICATE_EXTENTS_DATA data{};
        data.FileHandle=source;
        data.SourceFileOffset.QuadPart=static_cast<LONGLONG>(offset);
        data.TargetFileOffset.QuadPart=static_cast<LONGLONG>(offset);
        data.ByteCount.QuadPart=static_cast<LONGLONG>(length);
        DWORD returned=0;
        cloned=DeviceIoControl(dest,FSCTL_DUPLICATE_EXTENTS_TO_FILE,
                               &data,sizeof(data),nullptr,0,
                               &returned,nullptr)!=0;
        offset+=length;
    }
    const bool closed_dest=CloseHandle(dest)!=0;
    CloseHandle(source);
    if(cloned && closed_dest &&
       std::filesystem::file_size(staged)==size)return true;
    std::error_code ec;
    std::filesystem::remove(staged,ec);
    need(!ec,"cannot discard incomplete UDF block-clone stage");
    return false;
#else
    (void)original;
    (void)staged;
    (void)size;
    return false;
#endif
}

// Prepare a distinct sibling stage. The existing EditTransaction still
// performs the final atomic rename, including timestamp/size validation.
void prepare_udf_stage(const std::filesystem::path& original,
                       const std::filesystem::path& staged,
                       std::uint64_t size,const EditProgress& progress) {
    edit_tick(progress);
    need(size==std::filesystem::file_size(original),
         "UDF original changed size before staging");
    if(try_clone_udf_stage(original,staged,size))return;
    std::ifstream in(original,std::ios::binary);
    need(bool(in),"cannot open UDF source for sparse staging");
    std::fstream out(staged,std::ios::binary|std::ios::in|
                     std::ios::out|std::ios::trunc);
    need(bool(out),"cannot create UDF sparse stage");
    copy_image_sparse(in,out,size,progress);
    out.flush();
    need(bool(out),"cannot flush UDF sparse stage");
}

bool same_staged_item(const std::filesystem::path& staged,const Item& item,
                      const std::filesystem::path& source,
                      const EditProgress& progress) {
    if(item.directory || !item.direct_source.empty() ||
       item.size!=std::filesystem::file_size(source))return false;
    std::ifstream a(source,std::ios::binary),b(staged,std::ios::binary);
    need(bool(a) && bool(b),"cannot open UDF staged verification sources");
    const auto image_size=std::filesystem::file_size(staged);
    Bytes aa(1024*1024),bb(1024*1024);
    std::uint64_t verified=0;
    if(!item.inline_data.empty()) {
        need(item.inline_data.size()==item.size,
             "invalid UDF inline verification size");
        while(verified<item.size) {
            edit_tick(progress);
            const auto n=static_cast<std::size_t>(
                std::min<std::uint64_t>(item.size-verified,aa.size()));
            a.read(reinterpret_cast<char*>(aa.data()),
                   static_cast<std::streamsize>(n));
            need(bool(a),"UDF verification source truncated");
            if(!std::equal(aa.begin(),aa.begin()+
                           static_cast<std::ptrdiff_t>(n),
                           item.inline_data.begin()+
                           static_cast<std::ptrdiff_t>(verified)))return false;
            verified+=n;
        }
    } else {
        for(const auto& extent:item.extents) {
            if(extent.sparse || extent.length>item.size-verified ||
               extent.offset>image_size ||
               extent.length>image_size-extent.offset)return false;
            b.clear();
            b.seekg(static_cast<std::streamoff>(extent.offset));
            need(bool(b),"cannot seek staged UDF readback run");
            std::uint64_t remaining=extent.length;
            while(remaining) {
                edit_tick(progress);
                const auto n=static_cast<std::size_t>(
                    std::min<std::uint64_t>(remaining,aa.size()));
                a.read(reinterpret_cast<char*>(aa.data()),
                       static_cast<std::streamsize>(n));
                b.read(reinterpret_cast<char*>(bb.data()),
                       static_cast<std::streamsize>(n));
                need(bool(a) && bool(b),"UDF staged readback was truncated");
                if(!std::equal(aa.begin(),aa.begin()+
                               static_cast<std::ptrdiff_t>(n),bb.begin()))
                    return false;
                verified+=n;
                remaining-=n;
            }
        }
    }
    return verified==item.size;
}
// Encode a path component as OSTA Compressed Unicode (CS0), never by
// assuming arbitrary UTF-8 bytes are a UDF 8-bit filename. For UDF 1.02
// keep newly created names in the portable BMP subset (UCS-2); rejecting
// supplementary characters is safer than emitting unsupported surrogates.
Bytes encode_udf_cs0(std::string_view component) {
    std::vector<std::uint16_t> scalars;
    scalars.reserve(component.size());
    bool needs_16bit=false;
    for(std::size_t i=0;i<component.size();) {
        const auto lead=static_cast<unsigned char>(component[i++]);
        std::uint32_t cp=0;
        unsigned trailing=0;
        if(lead<0x80)cp=lead;
        else if(lead>=0xc2 && lead<=0xdf) {
            cp=lead&0x1fu;trailing=1;
        } else if(lead>=0xe0 && lead<=0xef) {
            cp=lead&0x0fu;trailing=2;
        } else {
            throw EditUnsupported(
                "UDF Type-1 writer accepts BMP Unicode, not non-BMP names");
        }
        need(trailing<=component.size()-i,
             "truncated UTF-8 UDF filename");
        for(unsigned j=0;j<trailing;++j) {
            const auto b=static_cast<unsigned char>(component[i++]);
            need((b&0xc0u)==0x80u,
                 "invalid UTF-8 UDF continuation");
            cp=(cp<<6)|(b&0x3fu);
        }
        need((trailing!=1 || cp>=128) &&
             (trailing!=2 || cp>=2048) &&
             cp>=32 && cp!=127 && cp!=0xfeff && cp!=0xfffe &&
             !(cp>=0xd800 && cp<=0xdfff),
             "invalid UDF CS0 Unicode scalar");
        needs_16bit=needs_16bit || cp>255;
        scalars.push_back(static_cast<std::uint16_t>(cp));
    }
    need(!scalars.empty(),"empty UDF filename");
    const auto encoded_bytes=1u+scalars.size()*(needs_16bit?2u:1u);
    need(encoded_bytes<=255,"UDF CS0 filename exceeds 255 bytes");
    // The exact encoded length is already checked. Fixed-size writes
    // avoid reallocations and eliminate GCC 14's false positive for the
    // C++26 vector::_M_realloc_append inlining path under -Werror.
    Bytes result(encoded_bytes,0);
    result[0]=needs_16bit?16u:8u;
    std::size_t pos=1;
    for(const auto cp:scalars) {
        if(needs_16bit)result[pos++]=static_cast<std::uint8_t>(cp>>8);
        result[pos++]=static_cast<std::uint8_t>(cp);
    }
    need(pos==result.size(),"UDF CS0 length encoding mismatch");
    return result;
}
// Retag every existing FID before relocating an embedded directory to a
// standalone ShortAD sector. A FID's TagLocation refers to the sector holding
// that FID, not the directory's (now different) File Entry block.
void retag_directory_fids(Bytes& bytes,std::uint32_t logical) {
    for(std::size_t p=0;p<bytes.size();) {
        const auto remaining=bytes.size()-p;
        if(remaining<38) {
            need(std::all_of(bytes.begin()+static_cast<std::ptrdiff_t>(p),
                             bytes.end(),[](std::uint8_t v){return v==0;}),
                 "trailing incomplete UDF FID during relocation");
            break;
        }
        if(std::all_of(bytes.begin()+static_cast<std::ptrdiff_t>(p),
                       bytes.begin()+static_cast<std::ptrdiff_t>(p+16),
                       [](std::uint8_t v){return v==0;})) {
            const auto padding=std::min<std::size_t>(
                block-p%block,remaining);
            need(std::all_of(bytes.begin()+static_cast<std::ptrdiff_t>(p),
                             bytes.begin()+static_cast<std::ptrdiff_t>(p+padding),
                             [](std::uint8_t v){return v==0;}),
                 "invalid UDF directory relocation padding");
            p+=padding;
            continue;
        }
        tag_check(bytes,p,257);
        const auto name_bytes=std::size_t(bytes[p+19]);
        const auto implementation=std::size_t(u16(bytes,p+36));
        need(implementation<=remaining-38 &&
             name_bytes<=remaining-38-implementation,
             "UDF relocated FID identifier exceeds directory length");
        const auto record=(38u+implementation+name_bytes+3u)&~std::size_t(3);
        need(record>=40 && record<=remaining,
             "UDF relocation would truncate an existing FID");
        const auto crc_bytes=16u+u16(bytes,p+10);
        need(crc_bytes<=record,"UDF FID CRC would exceed record");
        put32(bytes,p+12,logical+static_cast<std::uint32_t>(p/block));
        tag_update(bytes,p,crc_bytes);
        p+=record;
    }
}
// Deleted FIDs remain part of a directory's logical byte stream. A new
// entry may overwrite a tombstone only when its ICB long_ad has been fully
// cleared and its original byte span can be preserved exactly. Never split,
// shift or compact other records: 7-Zip requires packed aligned FIDs.
struct FreeFidSlot {
    std::size_t offset=0,span=0;
};
std::vector<FreeFidSlot> collect_free_fid_slots(
        const Bytes& payload,std::uint32_t first_logical) {
    std::vector<FreeFidSlot> slots;
    for(std::size_t p=0;p<payload.size();) {
        const auto remaining=payload.size()-p;
        if(remaining<38) {
            need(std::all_of(payload.begin()+static_cast<std::ptrdiff_t>(p),
                             payload.end(),[](std::uint8_t value) {
                                 return value==0;
                             }),"UDF reusable FID trailing bytes are invalid");
            break;
        }
        if(std::all_of(payload.begin()+static_cast<std::ptrdiff_t>(p),
                       payload.begin()+static_cast<std::ptrdiff_t>(p+16),
                       [](std::uint8_t value){return value==0;})) {
            const auto pad=std::min<std::size_t>(block-p%block,remaining);
            need(std::all_of(payload.begin()+static_cast<std::ptrdiff_t>(p),
                             payload.begin()+static_cast<std::ptrdiff_t>(p+pad),
                             [](std::uint8_t value){return value==0;}),
                 "UDF free FID scanner encountered nonzero sector padding");
            p+=pad;
            continue;
        }
        tag_check(payload,p,257);
        const auto implementation=std::size_t(u16(payload,p+36));
        const auto name_bytes=std::size_t(payload[p+19]);
        need(implementation<=remaining-38 &&
             name_bytes<=remaining-38-implementation,
             "UDF old FID name or ImplementationUse overflows directory");
        const auto span=(38u+implementation+name_bytes+3u)&~std::size_t(3);
        need(span>=40 && span<=remaining && p%4u==0,
             "UDF tombstone has invalid record size/alignment");
        const bool is_file_tombstone=
            (payload[p+18]&0x0eu)==0x04u;
        const bool cleared_icb=
            std::all_of(payload.begin()+static_cast<std::ptrdiff_t>(p+20),
                        payload.begin()+static_cast<std::ptrdiff_t>(p+36),
                        [](std::uint8_t value){return value==0;});
        if(is_file_tombstone && cleared_icb && span<=4096u &&
           u16(payload,p+10)+16u<=span &&
           u32(payload,p+12)==first_logical+static_cast<std::uint32_t>(p/block))
            slots.push_back({p,span});
        p+=span;
    }
    return slots;
}

struct ReusedFid {
    std::size_t offset=0,span=0,implementation=0;
};
std::optional<ReusedFid> choose_reused_fid(
        const std::vector<FreeFidSlot>& slots,
        std::size_t name_bytes,const std::set<std::size_t>& used) {
    const auto minimum=(38u+name_bytes+3u)&~std::size_t(3);
    std::optional<ReusedFid> padded;
    for(const auto& slot:slots) {
        if(used.count(slot.offset) || slot.span<minimum)continue;
        if(slot.span==minimum)return ReusedFid{slot.offset,slot.span,0};
        // UDF's FID ImplementationUse must contain a 32-byte EntityID
        // whenever nonempty. Do not invent 4..31-byte implementation areas.
        const auto impl=slot.span-(38u+name_bytes);
        if(impl>=32u && impl<=65535u &&
           (!padded || slot.span<padded->span))
            padded=ReusedFid{slot.offset,slot.span,impl};
    }
    return padded;
}

void create_udf_type1_batch(const std::filesystem::path& image,Type1& volume,
                            const std::vector<ArchiveChange>& changes,
                            const EditProgress& progress,bool allow_bridge,
                            const EditStageValidator& validate_stage,
                            const ReadonlyAppendPlan* mastering=nullptr,
                            const CoordinatedIsoUpdate* coordinated=nullptr);

// Allocate one FE and optionally a contiguous ShortAD run for file data.
// Embedded or external parent directories may grow to a larger contiguous
// ShortAD run. FIDs are tightly packed (and may straddle sector boundaries)
// for compatibility with independent readers including 7-Zip.
void create_udf_type1(const std::filesystem::path& image,Type1& volume,
                      const ArchiveChange& change,
                      const EditProgress& progress,bool allow_bridge,
                      const EditStageValidator& validate_stage) {
    const auto key=edit_path(change.path);
    if(!volume.has_allocation_map())
        throw EditUnsupported(volume.missing_allocation_map_reason());
    need(!volume.files().count(key) &&
         !volume.directories().count(key),"UDF destination already exists");

    const auto separator=key.rfind('/');
    const std::string parent_name=separator==std::string::npos?
                                  std::string{}:key.substr(0,separator);
    const auto component=separator==std::string::npos?
                         key:key.substr(separator+1);
    const Bytes encoded_name=encode_udf_cs0(component);
    need(component.find('\\')==std::string::npos &&
         component.find('/')==std::string::npos,
         "invalid UDF path separator");
    const auto parent_it=volume.directories().find(parent_name);
    need(parent_it!=volume.directories().end(),
         "UDF parent directory does not exist");
    const auto& parent=parent_it->second;
    need(parent.type==4 && (parent.storage==3 || parent.storage==0) &&
         parent.size<=max_dir && parent.size<=parent.capacity &&
         parent.ad_offset<=block && parent.ads<=block-parent.ad_offset &&
         (parent.storage!=3 || parent.size<=parent.ads),
         "UDF parent directory has unsupported allocation layout");

    // Do not shadow an existing name with different ASCII case.
    std::string folded=key;
    for(char& c:folded)
        if(c>='a' && c<='z')c=static_cast<char>(c-'a'+'A');
    for(const auto& [other,file]:volume.files()) {
        (void)file;
        std::string upper=other;
        for(char& c:upper)
            if(c>='a' && c<='z')c=static_cast<char>(c-'a'+'A');
        need(upper!=folded,"UDF new name collides case-insensitively");
    }
    for(const auto& [other,dir]:volume.directories()) {
        (void)dir;
        std::string upper=other;
        for(char& c:upper)
            if(c>='a' && c<='z')c=static_cast<char>(c-'a'+'A');
        need(upper!=folded,"UDF new directory collides case-insensitively");
    }

    constexpr std::size_t fe_header=176;
    // InformationLength is uint64; every ShortAD has a 30-bit length.
    // The image/partition bounds and limited FE descriptor area are enforced.
    std::uint64_t length=0;
    if(!change.directory) {
        need(std::filesystem::is_regular_file(
                 std::filesystem::symlink_status(change.source)),
             "UDF create source must be a regular file, not symlink");
        length=std::filesystem::file_size(change.source);
        need(length<=volume.image_size(),
             "new UDF file exceeds image physical size");
    }
    const bool embedded=length<=block-fe_header;
    const auto data_blocks=embedded?0ull:(length+block-1u)/block;
    need(data_blocks<=std::numeric_limits<std::uint32_t>::max(),
         "new UDF file exceeds partition allocation counters");
    const auto fid_id_size=encoded_name.size();
    const auto fid_size=(38u+fid_id_size+3u)&~std::size_t(3);
    need(fid_size<=255 && fid_size<=block,
         "new UDF FID exceeds maximum supported descriptor size");
    // UDF FIDs are four-byte aligned and tightly packed. Inserting sector
    // padding here makes valid-looking images unreadable by 7-Zip.
    const auto fid_start=parent.size;
    const auto new_dir_size=fid_start+fid_size;
    need(new_dir_size<=max_dir,
         "new UDF FID exceeds bounded directory size");
    const bool relocate_parent=new_dir_size>parent.capacity;
    const bool external_parent=relocate_parent || parent.storage==0;
    const auto new_parent_blocks=relocate_parent?
        static_cast<std::uint32_t>((new_dir_size+block-1u)/block):0u;
    const auto old_parent_blocks=relocate_parent && parent.storage==0?
        static_cast<std::uint32_t>(parent.capacity/block):0u;
    const auto old_directory=volume.directory_payload(parent);
    need(old_directory.size()==parent.size,
         "UDF parent directory data size mismatch");
    // A normal single-file addition uses the shared batch planner only if
    // an existing, safely cleared tombstone can hold this FID. This keeps
    // legacy single-add behavior unchanged when no reusable slot exists.
    if(choose_reused_fid(collect_free_fid_slots(
                         old_directory,parent.storage==3?
                             volume.logical_of(parent.at):
                             volume.logical_of(parent.data_at)),
                         encoded_name.size(),std::set<std::size_t>{})) {
        create_udf_type1_batch(image,volume,{change},progress,
                               allow_bridge,validate_stage);
        return;
    }

    const auto new_lba=volume.allocate_block();
    const auto data_runs=data_blocks?
        volume.allocate_file_data(length,(block-fe_header)/8u):
        std::vector<DataRun>{};
    const auto parent_data_lba=relocate_parent?
        volume.allocate_blocks(new_parent_blocks):
        (parent.storage==0?volume.logical_of(parent.data_at):0u);
    // Only a uniquely owned external directory is released. A failed
    // allocation or alias check does not change the source image.
    if(old_parent_blocks)
        volume.release_directory_blocks(
            parent.at,parent.data_at,old_parent_blocks);
    const auto new_offset=volume.absolute(new_lba);
    Bytes child(block,0);
    put16(child,0,261); // ECMA-167 File Entry
    put16(child,2,2);   // DescriptorVersion
    put32(child,12,new_lba);
    child[27]=change.directory?4u:5u; // Directory or ordinary file
    put16(child,34,embedded?3u:0u); // ICB embedded or ShortAD
    put32(child,36,0xffffffffu); // UID not supplied
    put32(child,40,0xffffffffu); // GID not supplied
    put32(child,44,change.directory?0x000001ffu:0x000001b6u);
    put16(child,48,1); // Link count
    put64(child,56,length);
    put64(child,64,data_blocks); // LogicalBlocksRecorded
    put32(child,108,1); // Checkpoint
    put64(child,160,volume.new_unique_id());
    put32(child,168,0); // ExtendedAttributes length
    const auto child_ad_bytes=static_cast<std::uint32_t>(8u*data_runs.size());
    put32(child,172,embedded?static_cast<std::uint32_t>(length):child_ad_bytes);
    if(!embedded) {
        for(std::size_t i=0;i<data_runs.size();++i) {
            const auto& run=data_runs[i];
            need(run.recorded<=0x3fffffffull,
                 "UDF ShortAD 30-bit length exceeded");
            put32(child,176u+i*8u,static_cast<std::uint32_t>(run.recorded));
            put32(child,180u+i*8u,run.logical);
        }
    } else if(length) {
        std::ifstream input(change.source,std::ios::binary);
        need(bool(input),"cannot read new UDF embedded source");
        input.read(reinterpret_cast<char*>(child.data()+fe_header),
                   static_cast<std::streamsize>(length));
        need(bool(input),"new UDF embedded source shortened");
    }
    tag_update(child,0,embedded?fe_header+static_cast<std::size_t>(length):
               fe_header+child_ad_bytes);

    Bytes fid(fid_size,0);
    put16(fid,0,257); // File Identifier Descriptor
    put16(fid,2,2);
    put32(fid,12,external_parent?
                           parent_data_lba+
                           static_cast<std::uint32_t>(fid_start/block):
                           volume.logical_of(parent.at));
    put16(fid,16,1); // FileVersionNumber
    fid[18]=change.directory?2u:0u;
    fid[19]=static_cast<std::uint8_t>(fid_id_size);
    put32(fid,20,static_cast<std::uint32_t>(block));
    put32(fid,24,new_lba);
    put16(fid,28,0); // Physical partition map index
    std::copy(encoded_name.begin(),encoded_name.end(),fid.begin()+38);
    // UDF FID CRC covers the complete four-byte-aligned descriptor,
    // including its required padding. Independent 7-Zip validation
    // rejects a shortened CRC span on these Type-1 images.
    tag_update(fid,0,fid.size());

    auto parent_bytes=parent.bytes;
    Bytes directory_blocks;
    const bool extended=u16(parent_bytes,0)==266;
    const auto ad_length_field=extended?212u:172u;
    put64(parent_bytes,56,new_dir_size);
    if(extended)
        put64(parent_bytes,64,new_dir_size); // EFE Object Size
    if(!external_parent) {
        const auto fid_at=parent.ad_offset+
                          static_cast<std::size_t>(fid_start);
        need(fid_at<=parent_bytes.size() &&
             fid.size()<=parent_bytes.size()-fid_at,
             "embedded UDF FID would exceed parent FE");
        std::copy(fid.begin(),fid.end(),
                  parent_bytes.begin()+static_cast<std::ptrdiff_t>(fid_at));
        put32(parent_bytes,ad_length_field,
              static_cast<std::uint32_t>(new_dir_size));
        tag_update(parent_bytes,0,parent.ad_offset+
                   static_cast<std::size_t>(new_dir_size));
    } else {
        directory_blocks=old_directory;
        if(relocate_parent)
            retag_directory_fids(directory_blocks,parent_data_lba);
        // Retain logical sector padding; explicitly zero new space.
        const auto capacity=relocate_parent?
            std::uint64_t(new_parent_blocks)*block:parent.capacity;
        need(capacity>=new_dir_size && capacity<=max_dir,
             "UDF relocated directory extent size invalid");
        directory_blocks.resize(static_cast<std::size_t>(capacity),0);
        std::copy(fid.begin(),fid.end(),
                  directory_blocks.begin()+
                  static_cast<std::ptrdiff_t>(fid_start));
        if(relocate_parent) {
            need(parent.ad_offset+8<=block,
                 "UDF parent cannot store a ShortAD");
            std::fill(parent_bytes.begin()+
                      static_cast<std::ptrdiff_t>(parent.ad_offset),
                      parent_bytes.end(),std::uint8_t{0});
            put16(parent_bytes,34,
                  static_cast<std::uint16_t>(u16(parent_bytes,34)&~7u));
            put32(parent_bytes,ad_length_field,8u);
            put32(parent_bytes,parent.ad_offset,
                  static_cast<std::uint32_t>(new_dir_size));
            put32(parent_bytes,parent.ad_offset+4,parent_data_lba);
            // FE LogicalBlocksRecorded / EFE LogicalBlocksRecorded.
            put64(parent_bytes,extended?72u:64u,new_parent_blocks);
        } else {
            put32(parent_bytes,parent.ad_offset,
                  static_cast<std::uint32_t>(new_dir_size));
        }
        tag_update(parent_bytes,0,parent.ad_offset+8u);
    }

    EditTransaction tx(image);
    prepare_udf_stage(image,tx.output,volume.image_size(),progress);
    std::fstream out(tx.output,std::ios::binary|std::ios::in|
                     std::ios::out);
    need(bool(out),"cannot open prepared UDF transaction stage");
    volume.close();

    staged_write(out,new_offset,child);
    if(data_blocks) {
        std::ifstream source(change.source,std::ios::binary);
        need(bool(source),"cannot open new UDF file content");
        std::uint64_t copied=0;
        for(const auto& run:data_runs) {
            out.clear();
            out.seekp(static_cast<std::streamoff>(run.at));
            need(bool(out),"cannot seek new UDF ShortAD run");
            copy(source,out,run.recorded,progress);
            copied+=run.recorded;
            const auto tail=std::uint64_t(run.blocks)*block-run.recorded;
            if(tail) staged_write(out,run.at+run.recorded,
                                  Bytes(static_cast<std::size_t>(tail),0));
        }
        need(copied==length,"new UDF multi-extent write was incomplete");
    }
    if(old_parent_blocks)
        staged_write(out,parent.data_at,
                     Bytes(std::size_t(old_parent_blocks)*block,0));
    if(external_parent)
        staged_write(out,volume.absolute(parent_data_lba),directory_blocks);
    staged_write(out,parent.at,parent_bytes);
    const auto [bitmap_at,bitmap_data]=volume.updated_bitmap();
    staged_write(out,bitmap_at,bitmap_data);
    const auto [integrity_at,integrity_data]=
        volume.updated_integrity(change.directory,
                                 1u+static_cast<std::uint32_t>(data_blocks)+
                                 new_parent_blocks-old_parent_blocks);
    staged_write(out,integrity_at,integrity_data);
    out.flush();
    need(bool(out),"staged UDF allocation flush failed");
    out.close();

    // Full strict structural parse plus independent extraction of changed
    // file bytes. Newly allocated FEs must also be discoverable by name.
    {
        Type1 strict(tx.output,allow_bridge,volume.bounded_partitioned_bridge());
        need( (strict.files().count(key)!=0)==(!change.directory),
             "staged UDF file map was not updated");
        need( (strict.directories().count(key)!=0)==change.directory,
             "staged UDF directory map was not updated");
        strict.close();
        const Disc inspected(tx.output);
        need(inspected.format()=="UDF","staged UDF allocation unreadable");
        auto it=std::find_if(inspected.items().begin(),inspected.items().end(),
            [&](const Item& item) {
                return item.path==key && item.directory==change.directory;
            });
        need(it!=inspected.items().end(),"new UDF entry not visible");
        if(!change.directory) {
            need(it->size==length,"new UDF file length mismatched");
            need(same_staged_item(tx.output,*it,change.source,progress),
                 "new UDF file content verification failed");
        }
    }
    if(validate_stage)validate_stage(tx.output);
    tx.commit(progress);
}

// A single bounded replacement engine serves standalone edits and mixed
// transactions. Callers preflight FE/ShortAD capacity and ownership before
// allocating or writing the transaction's temporary image.
void write_udf_existing_replacement(std::fstream& out,const File& target,
                                    const std::filesystem::path& source_path,
                                    const EditProgress& progress) {
        const auto len=std::filesystem::file_size(source_path);
        auto fe=staged_read(out,target.fe.at,block);
        tag_check(fe,0,u16(fe,0));
        if(target.fe.storage==3) {
            Bytes data(static_cast<std::size_t>(len));
            std::ifstream input(source_path,std::ios::binary);
            need(bool(input),"cannot open UDF replacement");
            input.read(reinterpret_cast<char*>(data.data()),
                       static_cast<std::streamsize>(data.size()));
            need(bool(input) || data.empty(),"UDF source shortened");
            const auto at=target.fe.ad_offset;
            std::fill(fe.begin()+static_cast<std::ptrdiff_t>(at),
                      fe.begin()+static_cast<std::ptrdiff_t>(
                          at+std::max<std::uint64_t>(len,target.fe.size)),0);
            std::copy(data.begin(),data.end(),
                      fe.begin()+static_cast<std::ptrdiff_t>(at));
            // For Extended File Entry this is at 212; regular File
            // Entries keep the allocation-descriptor length at 172.
            put32(fe,u16(fe,0)==266?212u:172u,
                  static_cast<std::uint32_t>(len));
            put64(fe,56,len);
            if(u16(fe,0)==266)put64(fe,64,len); // EFE Object Size
            tag_update(fe,0,target.fe.ad_offset+static_cast<std::size_t>(len));
            staged_write(out,target.fe.at,fe);
        } else {
            std::ifstream source(source_path,std::ios::binary);
            need(bool(source),"cannot open UDF replacement content");
            std::uint64_t remaining=len;
            for(std::size_t i=0;i<target.fe.runs.size();++i) {
                if(!remaining)break;
                const auto& run=target.fe.runs[i];
                const bool last=i+1==target.fe.runs.size();
                // The final run is allowed to consume its sector tail.
                const auto run_limit=last?
                    std::uint64_t(run.blocks)*block:run.recorded;
                const auto count=std::min(remaining,run_limit);
                out.clear();
                out.seekp(static_cast<std::streamoff>(run.at));
                need(bool(out),"cannot seek UDF content ShortAD run");
                copy(source,out,count,progress);
                remaining-=count;
            }
            need(remaining==0,"UDF replacement ShortAD sequence incomplete");
            put64(fe,56,len);
            if(u16(fe,0)==266)put64(fe,64,len); // EFE Object Size
            if(len>target.fe.recorded) {
                // Preserve every preceding ShortAD and extend only the
                // last recorded length, not its logical LBA or allocation.
                const auto& last=target.fe.runs.back();
                const auto grown=last.recorded+(len-target.fe.recorded);
                need(grown<=0x3fffffffull &&
                     grown<=std::uint64_t(last.blocks)*block,
                     "UDF final ShortAD cannot accommodate growth");
                put32(fe,target.fe.ad_offset+
                      (target.fe.runs.size()-1u)*8u,
                      static_cast<std::uint32_t>(grown));
            }
            tag_update(fe,0,target.fe.ad_offset+target.fe.ads);
            staged_write(out,target.fe.at,fe);
        }
}

// The only in-place growth possible is within the final recorded
// ShortAD's last allocated sector (or within an embedded FE).
std::uint64_t replacement_capacity(const Fe& fe) {
    if(fe.runs.empty())return fe.capacity;
    const auto& last=fe.runs.back();
    const auto last_limit=std::min<std::uint64_t>(
        std::uint64_t(last.blocks)*block,0x3fffffffull);
    need(last.recorded<=last_limit,
         "UDF final ShortAD exceeds its allocated blocks or recorded length");
    return std::min(fe.capacity,fe.recorded+(last_limit-last.recorded));
}

void validate_udf_existing_change(const Type1& volume,
                                  const ArchiveChange& change,
                                  bool allow_relocation=false,
                                  bool preserve_shared_payload=false) {
        const auto name=edit_path(change.path);
        const auto file_it=volume.files().find(name);
        const auto dir_it=volume.directory_links().find(name);
        const bool directory=dir_it!=volume.directory_links().end();
        need(file_it!=volume.files().end() || directory,
             "UDF existing edit target does not exist");
        need(directory?change.remove:!change.directory,
             "UDF only supports removing empty directories, not replacing them");
        const auto& target=directory?dir_it->second:file_it->second;
        need(target.fe.type==(directory?4u:5u),
             "UDF existing entry type does not match namespace");
        if(directory) {
            need(volume.has_allocation_map(),
                 "UDF empty directory deletion needs allocation map/LVID");
            const auto prefix=name+"/";
            for(const auto& [key,entry]:volume.files()) {
                (void)entry;
                need(!key.starts_with(prefix),
                     "UDF directory is not empty (contains a file)");
            }
            for(const auto& [key,entry]:volume.directories()) {
                (void)entry;
                need(!key.starts_with(prefix),
                     "UDF directory is not empty (contains a subdirectory)");
            }
        }
        // A differently named UDF alias can point at an ISO-owned file.
        // Refuse changes and reclamation before the expensive staged copy.
        need(!volume.overlaps_external(target.fe.at,block),
             "UDF file descriptor intersects immutable ISO/boot metadata");
        for(const auto& run:target.fe.runs)
            need(preserve_shared_payload || !volume.overlaps_external(
                     run.at,std::uint64_t(run.blocks)*block),
                 "UDF file data intersects immutable ISO/boot extent");
        if(!change.remove) {
            need(std::filesystem::is_regular_file(
                     std::filesystem::symlink_status(change.source)),
                 "UDF replacement must be a regular file, not symlink");
            const auto len=std::filesystem::file_size(change.source);
            // Only the last ShortAD's unused final sector can grow without
            // moving data or changing the order of existing logical runs.
            // Earlier runs may have padding but extending them would shift
            // every subsequent file offset. Keep the growth fully in place.
            const auto available=replacement_capacity(target.fe);
            need(allow_relocation || len<=available,
                 "UDF replacement exceeds existing ShortAD allocation");
            for(const auto& run:target.fe.runs) {
                const Location dest{run.at,std::uint64_t(run.blocks)*block};
                for(const auto& interval:volume.intervals())
                    if(interval.owner!=target.fe.at)
                        need(!intersects(dest,interval.range),
                             "UDF file extent overlaps another file/descriptor");
            }
        }
}

// Plan every new child FE, file run and parent FID before editing a single
// byte of the source. Each modified parent is relocated at most once.
// The staged image is copied once, validated once and atomically committed
// only after *all* requested paths and file contents verify successfully.
void create_udf_type1_batch(const std::filesystem::path& image,
                            Type1& volume,
                            const std::vector<ArchiveChange>& changes,
                            const EditProgress& progress,bool allow_bridge,
                            const EditStageValidator& validate_stage,
                            const ReadonlyAppendPlan* mastering,
                            const CoordinatedIsoUpdate* coordinated) {
    need(!changes.empty() && changes.size()<=max_items &&
         volume.has_allocation_map(),
         "UDF transaction needs a valid allocation map and changes");
    struct Prepared {
        ArchiveChange change;
        std::string key,parent;
        Bytes name;
        std::uint64_t length=0;
    };
    auto fold=[](std::string text) {
        for(char& c:text)
            if(c>='a' && c<='z')c=static_cast<char>(c-'a'+'A');
        return text;
    };
    std::set<std::string> names;
    for(const auto& [key,item]:volume.files()) {
        (void)item;
        need(names.insert(fold(key)).second,
             "UDF existing names conflict case-insensitively");
    }
    for(const auto& [key,item]:volume.directories()) {
        (void)item;
        if(!key.empty())
            need(names.insert(fold(key)).second,
                 "UDF existing directory names conflict case-insensitively");
    }
    std::vector<Prepared> prepared;
    prepared.reserve(changes.size());
    std::vector<ArchiveChange> existing_changes;
    existing_changes.reserve(changes.size());
    std::set<std::string> changed_names;
    for(const auto& change:changes) {
        const auto key=edit_path(change.path);
        need(changed_names.insert(fold(key)).second,
             "UDF batch contains duplicate or case-colliding changes");
        if(volume.files().count(key) || volume.directory_links().count(key)) {
            const bool shared=coordinated &&
                (coordinated->shared_replacements.count(key)!=0 ||
                 coordinated->shared_deletions.count(key)!=0);
            need(!shared || (mastering && !change.directory &&
                 (change.remove?coordinated->shared_deletions.count(key):
                                coordinated->shared_replacements.count(key))),
                 "shared ISO/UDF edit needs matching append mastering plan");
            validate_udf_existing_change(volume,change,mastering!=nullptr,shared);
            existing_changes.push_back(change);
            continue;
        }
        need(!change.remove,"UDF cannot delete a nonexistent file");
        need(!volume.directories().count(key),
             "UDF cannot replace or recreate an existing directory");
        Prepared p;
        p.change=change;
        p.key=key;
        need(names.insert(fold(p.key)).second,
             "UDF new name collides with an existing file or directory");
        const auto slash=p.key.rfind('/');
        p.parent=slash==std::string::npos?
                 std::string{}:p.key.substr(0,slash);
        const auto component=slash==std::string::npos?
                             p.key:p.key.substr(slash+1);
        p.name=encode_udf_cs0(component);
        need(component.find('\\')==std::string::npos &&
             component.find('/')==std::string::npos,
             "UDF batch contains invalid path separators");
        const auto fid_size=(38u+p.name.size()+3u)&~std::size_t(3);
        need(fid_size>=40 && fid_size<=255,
             "UDF batch FID is too large");
        if(!change.directory) {
            need(std::filesystem::is_regular_file(
                     std::filesystem::symlink_status(change.source)),
                 "UDF batch source must be a regular non-symlink file");
            p.length=std::filesystem::file_size(change.source);
            need(p.length<=volume.image_size(),
                 "UDF batch file exceeds image capacity");
        }
        prepared.push_back(std::move(p));
    }
    // An empty directory cannot be removed and simultaneously populated.
    for(const auto& change:existing_changes) {
        const auto key=edit_path(change.path);
        if(!change.remove || !volume.directory_links().count(key))continue;
        const auto prefix=key+"/";
        for(const auto& p:prepared)
            need(!p.key.starts_with(prefix),
                 "UDF cannot add a child beneath a deleted directory");
    }
    // Depth ordering lets callers submit children before the mkdir entry,
    // without ever accepting an unresolved parent or file-as-parent.
    const auto depth=[](const Prepared& p) {
        return std::count(p.key.begin(),p.key.end(),'/');
    };
    std::stable_sort(prepared.begin(),prepared.end(),
        [&](const Prepared& a,const Prepared& b) {
            return depth(a)<depth(b);
        });

    struct FidPlan {
        Bytes name;
        std::uint32_t child_lba=0;
        bool directory=false;
        std::optional<ReusedFid> reused;
    };
    struct ParentPlan {
        Fe original;
        Bytes payload;
        std::vector<FidPlan> children;
        std::vector<FreeFidSlot> free_slots;
        std::set<std::size_t> reused_offsets;
        bool preexisting=false;
        std::uint32_t final_lba=0;
        std::uint32_t old_blocks=0;
        std::uint32_t new_blocks=0;
        bool external=false;
        Bytes fe_write,content_write;
    };
    struct ChildPlan {
        std::string key;
        std::filesystem::path source;
        bool directory=false;
        std::uint64_t length=0;
        std::uint32_t lba=0;
        Bytes fe;
        std::vector<DataRun> runs;
    };
    std::map<std::string,ParentPlan> parents;
    std::vector<ChildPlan> children;
    children.reserve(prepared.size());
    std::uint64_t allocated=0;
    std::uint32_t files_added=0,dirs_added=0;
    constexpr std::size_t fe_header=176;
    const auto record_allocation=[&](std::uint64_t count) {
        need(count<=std::numeric_limits<std::uint32_t>::max()-allocated,
             "UDF batch block counter overflow");
        allocated+=count;
    };
    for(const auto& p:prepared) {
        edit_tick(progress);
        auto parent_it=parents.find(p.parent);
        if(parent_it==parents.end()) {
            const auto original=volume.directories().find(p.parent);
            need(original!=volume.directories().end(),
                 "UDF batch parent does not exist or is not a directory");
            ParentPlan state;
            state.original=original->second;
            state.payload=volume.directory_payload(state.original);
            state.free_slots=collect_free_fid_slots(
                state.payload,state.original.storage==3?
                    volume.logical_of(state.original.at):
                    volume.logical_of(state.original.data_at));
            state.preexisting=true;
            parent_it=parents.emplace(p.parent,std::move(state)).first;
        }
        auto& parent=parent_it->second;
        const auto& parent_fe=parent.original;
        need(parent_fe.type==4 &&
             (parent_fe.storage==0 || parent_fe.storage==3) &&
             parent_fe.size==parent.payload.size() &&
             parent_fe.size<=parent_fe.capacity &&
             parent_fe.ad_offset<=block &&
             parent_fe.ads<=block-parent_fe.ad_offset &&
             (parent_fe.storage!=3 || parent_fe.size<=parent_fe.ads) &&
             (parent_fe.storage!=0 || parent_fe.runs.size()==1),
             "UDF batch parent allocation layout is unsupported");
        ChildPlan child;
        child.key=p.key;
        child.source=p.change.source;
        child.directory=p.change.directory;
        child.length=p.length;
        child.lba=volume.allocate_block();
        record_allocation(1);
        const bool shared=coordinated &&
            coordinated->shared_additions.count(p.key)!=0;
        need(!shared || (mastering && !p.change.directory &&
                         p.length>0 && p.length<=0xffffffffull),
             "shared ISO/UDF creation requires external ISO-compatible data");
        const auto embedded=!shared && p.length<=block-fe_header;
        const auto file_blocks=embedded?
            0ull:(p.length+block-1u)/block;
        need(file_blocks<=std::numeric_limits<std::uint32_t>::max(),
             "UDF batch file exceeds partition block counters");
        if(file_blocks) {
            child.runs=volume.allocate_file_data(
                p.length,(block-fe_header)/8u);
            record_allocation(file_blocks);
        }
        child.fe.assign(block,0);
        put16(child.fe,0,261);
        put16(child.fe,2,2);
        put32(child.fe,12,child.lba);
        child.fe[27]=p.change.directory?4u:5u;
        put16(child.fe,34,embedded?3u:0u);
        put32(child.fe,36,0xffffffffu);
        put32(child.fe,40,0xffffffffu);
        put32(child.fe,44,p.change.directory?0x000001ffu:0x000001b6u);
        put16(child.fe,48,1);
        put64(child.fe,56,p.length);
        put64(child.fe,64,file_blocks);
        put32(child.fe,108,1);
        put64(child.fe,160,volume.new_unique_id(children.size()));
        const auto ad_bytes=8u*child.runs.size();
        need(ad_bytes<=block-fe_header,
             "UDF batch child ShortAD array exceeds FE");
        put32(child.fe,168,0);
        put32(child.fe,172,embedded?
              static_cast<std::uint32_t>(p.length):
              static_cast<std::uint32_t>(ad_bytes));
        if(!embedded) {
            for(std::size_t i=0;i<child.runs.size();++i) {
                const auto& run=child.runs[i];
                need(run.recorded<=0x3fffffffull,
                     "UDF batch ShortAD exceeds 30-bit length");
                put32(child.fe,fe_header+i*8u,
                      static_cast<std::uint32_t>(run.recorded));
                put32(child.fe,fe_header+i*8u+4u,run.logical);
            }
        } else if(p.length) {
            std::ifstream input(p.change.source,std::ios::binary);
            need(bool(input),"cannot open embedded UDF batch source");
            input.read(reinterpret_cast<char*>(
                           child.fe.data()+fe_header),
                       static_cast<std::streamsize>(p.length));
            need(bool(input),"embedded UDF batch source is truncated");
        }
        tag_update(child.fe,0,fe_header+(embedded?
                   static_cast<std::size_t>(p.length):ad_bytes));
        const auto reused=choose_reused_fid(
            parent.free_slots,p.name.size(),parent.reused_offsets);
        if(reused)
            need(parent.reused_offsets.insert(reused->offset).second,
                 "UDF free FID slot allocated twice in one transaction");
        parent.children.push_back({
            p.name,child.lba,p.change.directory,reused});
        if(p.change.directory) {
            ParentPlan nested;
            nested.original.bytes=child.fe;
            nested.original.at=volume.absolute(child.lba);
            nested.original.data_at=nested.original.at+fe_header;
            nested.original.size=0;
            nested.original.capacity=block-fe_header;
            nested.original.recorded=0;
            nested.original.base=fe_header;
            nested.original.ad_offset=fe_header;
            nested.original.ads=0;
            nested.original.storage=3;
            nested.original.type=4;
            need(parents.emplace(p.key,std::move(nested)).second,
                 "duplicate UDF batch-created directory");
            ++dirs_added;
        } else {
            ++files_added;
        }
        children.push_back(std::move(child));
    }

    // DVD-ROM mastering can resize a file beyond its original allocation.
    // Keep its FE/ICB identity, unique ID, attributes and timestamps. New
    // payload runs are exclusively in the append area. The obsolete
    // payload remains unadvertised and is never reused by this mastering.
    struct ReplacementPlan {
        std::filesystem::path source;
        std::uint64_t at{},length{};
        Bytes fe;
        std::vector<DataRun> runs;
    };
    std::vector<ReplacementPlan> replacements;
    if(mastering)for(const auto& change:existing_changes) {
        if(change.remove)continue;
        edit_tick(progress);
        const auto& old=volume.files().at(edit_path(change.path)).fe;
        ReplacementPlan plan;
        plan.source=change.source;
        plan.at=old.at;
        plan.length=std::filesystem::file_size(change.source);
        plan.fe=old.bytes;
        const bool extended=u16(plan.fe,0)==266;
        const auto header=old.ad_offset;
        need(header<=block,"UDF replacement FE allocation header exceeds sector");
        std::fill(plan.fe.begin()+static_cast<std::ptrdiff_t>(header),
                  plan.fe.end(),std::uint8_t{0});
        const bool shared=coordinated &&
            coordinated->shared_replacements.count(edit_path(change.path))!=0;
        need(!shared || (plan.length>0 && plan.length<=0xffffffffull),
             "shared replacement exceeds single ISO record size");
        const bool embedded=!shared && plan.length<=block-header;
        const auto blocks=embedded?0ull:(plan.length+block-1u)/block;
        if(!embedded) {
            need(header+8u<=block,"UDF replacement FE has no ShortAD slots");
            plan.runs=volume.allocate_file_data(plan.length,(block-header)/8u);
            record_allocation(blocks);
        }
        put16(plan.fe,34,static_cast<std::uint16_t>(
              (u16(plan.fe,34)&~7u)|(embedded?3u:0u)));
        put64(plan.fe,56,plan.length);
        if(extended)put64(plan.fe,64,plan.length);
        put64(plan.fe,extended?72u:64u,blocks);
        const auto ads=embedded?static_cast<std::size_t>(plan.length):
                                plan.runs.size()*8u;
        put32(plan.fe,extended?212u:172u,static_cast<std::uint32_t>(ads));
        if(embedded) {
            std::ifstream input(plan.source,std::ios::binary);
            need(bool(input),"cannot open remastered UDF replacement");
            if(plan.length) {
                input.read(reinterpret_cast<char*>(plan.fe.data()+header),
                           static_cast<std::streamsize>(plan.length));
                need(bool(input),"remastered UDF replacement source shortened");
            }
            need(input.peek()==std::char_traits<char>::eof(),
                 "remastered UDF replacement source grew");
        } else for(std::size_t i=0;i<plan.runs.size();++i) {
            put32(plan.fe,header+i*8u,
                  static_cast<std::uint32_t>(plan.runs[i].recorded));
            put32(plan.fe,header+i*8u+4u,plan.runs[i].logical);
        }
        tag_update(plan.fe,0,header+ads);
        replacements.push_back(std::move(plan));
    }

    // Apply logical deletes to the same in-memory directory payload used
    // for FID appends and relocations. Never write a stale parent FE later:
    // an embedded parent checksum must cover both mutations together.
    for(const auto& change:existing_changes) {
        if(!change.remove)continue;
        edit_tick(progress);
        const auto key=edit_path(change.path);
        const auto file_it=volume.files().find(key);
        const auto& existing=file_it!=volume.files().end()?
            file_it->second:volume.directory_links().at(key);
        const auto slash=key.rfind('/');
        const auto parent_key=slash==std::string::npos?
                              std::string{}:key.substr(0,slash);
        auto at=parents.find(parent_key);
        if(at==parents.end()) {
            const auto old=volume.directories().find(parent_key);
            need(old!=volume.directories().end(),
                 "UDF deleted FID parent is missing");
            ParentPlan plan;
            plan.original=old->second;
            plan.payload=volume.directory_payload(plan.original);
            plan.preexisting=true;
            at=parents.emplace(parent_key,std::move(plan)).first;
        }
        auto& parent=at->second;
        need(parent.original.at==existing.parent_at &&
             existing.fid_at>=parent.original.data_at,
             "UDF deleted FID refers to the wrong parent");
        const auto relative=existing.fid_at-parent.original.data_at;
        need(relative<=parent.payload.size() &&
             existing.fid_bytes<=parent.payload.size()-relative,
             "UDF deleted FID lies outside directory payload");
        const auto p=static_cast<std::size_t>(relative);
        tag_check(parent.payload,p,257);
        need(!(parent.payload[p+18]&0x0cu) &&
             u32(parent.payload,p+24)==
                 volume.logical_of(existing.fe.at),
             "UDF deleted FID identity or state mismatch");
        parent.payload[p+18]|=0x04u;
        // A freed FE must never remain reachable by a tombstone long_ad.
        // Zero only the FID ICB extent (16 bytes), retaining its diagnostic
        // name, version and deleted flag. Recalculate descriptor CRC.
        std::fill(parent.payload.begin()+static_cast<std::ptrdiff_t>(p+20u),
                  parent.payload.begin()+static_cast<std::ptrdiff_t>(p+36u),
                  std::uint8_t{0});
        tag_update(parent.payload,p,existing.fid_bytes);
    }

    // Allocate all destination parent runs before releasing old ones.
    // Thus a later child allocation can never reuse a block scheduled for
    // zeroing after an original directory is relocated.
    for(auto& [key,parent]:parents) {
        (void)key;
        edit_tick(progress);
        const auto& fe=parent.original;
        if(parent.preexisting) {
            need(!volume.overlaps_external(fe.at,block),
                 "UDF parent descriptor overlaps immutable ISO metadata");
            for(const auto& run:fe.runs)
                need(!volume.overlaps_external(run.at,std::uint64_t(run.blocks)*block),
                     "UDF parent directory overlaps immutable ISO data");
        }
        std::uint64_t length=parent.payload.size();
        for(const auto& fid:parent.children) {
            if(fid.reused)continue;
            const auto bytes=(38u+fid.name.size()+3u)&~std::size_t(3);
            need(bytes<=max_dir-length,
                 "UDF batch directory exceeds 8-MiB limit");
            length+=bytes;
        }
        need(length<=max_dir &&
             fe.ad_offset<=block && fe.ads<=block-fe.ad_offset,
             "UDF batch parent descriptor is invalid");
        const bool relocate=length>fe.capacity;
        parent.external=relocate || fe.storage==0;
        parent.new_blocks=relocate?
            static_cast<std::uint32_t>((length+block-1u)/block):0u;
        parent.old_blocks=relocate && fe.storage==0?
            static_cast<std::uint32_t>(fe.capacity/block):0u;
        parent.final_lba=relocate?
            volume.allocate_blocks(parent.new_blocks):
            (fe.storage==0?volume.logical_of(fe.data_at):0u);
        record_allocation(parent.new_blocks);

        parent.fe_write=fe.bytes;
        if(!parent.external) {
            need(fe.ad_offset<=block &&
                 parent.payload.size()<=block-fe.ad_offset,
                 "UDF embedded directory payload exceeds FE");
            std::copy(parent.payload.begin(),parent.payload.end(),
                      parent.fe_write.begin()+
                      static_cast<std::ptrdiff_t>(fe.ad_offset));
        }
        put64(parent.fe_write,56,length);
        const bool extended=u16(parent.fe_write,0)==266;
        if(extended)put64(parent.fe_write,64,length);
        const auto ad_length_field=extended?212u:172u;
        if(parent.external) {
            parent.content_write=parent.payload;
            if(relocate && !parent.payload.empty())
                retag_directory_fids(parent.content_write,parent.final_lba);
            const auto capacity=relocate?
                std::uint64_t(parent.new_blocks)*block:fe.capacity;
            need(length<=capacity && capacity<=max_dir,
                 "UDF batch relocated directory capacity is invalid");
            parent.content_write.resize(static_cast<std::size_t>(capacity),0);
        }
        std::size_t at=parent.payload.size();
        for(const auto& item:parent.children) {
            const auto fid_at=item.reused?item.reused->offset:at;
            const auto bytes=item.reused?
                item.reused->span:
                (38u+item.name.size()+3u)&~std::size_t(3);
            const auto implementation=item.reused?
                item.reused->implementation:0u;
            need(fid_at<=length && bytes<=length-fid_at &&
                 implementation<=bytes-38u-item.name.size() &&
                 implementation<=65535u,
                 "UDF reused FID exceeds its original byte span");
            Bytes fid(bytes,0);
            put16(fid,0,257);
            put16(fid,2,2);
            put32(fid,12,parent.external?
                  parent.final_lba+static_cast<std::uint32_t>(fid_at/block):
                  volume.logical_of(fe.at));
            put16(fid,16,1);
            fid[18]=item.directory?2u:0u;
            fid[19]=static_cast<std::uint8_t>(item.name.size());
            put32(fid,20,static_cast<std::uint32_t>(block));
            put32(fid,24,item.child_lba);
            put16(fid,28,0);
            if(implementation) {
                // A nonempty FID ImplementationUse starts with a regid
                // EntityID (32 bytes). The remaining bytes are all zero.
                need(implementation>=32u,
                     "UDF FID ImplementationUse cannot omit the EntityID");
                const std::string_view identifier="*ISO-WCX";
                std::copy(identifier.begin(),identifier.end(),fid.begin()+39);
            }
            put16(fid,36,static_cast<std::uint16_t>(implementation));
            std::copy(item.name.begin(),item.name.end(),
                      fid.begin()+static_cast<std::ptrdiff_t>(
                          38u+implementation));
            tag_update(fid,0,fid.size());
            if(parent.external)
                std::copy(fid.begin(),fid.end(),
                          parent.content_write.begin()+
                          static_cast<std::ptrdiff_t>(fid_at));
            else
                std::copy(fid.begin(),fid.end(),
                          parent.fe_write.begin()+
                          static_cast<std::ptrdiff_t>(fe.ad_offset+fid_at));
            if(!item.reused)at+=fid.size();
        }
        need(at==length,"UDF batch FID payload length mismatch");
        if(relocate) {
            need(fe.ad_offset+8u<=block,
                 "UDF batch cannot relocate parent with nonempty AD header");
            std::fill(parent.fe_write.begin()+
                      static_cast<std::ptrdiff_t>(fe.ad_offset),
                      parent.fe_write.end(),std::uint8_t{0});
            put16(parent.fe_write,34,
                  static_cast<std::uint16_t>(u16(parent.fe_write,34)&~7u));
            put32(parent.fe_write,ad_length_field,8u);
            put32(parent.fe_write,fe.ad_offset,
                  static_cast<std::uint32_t>(length));
            put32(parent.fe_write,fe.ad_offset+4u,parent.final_lba);
            put64(parent.fe_write,extended?72u:64u,parent.new_blocks);
        } else if(parent.external) {
            put32(parent.fe_write,fe.ad_offset,
                  static_cast<std::uint32_t>(length));
        } else {
            put32(parent.fe_write,ad_length_field,
                  static_cast<std::uint32_t>(length));
        }
        tag_update(parent.fe_write,0,parent.external?
                   fe.ad_offset+8u:fe.ad_offset+
                   static_cast<std::size_t>(length));
    }
    std::uint64_t released=0;
    for(const auto& [key,parent]:parents) {
        (void)key;
        if(parent.old_blocks) {
            volume.release_directory_blocks(
                parent.original.at,parent.original.data_at,
                parent.old_blocks);
            released+=parent.old_blocks;
        }
    }
    std::vector<Location> deleted_regions;
    std::uint64_t deleted_blocks=0;
    std::uint32_t deleted_files=0,deleted_dirs=0;
    for(const auto& change:existing_changes) {
        if(!change.remove)continue;
        edit_tick(progress);
        const auto key=edit_path(change.path);
        const bool is_directory=volume.directory_links().count(key)!=0;
        const auto& entry=is_directory?
            volume.directory_links().at(key):volume.files().at(key);
        const bool preserve=mastering && coordinated &&
            coordinated->shared_deletions.count(key)!=0;
        const auto regions=volume.reclaim_entry_blocks(entry,is_directory,preserve);
        for(const auto& region:regions) {
            need(region.bytes%block==0 &&
                 region.bytes/block<=
                     std::numeric_limits<std::uint32_t>::max()-deleted_blocks,
                 "UDF deleted file allocation counter overflow");
            deleted_blocks+=region.bytes/block;
            deleted_regions.push_back(region);
        }
        auto& deleted_count=is_directory?deleted_dirs:deleted_files;
        need(deleted_count<std::numeric_limits<std::uint32_t>::max(),
             "UDF deleted entry count overflow");
        ++deleted_count;
    }
    need(released<=std::numeric_limits<std::uint32_t>::max()-
         deleted_blocks,
         "UDF aggregate released block counter overflow");
    released+=deleted_blocks;

    EditTransaction tx(image);
    prepare_udf_stage(image,tx.output,
                      mastering?mastering->original_size:volume.image_size(),progress);
    if(mastering)std::filesystem::resize_file(tx.output,volume.image_size());
    std::fstream out(tx.output,std::ios::binary|std::ios::in|
                     std::ios::out);
    need(bool(out),"cannot open prepared UDF transaction stage");
    volume.close();
    for(const auto& child:children) {
        edit_tick(progress);
        staged_write(out,volume.absolute(child.lba),child.fe);
        if(!child.runs.empty()) {
            std::ifstream source(child.source,std::ios::binary);
            need(bool(source),"cannot open UDF batch data source");
            std::uint64_t copied=0;
            for(const auto& run:child.runs) {
                out.clear();
                out.seekp(static_cast<std::streamoff>(run.at));
                need(bool(out),"cannot seek UDF batch data extent");
                copy(source,out,run.recorded,progress);
                copied+=run.recorded;
                const auto tail=std::uint64_t(run.blocks)*block-
                                run.recorded;
                if(tail)
                    staged_write(out,run.at+run.recorded,
                                 Bytes(static_cast<std::size_t>(tail),0));
            }
            need(copied==child.length,
                 "UDF batch data source length changed during writing");
        }
    }
    for(const auto& plan:replacements) {
        edit_tick(progress);
        if(!plan.runs.empty()) {
            std::ifstream source(plan.source,std::ios::binary);
            need(bool(source),"cannot open remastered UDF data");
            std::uint64_t copied=0;
            for(const auto& run:plan.runs) {
                out.clear();
                out.seekp(static_cast<std::streamoff>(run.at));
                need(bool(out),"cannot seek remastered UDF data extent");
                copy(source,out,run.recorded,progress);
                copied+=run.recorded;
                const auto tail=std::uint64_t(run.blocks)*block-run.recorded;
                if(tail)staged_write(out,run.at+run.recorded,
                                      Bytes(static_cast<std::size_t>(tail),0));
            }
            need(copied==plan.length &&
                 source.peek()==std::char_traits<char>::eof(),
                 "remastered UDF replacement length changed");
        }
        staged_write(out,plan.at,plan.fe);
    }
    if(!mastering)for(const auto& change:existing_changes) {
        edit_tick(progress);
        if(change.remove)continue; // FID was patched in its parent payload.
        write_udf_existing_replacement(
            out,volume.files().at(edit_path(change.path)),
            change.source,progress);
    }
    for(const auto& [key,parent]:parents) {
        (void)key;
        edit_tick(progress);
        if(parent.old_blocks)
            staged_write(out,parent.original.data_at,
                         Bytes(std::size_t(parent.old_blocks)*block,0));
        if(parent.external)
            staged_write(out,volume.absolute(parent.final_lba),
                         parent.content_write);
        staged_write(out,parent.original.at,parent.fe_write);
    }
    // Wipe the released FE and every recorded ShortAD sector only after
    // parent FIDs have lost their ICB pointers. No new allocation reuses
    // these blocks in the current transaction.
    for(const auto& region:deleted_regions)
        staged_zero(out,region.at,region.bytes,progress);
    if(mastering) {
        for(const auto& [at,bytes]:mastering->descriptors) {
            edit_tick(progress);
            staged_write(out,at,bytes);
        }
    } else {
        const auto [bitmap_at,bitmap_bytes]=volume.updated_bitmap();
        staged_write(out,bitmap_at,bitmap_bytes);
    }
    auto [lvid_at,lvid_bytes]=volume.updated_integrity_delta(
        files_added,dirs_added,deleted_files,deleted_dirs,allocated,released);
    if(mastering) {
        put32(lvid_bytes,80,0); // read-only: no advertised writable space
        tag_update(lvid_bytes,0,16u+u16(lvid_bytes,10));
    }
    staged_write(out,lvid_at,lvid_bytes);
    out.flush();
    need(bool(out),"cannot flush staged UDF batch image");
    out.close();

    if(mastering) {
        // Type1 validates the main sequence and namespace, but the
        // read-only parser does not consume the LVID or reserve VDS.
        // Independently read back all newly written mastering metadata.
        std::ifstream check(tx.output,std::ios::binary);
        need(bool(check),"cannot reopen mastered UDF metadata");
        auto verify=[&](std::uint64_t at,const Bytes& expected) {
            edit_tick(progress);
            Bytes actual(expected.size());
            check.clear();
            check.seekg(static_cast<std::streamoff>(at));
            check.read(reinterpret_cast<char*>(actual.data()),
                       static_cast<std::streamsize>(actual.size()));
            need(bool(check) && actual==expected,
                 "UDF mastering metadata readback differs");
            tag_check(actual,0,u16(expected,0));
        };
        for(const auto& [at,bytes]:mastering->descriptors)verify(at,bytes);
        verify(lvid_at,lvid_bytes);
        check.close();
    }

    // A strict second parse detects invalid tag CRC/location, FE/FID aliases,
    // bitmap/LVID mismatches and parent allocation errors. Read every new
    // file through Disc extents and compare its complete source stream.
    {
        Type1 strict(tx.output,allow_bridge,volume.bounded_partitioned_bridge());
        // A mastered ROM intentionally has no persistent free-space map.
        // Its deletion is checked by namespace and data verification below.
        if(!mastering)for(const auto& region:deleted_regions) {
            const auto start=strict.logical_of(region.at);
            for(std::uint64_t i=0;i<region.bytes/block;++i)
                need(strict.is_free(start+static_cast<std::uint32_t>(i)),
                     "UDF reclaimed sector remains allocated after staging");
        }
        for(const auto& child:children) {
            need(child.directory?
                 strict.directories().count(child.key)!=0:
                 strict.files().count(child.key)!=0,
                 "UDF batch staged namespace is incomplete");
        }
        for(const auto& change:existing_changes) {
            const auto key=edit_path(change.path);
            const bool is_directory=volume.directory_links().count(key)!=0;
            const bool visible=is_directory?
                strict.directories().count(key)!=0:
                strict.files().count(key)!=0;
            need(visible==!change.remove,
                 "UDF mixed batch deletion/replacement namespace mismatch");
        }
        strict.close();
        const Disc inspected(tx.output);
        need(inspected.format()=="UDF",
             "UDF batch staged filesystem is unreadable");
        std::map<std::string,std::size_t> by_path;
        for(std::size_t i=0;i<inspected.items().size();++i)
            need(by_path.emplace(inspected.items()[i].path,i).second,
                 "duplicate staged UDF batch name");
        for(const auto& change:existing_changes) {
            edit_tick(progress);
            const auto key=edit_path(change.path);
            const auto at=by_path.find(key);
            if(change.remove) {
                need(at==by_path.end(),
                     "UDF mixed batch deleted file remains visible");
            } else {
                need(at!=by_path.end() &&
                     !inspected.items()[at->second].directory &&
                     inspected.items()[at->second].size==
                         std::filesystem::file_size(change.source),
                     "UDF mixed batch replacement size mismatch");
                need(same_staged_item(tx.output,inspected.items()[at->second],
                                      change.source,progress),
                     "UDF mixed batch replacement content mismatch");
            }
        }
        for(const auto& child:children) {
            edit_tick(progress);
            const auto at=by_path.find(child.key);
            need(at!=by_path.end(),
                 "UDF batch child is not visible in staged image");
            const auto& entry=inspected.items()[at->second];
            need(entry.directory==child.directory &&
                 (child.directory || entry.size==child.length),
                 "UDF batch child type/size mismatch");
            if(!child.directory)
                need(same_staged_item(tx.output,entry,child.source,progress),
                     "UDF batch staged file differs from source");
        }
    }
    if(validate_stage)validate_stage(tx.output);
    tx.commit(progress);
}


// Standalone open write-once UDF 1.50 / 2.x images with a clean
// VAT ICB at actual EOF and declared unrecorded partition capacity.
// No original sector is overwritten. Directory metadata uses virtual
// addresses; bulk file data and external VAT storage use physical ADs.
// Keep each revision's on-disc VAT layout and FID conventions distinct.
struct VatFileLayout {
    bool directory=false;
    std::uint64_t size{};
    Bytes embedded;
    std::vector<Extent> extents;
};
struct VatPayload {
    std::filesystem::path source;
    std::uint64_t size{},blocks{};
};
struct VatAppendPlan {
    std::uint64_t original_size{},final_size{},payload_blocks{};
    std::uint64_t iso_reservation_at{},iso_reservation_bytes{};
    Bytes mbr; // exact independently validated replacement header, or empty
    std::vector<VatPayload> payloads;
    std::vector<Bytes> append;
    std::map<std::string,VatFileLayout> expected;
    std::map<std::string,std::filesystem::path> verify_sources;
};
constexpr std::uint64_t vat_max_table=16u*1024u*1024u;
constexpr std::uint64_t vat_max_ad=0x3ffff800ull;

bool vat_layout_matches(const Item& item,const VatFileLayout& wanted) {
    if(item.directory!=wanted.directory || !item.direct_source.empty() ||
       item.size!=wanted.size || item.inline_data!=wanted.embedded ||
       item.extents.size()!=wanted.extents.size())return false;
    for(std::size_t j=0;j<wanted.extents.size();++j) {
        const auto& a=item.extents[j];
        const auto& b=wanted.extents[j];
        if(a.offset!=b.offset || a.length!=b.length || a.sparse!=b.sparse)
            return false;
    }
    return true;
}

void vat_need(bool ok,const char* reason) {
    if(!ok)throw EditUnsupported(std::string("UDF VAT virtual partition: ")+reason);
}
Bytes vat_read_sector(std::ifstream& in,std::uint64_t lba,std::uint64_t sectors) {
    vat_need(lba<sectors,"descriptor outside recorded image");
    Bytes b(block);
    in.clear();in.seekg(static_cast<std::streamoff>(lba*block));
    in.read(reinterpret_cast<char*>(b.data()),static_cast<std::streamsize>(b.size()));
    vat_need(bool(in),"cannot read descriptor sector");
    return b;
}
void vat_tag(const Bytes& b,std::uint16_t type,std::uint32_t lba,
             std::size_t used) {
    tag_check(b,0,type);
    vat_need(used>=16 && used<=b.size() &&
             u32(b,12)==lba && u16(b,10)>=used-16,
             "descriptor tag location or CRC coverage mismatch");
}
struct VatVolumeDescriptors {
    Bytes pd,lvd;
};
VatVolumeDescriptors vat_vds(std::ifstream& in,std::uint64_t sectors,
        std::uint32_t start,std::uint32_t bytes) {
    vat_need(bytes>=3*block && bytes%block==0 && bytes<=16u*1024u*1024u &&
             start>=257 && start<sectors && bytes/block<=sectors-start,
             "unsupported descriptor sequence geometry");
    VatVolumeDescriptors v;
    bool terminated=false;
    for(std::uint32_t j=0;j<bytes/block;++j) {
        auto b=vat_read_sector(in,std::uint64_t(start)+j,sectors);
        const auto id=u16(b,0);
        vat_tag(b,id,start+j,id==6?510u:512u);
        if(id==5) {
            vat_need(v.pd.empty(),"multiple physical partitions");
            v.pd=std::move(b);
        } else if(id==6) {
            vat_need(v.lvd.empty(),"multiple logical volumes");
            v.lvd=std::move(b);
        } else if(id==8) {
            terminated=true;break;
        } else vat_need(id==1 || id==4 || id==7,
                        "unsupported or chained volume descriptor");
    }
    vat_need(terminated && !v.pd.empty() && !v.lvd.empty(),
             "incomplete volume descriptor sequence");
    return v;
}
Bytes vat_embedded_fe(Bytes b,std::uint32_t lba,std::uint8_t type) {
    vat_need(u16(b,0)==261 && b[27]==type && u16(b,20)==4 &&
             u16(b,24)==1 &&
             (type==4?u16(b,48)>=1:(type==0?u16(b,48)==0:u16(b,48)==1)) &&
             u16(b,34)==3 &&
             u32(b,168)==0 && u64(b,64)==0 &&
             u64(b,56)==u32(b,172) && u32(b,172)<=block-176,
             "requires an ordinary embedded FE with no extended attributes");
    vat_tag(b,261,lba,176u+u32(b,172));
    return b;
}
// Validate FE and every physical data run independently from the
// permissive Disc reader. Reject unrecorded extents, continuations,
// virtual data references and references beyond the current VAT ICB.
VatFileLayout vat_physical_file(const Bytes& b,std::uint32_t lba,
        std::uint8_t type,std::uint32_t partition,std::uint32_t old_vat) {
    vat_need(u16(b,0)==261 && b[27]==type && u16(b,20)==4 &&
             u16(b,24)==1 && u16(b,48)==(type==0?0:1) && u32(b,168)==0,
             "requires a single-link ordinary FE without extended attributes");
    const auto mode=u16(b,34);
    VatFileLayout layout;
    layout.size=u64(b,56);
    if(mode==3) {
        vat_embedded_fe(b,lba,type);
        layout.embedded.assign(b.begin()+176,b.begin()+176+u32(b,172));
        return layout;
    }
    vat_need(mode==1 || ((type==248 || type==0) && mode==0),
             "external VAT file data must use physical allocation descriptors");
    const std::size_t step=mode==0?8u:16u;
    const auto ads=u32(b,172);
    vat_need(ads && ads<=block-176 && ads%step==0 && layout.size,
             "invalid external file allocation descriptors");
    vat_tag(b,261,lba,176u+ads);
    std::uint64_t total=0,recorded=0;
    for(std::size_t at=176;at<176u+ads;at+=step) {
        const auto length=u32(b,at);
        const auto logical=u32(b,at+4);
        const auto blocks=(std::uint64_t(length)+block-1)/block;
        vat_need(length && length<=0x3fffffffu &&
                 total<=layout.size && length<=layout.size-total &&
                 (total+length==layout.size || length%block==0) &&
                 logical<old_vat && blocks<=old_vat-logical,
                 "external file extent exceeds recorded partition or information length");
        if(mode==1)
            vat_need(u16(b,at+8)==0 &&
                     std::all_of(b.begin()+at+10,b.begin()+at+16,
                                 [](auto c){return c==0;}),
                     "external file extent must reference the physical partition");
        layout.extents.push_back({(std::uint64_t(partition)+logical)*block,
                                   length,false});
        total+=length;recorded+=blocks;
    }
    vat_need(total==layout.size && recorded==u64(b,64),
             "external FE information length or recorded blocks disagree");
    return layout;
}
Bytes vat_table_bytes(std::ifstream& in,const VatFileLayout& layout) {
    vat_need(layout.size<=vat_max_table,"VAT table exceeds 16 MiB limit");
    if(layout.extents.empty())return layout.embedded;
    Bytes data(static_cast<std::size_t>(layout.size));
    std::size_t pos=0;
    for(const auto& run:layout.extents) {
        in.clear();in.seekg(static_cast<std::streamoff>(run.offset));
        in.read(reinterpret_cast<char*>(data.data()+pos),
                static_cast<std::streamsize>(run.length));
        vat_need(bool(in),"cannot read external VAT table");
        pos+=static_cast<std::size_t>(run.length);
    }
    return data;
}
// Reserve rounded physical spans; a data run may not alias a virtual
// descriptor, VAT storage, or any other live external file.
void vat_reserve_physical(const VatFileLayout& layout,std::uint32_t partition,
        const std::set<std::uint32_t>& mapped,std::uint32_t previous,
        std::map<std::uint32_t,std::uint64_t>& allocated) {
    for(const auto& run:layout.extents) {
        const auto start=static_cast<std::uint32_t>(run.offset/block-partition);
        const auto end=std::uint64_t(start)+(run.length+block-1)/block;
        const auto hit=mapped.lower_bound(start);
        vat_need((hit==mapped.end() || *hit>=end) &&
                 (previous==0xffffffffu || previous<start || previous>=end),
                 "external data aliases VAT metadata");
        const auto next=allocated.lower_bound(start);
        vat_need((next==allocated.end() || next->first>=end) &&
                 (next==allocated.begin() || std::prev(next)->second<=start),
                 "external physical extents overlap");
        allocated.emplace(start,end);
    }
}
// Validate the exact supported MBR geometry independently from the
// ISO planner. Only the container's CHS end and sector count can change.
// Never round after the VAT ICB: alignment belongs before the VAT.
Bytes vat_grown_mbr(std::ifstream& in,std::uint64_t original,
        std::uint64_t final,std::uint64_t alignment) {
    auto mbr=vat_read_sector(in,0,original/block);
    mbr.resize(512);
    vat_need(mbr[510]==0x55 && mbr[511]==0xaa &&
             original%block==0 && final%block==0 && final>original &&
             final/512<=0xffffffffull && alignment && alignment<=65536,
             "invalid MBR VAT growth envelope");
    const auto old_sectors=original/512;
    const auto new_sectors=final/512;
    std::optional<std::size_t> container;
    std::vector<std::pair<std::uint64_t,std::uint64_t>> esp;
    for(std::size_t at=446;at<510;at+=16) {
        const auto type=mbr[at+4];
        if(!type) {
            vat_need(std::all_of(mbr.begin()+at,mbr.begin()+at+16,
                                 [](auto c){return c==0;}),
                     "nonempty unused VAT MBR partition");
            continue;
        }
        const auto first=std::uint64_t(u32(mbr,at+8));
        const auto count=std::uint64_t(u32(mbr,at+12));
        if(type==0x17 && !first) {
            vat_need(!container && mbr[at]==0x80 && count==old_sectors &&
                     mbr[at+1]==0 && mbr[at+2]==1 && mbr[at+3]==0,
                     "VAT MBR requires a whole-image active ISO container");
            container=at;
        } else {
            vat_need(type==0xef && mbr[at]==0 && first>=64 && count &&
                     first<old_sectors && count<=old_sectors-first,
                     "VAT MBR contains unsupported independent partition");
            for(const auto& [begin,length]:esp)
                vat_need(first>=begin+length || begin>=first+count,
                         "overlapping VAT EFI partitions");
            esp.emplace_back(first,count);
        }
    }
    vat_need(container.has_value(),"missing VAT MBR ISO container");
    const auto at=*container;
    const auto heads=std::uint64_t(mbr[at+5])+1;
    const auto sectors=std::uint64_t(mbr[at+6]&0x3fu);
    const auto old_cylinder=std::uint64_t(mbr[at+7]) |
                           (std::uint64_t(mbr[at+6]&0xc0u)<<2);
    vat_need(heads>=2 && heads<=255 && sectors && sectors<=63,
             "invalid VAT MBR CHS geometry");
    const auto geometry=heads*sectors;
    const auto expected_alignment=geometry*(geometry%4==0?1u:geometry%2==0?2u:4u)/4;
    vat_need(alignment==expected_alignment &&
             old_sectors%geometry==0 && old_sectors/geometry>0 &&
             old_cylinder==std::min<std::uint64_t>(old_sectors/geometry-1,1023) &&
             new_sectors%geometry==0 && final/block%alignment==0,
             "VAT MBR alignment or recorded CHS mismatch");
    const auto cylinder=std::min<std::uint64_t>(new_sectors/geometry-1,1023);
    mbr[at+6]=static_cast<std::uint8_t>(sectors|((cylinder>>2)&0xc0u));
    mbr[at+7]=static_cast<std::uint8_t>(cylinder&255u);
    put32(mbr,at+12,static_cast<std::uint32_t>(new_sectors));
    return mbr;
}

std::unique_ptr<VatAppendPlan> prepare_vat_append(
        const std::filesystem::path& image,
        const std::vector<ArchiveChange>& changes,const EditProgress& progress,
        const CoordinatedIsoUpdate* coordinated,std::uint64_t mbr_alignment) {
    edit_tick(progress);
    vat_need(!mbr_alignment || (!coordinated && mbr_alignment<=65536),
             "VAT MBR is limited to a split namespace");
    vat_need(!changes.empty() && changes.size()<=4096,
             "VAT transaction requires 1..4096 changes");
    auto plan=std::make_unique<VatAppendPlan>();
    struct Request {
        std::string key;
        const ArchiveChange* change{};
    };
    auto fold=[](std::string key) {
        for(auto& c:key)if(c>='a' && c<='z')c=static_cast<char>(c-'a'+'A');
        return key;
    };
    auto depth=[](const std::string& key) {
        return std::count(key.begin(),key.end(),'/');
    };
    std::vector<Request> requests;
    requests.reserve(changes.size());
    std::set<std::string> changed_names;
    for(const auto& change:changes) {
        auto key=edit_path(change.path);
        vat_need(depth(key)<max_depth &&
                 changed_names.insert(fold(key)).second,
                 "duplicate, case-colliding or too-deep VAT batch path");
        requests.push_back({std::move(key),&change});
    }
    // Expand recursive removals only after validating the complete
    // on-disc tree and comparing it with the public reader.
    plan->original_size=std::filesystem::file_size(image);
    const auto sectors=plan->original_size/block;
    vat_need(plan->original_size%block==0 && sectors>=300 &&
             plan->original_size<max_image-2*block,"unsupported recorded image size");
    std::ifstream in(image,std::ios::binary);
    vat_need(bool(in),"cannot open sequential image");
    if(coordinated) {
        const bool remaster=coordinated->iso_metadata_blocks!=0;
        vat_need(coordinated->iso_metadata_blocks<=32768 &&
                 (remaster || (coordinated->shared_additions.empty() &&
                               coordinated->shared_directories.empty() &&
                               coordinated->vat_tree_deletions.empty())) &&
                 coordinated->shared_replacements.size()+
                 coordinated->shared_deletions.size()+
                 coordinated->shared_additions.size()+
                 coordinated->shared_directories.size()==changes.size() &&
                 !coordinated->metadata_writes.empty(),
                 "unsupported VAT shared ISO coordination plan");
        for(const auto& request:requests) {
            const auto& change=*request.change;
            const auto& key=request.key;
            const bool match=change.remove?
                ((remaster || !change.directory) && coordinated->shared_deletions.count(key)):
                (change.directory?coordinated->shared_directories.count(key):
                 (coordinated->shared_replacements.count(key)+
                  coordinated->shared_additions.count(key)));
            vat_need(match,"VAT shared ISO request differs from plan");
        }
        std::uint64_t end=16ull*block;
        // The only mutable historical bytes are ISO record fields
        // before the primary AVDP. VRS, boot, UDF VDS, LVID, FSD and
        // all physical partition bytes remain immutable.
        for(const auto& [at,length]:coordinated->metadata_writes) {
            vat_need(length && at>=end && at<256ull*block &&
                     length<=256ull*block-at,
                     "VAT shared ISO metadata is not safely before the UDF anchor");
            end=at+length;
            for(auto sector=at/block;sector<=(end-1)/block;++sector) {
                const auto b=vat_read_sector(in,sector,sectors);
                const std::string_view id(reinterpret_cast<const char*>(b.data()+1),5);
                vat_need(id!="NSR02" && id!="NSR03" && id!="BEA01" &&
                         id!="TEA01","VAT shared ISO metadata overlaps VRS");
            }
        }
    }
    const auto anchor=vat_read_sector(in,256,sectors);
    vat_tag(anchor,2,256,512);
    const auto main_start=u32(anchor,20),main_bytes=u32(anchor,16);
    const auto reserve_start=u32(anchor,28),reserve_bytes=u32(anchor,24);
    auto main=vat_vds(in,sectors,main_start,main_bytes);
    const auto reserve=vat_vds(in,sectors,reserve_start,reserve_bytes);
    vat_need(std::equal(main.pd.begin()+16,main.pd.end(),reserve.pd.begin()+16) &&
             std::equal(main.lvd.begin()+16,main.lvd.end(),reserve.lvd.begin()+16),
             "main and reserve partition/volume descriptors disagree");
    const auto& pd=main.pd;
    const auto& lvd=main.lvd;
    const auto partition=u32(pd,188),capacity=u32(pd,192);
    const auto part_number=u16(pd,22);
    const std::string_view contents(reinterpret_cast<const char*>(pd.data()+25),6);
    const auto revision=u16(lvd,240);
    const bool legacy=revision==0x0150;
    vat_need(contents==(legacy?"+NSR02":"+NSR03") && u32(pd,184)==2 &&
             partition>256 && partition<sectors &&
             capacity<=0xffffffffu-partition &&
             sectors-partition+2<=capacity,
             "requires UDF 1.50/2.x write-once partition with unrecorded capacity");
    vat_need(std::all_of(pd.begin()+56,pd.begin()+96,[](auto c){return c==0;}),
             "write-once partition must not advertise free-space structures");
    const Location main_range{std::uint64_t(main_start)*block,main_bytes};
    const Location reserve_range{std::uint64_t(reserve_start)*block,reserve_bytes};
    vat_need(main_range.at+main_range.bytes<=std::uint64_t(partition)*block &&
             reserve_range.at+reserve_range.bytes<=std::uint64_t(partition)*block &&
             !intersects(main_range,reserve_range),
             "volume descriptor sequences overlap the backing partition");
    vat_need(u32(lvd,212)==block && u32(lvd,264)==70 &&
             u32(lvd,268)==2 && lvd[440]==1 && lvd[441]==6 &&
             u16(lvd,442)==1 && u16(lvd,444)==part_number &&
             lvd[446]==2 && lvd[447]==64 &&
             std::string_view(reinterpret_cast<const char*>(lvd.data()+451),22)==
                 "*UDF Virtual Partition" &&
             u16(lvd,482)==1 && u16(lvd,484)==part_number,
             "requires exactly one physical map and its virtual map");
    vat_need(std::string_view(reinterpret_cast<const char*>(lvd.data()+217),19)==
                 "*OSTA UDF Compliant" &&
             (legacy || (revision>=0x0200 && revision<=0x0260)) &&
             lvd[242]==0,"unsupported or write-protected UDF domain");
    if(legacy) {
        vat_need(u16(lvd,2)==2 && u16(pd,2)==2 &&
                 u16(lvd,474)==0x0150 && lvd[450]==0,
                 "UDF 1.50 virtual partition revision mismatch");
    }
    // Open integrity plus a valid VAT at the final recorded sector is
    // the clean sequential state. Never recover an old VAT for writing.
    const auto integrity_start=u32(lvd,436),integrity_bytes=u32(lvd,432);
    const Location integrity_range{std::uint64_t(integrity_start)*block,integrity_bytes};
    vat_need(integrity_bytes>=block && integrity_bytes%block==0 &&
             integrity_bytes<=16u*1024u*1024u && integrity_start>=257 &&
             integrity_range.at+integrity_range.bytes<=std::uint64_t(partition)*block &&
             !intersects(integrity_range,main_range) &&
             !intersects(integrity_range,reserve_range),
             "unsupported integrity sequence");
    const auto lvid=vat_read_sector(in,integrity_start,sectors);
    const auto implementation=u32(lvid,76);
    vat_need(u32(lvid,72)==2 && implementation<=block-96 &&
             u32(lvid,28)==0 && u32(lvid,32)==0 && u32(lvid,36)==0,
             "requires open unchained two-map logical volume integrity");
    vat_tag(lvid,9,integrity_start,96u+implementation);
    for(std::uint32_t j=1;j<integrity_bytes/block;++j) {
        const auto tail=vat_read_sector(in,std::uint64_t(integrity_start)+j,sectors);
        if(std::all_of(tail.begin(),tail.end(),[](auto c){return c==0;}))continue;
        vat_need(j==1,"unsupported integrity sequence continuation");
        vat_tag(tail,8,integrity_start+j,512);
    }
    const auto old_vat=static_cast<std::uint32_t>(sectors-partition-1);
    auto vat_fe=vat_read_sector(in,sectors-1,sectors);
    const auto vat_layout=vat_physical_file(vat_fe,old_vat,
                                            legacy?0:248,partition,old_vat);
    auto vat=vat_table_bytes(in,vat_layout);
    Bytes legacy_trailer;
    std::uint32_t previous=0xffffffffu;
    std::size_t header=0;
    if(legacy) {
        // UDF 1.50 section 2.2.10: entries followed by EntityID
        // (32 bytes) and PreviousVAT ICB (4 bytes), not a 2.x header.
        // Exclude this trailer during mapping allocation so newly
        // allocated virtual addresses can never overwrite the EntityID.
        vat_need(vat.size()>=40 && (vat.size()-36)%4==0 &&
                 u16(vat_fe,2)==2,
                 "invalid UDF 1.50 VAT size or descriptor version");
        const auto at=vat.size()-36;
        vat_need(vat[at]==0 && vat[at+23]==0 &&
                 std::string_view(reinterpret_cast<const char*>(vat.data()+at+1),22)==
                     "*UDF Virtual Alloc Tbl" &&
                 u16(vat,at+24)==0x0150 &&
                 std::all_of(vat.begin()+at+28,vat.begin()+at+32,
                             [](auto c){return c==0;}),
                 "invalid UDF 1.50 VAT EntityID");
        previous=u32(vat,at+32);
        legacy_trailer.assign(vat.begin()+at,vat.end());
        vat.resize(at);
    } else {
        vat_need(vat.size()>=156 && u16(vat,0)==152u+u16(vat,2) &&
                 u16(vat,0)<=vat.size()-4 && (vat.size()-u16(vat,0))%4==0 &&
                 std::equal(vat.begin()+4,vat.begin()+132,lvd.begin()+84) &&
                 u16(vat,144)>=0x0200 && u16(vat,144)<=0x0260 &&
                 u16(vat,146)>=0x0200 && u16(vat,146)<=0x0260 &&
                 u16(vat,148)>=u16(vat,146) && u16(vat,148)<=0x0260 &&
                 u16(vat,150)==0,"invalid UDF 2.x VAT header");
        previous=u32(vat,132);
        header=u16(vat,0);
    }
    const auto table_limit=vat_max_table-legacy_trailer.size();
    vat_need(previous==0xffffffffu || previous<old_vat,
             "invalid previous VAT ICB location");
    std::vector<std::uint32_t> mapping;
    std::set<std::uint32_t> used;
    for(std::size_t pos=header;pos<vat.size();pos+=4) {
        const auto physical=u32(vat,pos);
        vat_need(physical==0xffffffffu ||
                 (physical<old_vat && physical!=previous &&
                  used.insert(physical).second),
                 "VAT mapping is outside recorded data or aliases another block");
        mapping.push_back(physical);
    }
    std::map<std::uint32_t,std::uint64_t> allocated;
    vat_reserve_physical(vat_layout,partition,used,previous,allocated);
    auto read_virtual=[&](std::uint32_t logical) {
        vat_need(logical<mapping.size() && mapping[logical]!=0xffffffffu,
                 "unmapped virtual descriptor");
        return vat_read_sector(in,std::uint64_t(partition)+mapping[logical],sectors);
    };
    vat_need(u32(lvd,248)>=512 && u32(lvd,248)<=block &&
             u16(lvd,256)==1,"unsupported virtual file-set sequence");
    const auto fsd_lba=u32(lvd,252);
    const auto fsd=read_virtual(fsd_lba);
    vat_tag(fsd,256,fsd_lba,512);
    vat_need(u32(fsd,400)==block && u16(fsd,408)==1 &&
             u32(fsd,448)==0 && u32(fsd,464)==0 &&
             fsd[442]==0,"unsupported virtual root or system-stream geometry");
    const auto root_lba=u32(fsd,404);
    vat_need(root_lba!=fsd_lba,"virtual root aliases file set");
    // Each directory retains its virtual address. A copy-on-write FE
    // remap therefore changes no ancestor reference. Traverse the whole
    // tree before planning any mutation, rejecting aliasing and cycles.
    struct Directory {
        std::uint32_t lba{};
        Bytes fe;
        Bytes body; // dense FIDs, independent of the FE allocation mode
        std::vector<std::uint32_t> old_data;
        std::size_t children{};
    };
    struct Pending {
        std::string path;
        std::uint32_t lba{},parent{};
        std::uint64_t parent_uid{};
        unsigned depth{};
    };
    std::map<std::string,Directory> directories;
    std::vector<Pending> pending{{"",root_lba,root_lba,0,0}};
    std::set<std::uint32_t> visited{fsd_lba,root_lba};
    std::set<std::uint64_t> live_ids;
    std::set<std::string> folded_names;
    const auto vat_uid=u64(vat_fe,160);
    std::map<std::string,std::uint32_t> objects;
    std::size_t file_count=0;
    std::uint64_t directory_bytes_total=0;
    constexpr std::uint64_t max_directory_metadata=64u*1024u*1024u;
    // Reserve every virtual directory-data block in the same set as
    // FSD and FE addresses. The already checked VAT is injective, so
    // this also prevents physical aliasing between metadata objects.
    auto read_directory=[&](const Bytes& fe,std::uint32_t logical,
                            std::vector<std::uint32_t>& data_lbas,
                            VatFileLayout& layout) {
        layout.directory=true;
        const auto mode=u16(fe,34);
        if(mode==3) {
            vat_embedded_fe(fe,logical,4);
            layout.embedded.assign(fe.begin()+176,fe.begin()+176+u32(fe,172));
            return layout.embedded;
        }
        vat_need(u16(fe,0)==261 && fe[27]==4 && u16(fe,20)==4 &&
                 u16(fe,24)==1 && u16(fe,48)>=1 && u32(fe,168)==0 &&
                 (mode==0 || mode==1),
                 "unsupported external VAT directory FE");
        const auto size=u64(fe,56);
        const auto ads=u32(fe,172);
        const std::size_t step=mode==0?8u:16u;
        vat_need(size>=40 && size<=max_dir &&
                 ads && ads<=block-176 && ads%step==0,
                 "invalid external VAT directory allocation size");
        vat_tag(fe,261,logical,176u+ads);
        Bytes data;
        data.reserve(static_cast<std::size_t>(size));
        for(std::size_t at=176;at<176u+ads;at+=step) {
            const auto length=u32(fe,at);
            const auto first=u32(fe,at+4);
            const auto count=(std::uint64_t(length)+block-1)/block;
            vat_need(length && length<=0x3fffffffu && length<=size-data.size() &&
                     (length==size-data.size() || length%block==0) &&
                     first<mapping.size() && count<=mapping.size()-first,
                     "external VAT directory extent outside virtual space");
            if(mode==1)
                vat_need(u16(fe,at+8)==1 &&
                         std::all_of(fe.begin()+at+10,fe.begin()+at+16,
                                     [](auto c){return c==0;}),
                         "directory long_ad must reference virtual partition");
            std::uint64_t remaining=length;
            for(std::uint64_t j=0;j<count;++j) {
                edit_tick(progress);
                const auto lba=static_cast<std::uint32_t>(first+j);
                vat_need(visited.insert(lba).second && visited.size()<=max_items,
                         "VAT directory data aliases another live object");
                const auto sector=read_virtual(lba);
                const auto n=std::min<std::uint64_t>(remaining,block);
                data.insert(data.end(),sector.begin(),sector.begin()+static_cast<std::ptrdiff_t>(n));
                data_lbas.push_back(lba);
                layout.extents.push_back({(std::uint64_t(partition)+mapping[lba])*block,n,false});
                remaining-=n;
            }
        }
        vat_need(data.size()==size && data_lbas.size()==u64(fe,64),
                 "external VAT directory recorded block count mismatch");
        return data;
    };
    for(std::size_t cursor=0;cursor<pending.size();++cursor) {
        edit_tick(progress);
        const auto current=pending[cursor]; // vector may grow in this loop
        vat_need(current.depth<max_depth && pending.size()<=4096 &&
                 visited.size()<=max_items,
                 "VAT directory tree exceeds safe traversal limit");
        auto dir=read_virtual(current.lba);
        std::vector<std::uint32_t> data_lbas;
        VatFileLayout layout;
        const auto body=read_directory(dir,current.lba,data_lbas,layout);
        vat_need(body.size()<=max_directory_metadata-directory_bytes_total,
                 "VAT directory metadata limit exceeded");
        directory_bytes_total+=body.size();
        Bytes dense;
        dense.reserve(body.size());
        if(!current.path.empty())
            vat_need(plan->expected.emplace(current.path,std::move(layout)).second,
                     "duplicate VAT directory path");
        const auto uid=u64(dir,160);
        vat_need(!current.path.empty() || uid==0,
                 "VAT root UniqueID must be zero");
        const auto end=body.size();
        bool have_parent=false;
        std::size_t child_count=0,subdirectories=0;
        for(std::size_t pos=0;pos<end;) {
            // External directories may contain sector-end zero padding.
            if(!data_lbas.empty() && body[pos]==0 &&
               (end-pos<2 || body[pos+1]==0)) {
                const auto padding=std::min<std::size_t>(end-pos,block-pos%block);
                vat_need(std::all_of(body.begin()+pos,body.begin()+pos+padding,
                                     [](auto c){return c==0;}),
                         "invalid VAT directory sector padding");
                pos+=padding;continue;
            }
            vat_need(end-pos>=38,"truncated embedded directory");
            const auto n=body[pos+19];
            const auto implementation_bytes=u16(body,pos+36);
            const auto fid_size=(38u+std::size_t(implementation_bytes)+n+3u)&~std::size_t(3);
            vat_need(fid_size<=end-pos && implementation_bytes==0,
                     "unsupported FID length or implementation use");
            tag_check(body,pos,257);
            vat_need(u16(body,pos+2)>=2 && u16(body,pos+2)<=3 &&
                     u16(body,pos+10)==fid_size-16 &&
                     u32(body,pos+12)==(data_lbas.empty()?current.lba:data_lbas.at(pos/block)) &&
                     u16(body,pos+16)==1 && u32(body,pos+20)==block &&
                     u16(body,pos+28)==1,"invalid virtual directory reference");
            const auto child=u32(body,pos+24);
            const auto flags=body[pos+18];
            if(flags==10) {
                vat_need(dense.empty() && !have_parent && !n &&
                         child==current.parent &&
                         (legacy || u32(body,pos+32)==static_cast<std::uint32_t>(current.parent_uid)),
                         "invalid VAT parent directory FID");
                have_parent=true;
            } else {
                vat_need(have_parent && (flags==0 || flags==2) && n &&
                         visited.insert(child).second && visited.size()<=max_items,
                         "duplicate, cyclic or unsupported VAT directory reference");
                const auto filename=name(body,pos+38,n);
                vat_need(filename.find('/')==std::string::npos,
                         "VAT FID must identify a single path component");
                const auto key=current.path.empty()?filename:current.path+"/"+filename;
                vat_need(edit_path(key)==key && folded_names.insert(fold(key)).second,
                         "case-insensitive VAT name collision");
                auto fe=read_virtual(child);
                const auto unique=u64(fe,160);
                vat_need(unique>=16 && unique<vat_uid &&
                         live_ids.insert(unique).second &&
                         (legacy || u32(body,pos+32)==static_cast<std::uint32_t>(unique)),
                         "VAT object has invalid or duplicate unique ID");
                const bool is_directory=flags==2;
                if(is_directory) {
                    vat_need(fe[27]==4,"VAT directory FID refers to non-directory");
                    ++subdirectories;
                    vat_need(pending.size()<4096,"too many VAT directories");
                    pending.push_back({key,child,current.lba,uid,current.depth+1});
                } else {
                    auto layout=vat_physical_file(fe,child,5,partition,old_vat);
                    vat_reserve_physical(layout,partition,used,previous,allocated);
                    vat_need(plan->expected.emplace(key,std::move(layout)).second,
                             "duplicate VAT file path");
                    ++file_count;
                }
                ++child_count;
                vat_need(objects.emplace(key,child).second,
                         "duplicate VAT object path");
            }
            dense.insert(dense.end(),body.begin()+pos,body.begin()+pos+fid_size);
            pos+=fid_size;
        }
        vat_need(have_parent && subdirectories<65535 &&
                 u16(dir,48)==subdirectories+1,
                 "VAT directory parent reference or FileLinkCount mismatch");
        vat_need(directories.emplace(current.path,
                     Directory{current.lba,std::move(dir),std::move(dense),
                               std::move(data_lbas),child_count}).second,
                 "duplicate directory during VAT traversal");
    }
    // OSTA UDF 2.01 and later count the root directory itself in VAT
    // NumberOfDirectories. Earlier synthetic fixtures omitted this one.
    // Legacy VAT has no count fields. Limit its writer to images
    // already carrying a valid monotonic FE UniqueID counter; do not
    // infer a new counter from a potentially incomplete historical tree.
    vat_need(vat_uid>=16 && (legacy ||
             (u32(vat,136)==file_count && u32(vat,140)==directories.size())),
             "VAT unique ID or file/directory counts disagree with namespace");
    // Independent public reader must agree with every strict input entry.
    // No hidden special files, physical payload references or fallback
    // interpretation may silently become a writable namespace.
    {
        const Disc before(image);
        vat_need(before.format()=="UDF" && before.items().size()==plan->expected.size(),
                 "strict VAT and public reader namespaces disagree");
        for(const auto& item:before.items()) {
            const auto found=plan->expected.find(item.path);
            vat_need(found!=plan->expected.end() &&
                     vat_layout_matches(item,found->second),
                     "strict VAT input payload mismatch");
        }
    }
    // A WCX directory Delete denotes the entire selected subtree.
    // Resolve it from the validated namespace, never from arbitrary
    // wildcard matching. All explicit removals must exist even if a
    // selected ancestor would otherwise cover them. The lexical map
    // orders each exact parent before its descendants, allowing nested
    // selections to be coalesced without walking the same tree twice.
    std::map<std::string,const ArchiveChange*> selected_removals;
    for(const auto& request:requests) {
        if(!request.change->remove)continue;
        vat_need(objects.count(request.key)!=0,
                 "deletion target does not exist");
        selected_removals.emplace(request.key,request.change);
    }
    std::map<std::string,const ArchiveChange*> removals;
    for(const auto& [key,change]:selected_removals) {
        edit_tick(progress);
        if(!removals.emplace(key,change).second)continue;
        if(!directories.count(key))continue;
        const auto prefix=key+"/";
        for(auto it=objects.lower_bound(prefix);
            it!=objects.end() && it->first.starts_with(prefix);++it) {
            edit_tick(progress);
            removals.emplace(it->first,change);
        }
    }
    vat_need(removals.size()<=max_items,"VAT recursive removal exceeds namespace limit");
    if(coordinated && coordinated->iso_metadata_blocks) {
        vat_need(removals.size()==coordinated->vat_tree_deletions.size(),
                 "VAT and ISO expanded deletion counts differ");
        for(const auto& [key,change]:removals) {
            (void)change;
            vat_need(coordinated->vat_tree_deletions.count(key)!=0,
                     "VAT and ISO expanded deletion paths differ");
        }
    }
    std::vector<Request> expanded;
    expanded.reserve(requests.size()+removals.size());
    for(const auto& [key,change]:removals)
        expanded.push_back({key,change});
    for(auto& request:requests) {
        if(request.change->remove)continue;
        // Reject additions/replacements inside a removed subtree up
        // front, including targets that did not exist in the original.
        // ASCII case aliases are already rejected by namespace checks.
        auto ancestor=request.key;
        for(;;) {
            vat_need(!removals.count(ancestor),
                     "VAT cannot add or replace within a deleted subtree");
            const auto slash=ancestor.rfind('/');
            if(slash==std::string::npos)break;
            ancestor.resize(slash);
        }
        expanded.push_back(std::move(request));
    }
    requests=std::move(expanded);
    // Descendants precede removed parents; additions are parent-first.
    std::stable_sort(requests.begin(),requests.end(),[&](const Request& a,const Request& b) {
        if(a.change->remove!=b.change->remove)return a.change->remove;
        return a.change->remove?depth(a.key)>depth(b.key):depth(a.key)<depth(b.key);
    });
    // Create a new virtual address rather than reusing an address from
    // a deleted object. The prior transaction remains independently
    // readable. VAT ICB UniqueID is the sequential volume's ID counter.
    auto next_uid=[](std::uint64_t id) {
        vat_need(id<std::numeric_limits<std::uint64_t>::max(),
                 "VAT UniqueID exhausted");
        ++id;
        if(static_cast<std::uint32_t>(id)<16u) {
            vat_need(id<=std::numeric_limits<std::uint64_t>::max()-16u,
                     "VAT UniqueID exhausted");
            id+=16u-static_cast<std::uint32_t>(id);
        }
        return id;
    };
    auto new_uid=next_uid(vat_uid);
    std::set<std::string> dirty_directories;
    std::vector<std::pair<std::uint32_t,Bytes>> file_entries;
    file_entries.reserve(requests.size());
    const auto next_physical=[&]() {
        return std::uint64_t(old_vat)+1+plan->payload_blocks;
    };
    const auto available=std::uint64_t(capacity)-(sectors-partition);
    for(const auto& request:requests) {
        edit_tick(progress);
        const auto& key=request.key;
        const auto& change=*request.change;
        const bool making_directory=change.directory && !change.remove;
        const auto slash=key.rfind('/');
        const auto parent_key=slash==std::string::npos?std::string{}:key.substr(0,slash);
        const auto component=slash==std::string::npos?key:key.substr(slash+1);
        const auto parent_it=directories.find(parent_key);
        vat_need(parent_it!=directories.end(),"VAT parent directory does not exist");
        auto& parent=parent_it->second;
        auto& parent_fe=parent.fe;
        const auto parent_lba=parent.lba;
        auto& parent_body=parent.body;
        const auto end=parent_body.size();
        const auto found=objects.find(key);
        const bool creating=found==objects.end();
        vat_need(!change.remove || !creating,"deletion target does not exist");
        const bool target_directory=!creating && plan->expected.at(key).directory;
        vat_need(change.remove || creating || (!target_directory && !making_directory),
                 "VAT cannot replace a directory or create an existing directory");
        std::uint32_t target_lba=creating?0:found->second;
        if(creating)
            vat_need(!folded_names.count(fold(key)),
                     "new VAT filename collides case-insensitively");
        if(coordinated) {
            const bool authorized_remove=coordinated->iso_metadata_blocks?
                coordinated->vat_tree_deletions.count(key)!=0:
                (!target_directory && coordinated->shared_deletions.count(key));
            vat_need(change.remove?
                     (!creating && authorized_remove):
                     (creating?(making_directory?coordinated->shared_directories.count(key):
                                                 coordinated->shared_additions.count(key)):
                               coordinated->shared_replacements.count(key)),
                     "VAT coordinated target state differs from ISO plan");
        }
        if(change.remove) {
            if(target_directory)
                vat_need(directories.at(key).children==0,
                         "VAT recursive deletion did not cover every descendant");
            vat_need(parent.children>0,"VAT parent child count underflow");
            const bool retire_parent=removals.count(parent_key)!=0;
            if(!retire_parent) {
                // Resolve the current FID offset: earlier operations
                // may have compacted the surviving parent's body.
                std::size_t pos=0;
                std::size_t fid_bytes=0;
                for(;pos<end;pos+=fid_bytes) {
                    vat_need(end-pos>=38,"truncated staged VAT directory");
                    fid_bytes=(38u+std::size_t(u16(parent_body,pos+36))+parent_body[pos+19]+3u)&~std::size_t(3);
                    vat_need(fid_bytes<=end-pos,"invalid staged VAT FID");
                    if(!(parent_body[pos+18]&8u) && u32(parent_body,pos+24)==target_lba)break;
                }
                vat_need(pos<end && fid_bytes<=end-pos,
                         "VAT deletion FID missing from surviving parent");
                parent_body.erase(parent_body.begin()+pos,parent_body.begin()+pos+fid_bytes);
                if(target_directory) {
                    vat_need(u16(parent_fe,48)>1,"VAT parent directory link count underflow");
                    put16(parent_fe,48,static_cast<std::uint16_t>(u16(parent_fe,48)-1));
                }
                dirty_directories.insert(parent_key);
            }
            // Internal FIDs in a doomed directory need no rewrite.
            // The full original tree was checked above; retire all its
            // FE/data mappings without modifying those physical bytes.
            if(target_directory) {
                dirty_directories.erase(key);
                for(const auto logical:directories.at(key).old_data)
                    put32(vat,header+std::size_t(logical)*4,0xffffffffu);
                directories.erase(key);
            }
            --parent.children;
            put32(vat,header+std::size_t(target_lba)*4,0xffffffffu);
            plan->expected.erase(key);
            folded_names.erase(fold(key));
            objects.erase(found);
            continue;
        }
        Bytes target;
        if(creating) {
            vat_need(vat.size()+4<=table_limit &&
                     (vat.size()-header)/4<0xffffffffu &&
                     objects.size()<max_items,
                     "VAT virtual address or namespace limit exhausted");
            target_lba=static_cast<std::uint32_t>((vat.size()-header)/4);
            vat.resize(vat.size()+4,0xff);
            const auto encoded=encode_udf_cs0(component);
            const auto fid_bytes=(38u+encoded.size()+3u)&~std::size_t(3);
            vat_need(end<=max_dir && fid_bytes<=max_dir-end &&
                     fid_bytes<=max_directory_metadata-directory_bytes_total,
                     "VAT directory metadata limit exceeded");
            directory_bytes_total+=fid_bytes;
            Bytes fid(fid_bytes,0);
            put16(fid,0,257);put16(fid,2,u16(parent_fe,2));
            put16(fid,6,u16(parent_fe,6));put32(fid,12,parent_lba);
            put16(fid,16,1);fid[18]=making_directory?2u:0u;
            fid[19]=static_cast<std::uint8_t>(encoded.size());
            put32(fid,20,static_cast<std::uint32_t>(block));
            put32(fid,24,target_lba);put16(fid,28,1);
            // The FID long_ad implementation bytes are not the
            // mandatory UDF UniqueID field introduced in UDF 2.x.
            // Preserve old legacy FIDs; new 1.50 FIDs leave them zero.
            if(!legacy)put32(fid,32,static_cast<std::uint32_t>(new_uid));
            std::copy(encoded.begin(),encoded.end(),fid.begin()+38);
            tag_update(fid,0,fid.size());
            parent_body.insert(parent_body.end(),fid.begin(),fid.end());
            if(making_directory) {
                vat_need(u16(parent_fe,48)<65535 && directories.size()<4096,
                         "VAT directory link count or directory limit exceeded");
                put16(parent_fe,48,static_cast<std::uint16_t>(u16(parent_fe,48)+1));
            }
            ++parent.children;
            dirty_directories.insert(parent_key);
            target.assign(block,0);
            put16(target,0,261);put16(target,2,u16(parent_fe,2));
            put16(target,6,u16(parent_fe,6));put32(target,12,target_lba);
            put16(target,20,4);put16(target,24,1);target[27]=making_directory?4u:5u;
            put32(target,28,parent_lba);put16(target,32,1);
            put32(target,36,0xffffffffu);put32(target,40,0xffffffffu);
            put32(target,44,making_directory?0x000001ffu:0x000001b6u);
            put16(target,48,1);
            std::copy(parent_fe.begin()+72,parent_fe.begin()+108,target.begin()+72);
            put32(target,108,1);
            std::copy(parent_fe.begin()+128,parent_fe.begin()+160,target.begin()+128);
            put64(target,160,new_uid);
            new_uid=next_uid(new_uid);
            vat_need(objects.emplace(key,target_lba).second &&
                     folded_names.insert(fold(key)).second,
                     "duplicate newly created VAT path");
            if(making_directory) {
                Bytes parent_fid(40,0);
                put16(parent_fid,0,257);put16(parent_fid,2,u16(target,2));
                put16(parent_fid,6,u16(target,6));put32(parent_fid,12,target_lba);
                put16(parent_fid,16,1);parent_fid[18]=10;
                put32(parent_fid,20,static_cast<std::uint32_t>(block));
                put32(parent_fid,24,parent_lba);put16(parent_fid,28,1);
                if(!legacy)put32(parent_fid,32,static_cast<std::uint32_t>(u64(parent_fe,160)));
                tag_update(parent_fid,0,parent_fid.size());
                put16(target,34,3);put64(target,56,parent_fid.size());
                put32(target,172,static_cast<std::uint32_t>(parent_fid.size()));
                std::copy(parent_fid.begin(),parent_fid.end(),target.begin()+176);
                tag_update(target,0,176+parent_fid.size());
                VatFileLayout layout;
                layout.directory=true;layout.embedded=parent_fid;
                plan->expected.emplace(key,std::move(layout));
                vat_need(parent_fid.size()<=max_directory_metadata-directory_bytes_total,
                         "VAT directory metadata limit exceeded");
                directory_bytes_total+=parent_fid.size();
                vat_need(directories.emplace(key,Directory{target_lba,std::move(target),
                                                          std::move(parent_fid),{},0}).second,
                         "duplicate new VAT directory");
                dirty_directories.insert(key);
                continue;
            }
        } else {
            // Duplicate paths in one batch are rejected, so a replacement
            // can only refer to a regular FE from the original map.
            target=read_virtual(target_lba);
        }
        vat_need(std::filesystem::is_regular_file(
                 std::filesystem::symlink_status(change.source)),
                 "VAT file source must be a regular non-symlink file");
        const auto size=std::filesystem::file_size(change.source);
        vat_need(size<=vat_max_ad*((block-176)/16),
                 "replacement exceeds FE long_ad capacity");
        const bool shared=coordinated && (coordinated->shared_replacements.count(key) ||
                                         coordinated->shared_additions.count(key));
        vat_need(!shared || (size && size<=0xffffffffull),
                 "VAT shared replacement requires a nonempty ISO-compatible file");
        // Even small shared files must remain externally addressable
        // by ISO9660. Never embed a common payload in its new UDF FE.
        const bool embedded=!shared && size<=block-176;
        const auto payload_blocks=embedded?0:(size+block-1)/block;
        vat_need(plan->payload_blocks<=available &&
                 payload_blocks<=available-plan->payload_blocks,
                 "insufficient unrecorded capacity for VAT batch payload");
        VatFileLayout output;
        output.size=size;
        std::fill(target.begin()+176,target.end(),std::uint8_t{0});
        put64(target,56,size);
        if(embedded) {
            Bytes incoming(static_cast<std::size_t>(size));
            std::ifstream source(change.source,std::ios::binary);
            vat_need(bool(source),"cannot open replacement");
            if(!incoming.empty())source.read(reinterpret_cast<char*>(incoming.data()),
                                             static_cast<std::streamsize>(incoming.size()));
            vat_need(bool(source) && source.peek()==std::char_traits<char>::eof(),
                     "replacement source changed during read");
            put16(target,34,3);put64(target,64,0);
            put32(target,172,static_cast<std::uint32_t>(size));
            std::copy(incoming.begin(),incoming.end(),target.begin()+176);
            tag_update(target,0,176+incoming.size());
            output.embedded=std::move(incoming);
        } else {
            put16(target,34,1);put64(target,64,payload_blocks);
            std::uint64_t remaining=size,logical=next_physical();
            std::size_t at=176;
            while(remaining) {
                const auto n=std::min(remaining,vat_max_ad);
                vat_need(at<=block-16,"replacement long_ad capacity exceeded");
                put32(target,at,static_cast<std::uint32_t>(n));
                put32(target,at+4,static_cast<std::uint32_t>(logical));
                output.extents.push_back({(std::uint64_t(partition)+logical)*block,n,false});
                remaining-=n;logical+=(n+block-1)/block;at+=16;
            }
            vat_need(logical==next_physical()+payload_blocks,
                     "VAT payload allocation plan differs");
            put32(target,172,static_cast<std::uint32_t>(at-176));
            tag_update(target,0,at);
            plan->payloads.push_back({change.source,size,payload_blocks});
            plan->payload_blocks+=payload_blocks;
        }
        file_entries.emplace_back(target_lba,std::move(target));
        plan->expected.insert_or_assign(key,std::move(output));
        plan->verify_sources.emplace(key,change.source);
    }
    in.close();
    // Stream all external data once. Emit each changed regular FE and
    // each dirty directory exactly once, followed by ONE VAT ICB.
    std::uint64_t directory_blocks=0;
    // Retire old directory-data addresses and allocate fresh contiguous
    // virtual ranges. Pre-size VAT before computing its own allocation.
    for(const auto& key:dirty_directories) {
        auto& dir=directories.at(key);
        for(const auto logical:dir.old_data)
            put32(vat,header+std::size_t(logical)*4,0xffffffffu);
        dir.old_data.clear();
        if(dir.body.size()>block-176) {
            const auto count=(dir.body.size()+block-1)/block;
            vat_need(count<=(table_limit-vat.size())/4 &&
                     count<=max_items-directory_blocks,
                     "VAT directory virtual address limit exceeded");
            const auto first=(vat.size()-header)/4;
            for(std::size_t j=0;j<count;++j)
                dir.old_data.push_back(static_cast<std::uint32_t>(first+j));
            vat.resize(vat.size()+count*4,0xff);
            directory_blocks+=count;
        }
    }
    // The strict reader counts both directory-data blocks and FE/FSD
    // addresses against the live metadata traversal limit. Keep the
    // newly published namespace within the same limit.
    std::uint64_t live_metadata=objects.size()+2u; // root and FSD
    for(const auto& [key,dir]:directories) {
        (void)key;
        vat_need(live_metadata<=max_items &&
                 dir.old_data.size()<=max_items-live_metadata,
                 "VAT live metadata traversal limit exceeded");
        live_metadata+=dir.old_data.size();
    }
    // Include the legacy trailer in the final VAT allocation, without
    // appending it to the mutable mapping until all addresses are assigned.
    const auto final_vat_bytes=vat.size()+legacy_trailer.size();
    const auto vat_blocks=final_vat_bytes<=block-176?0:(final_vat_bytes+block-1)/block;
    const auto iso_blocks=coordinated?coordinated->iso_metadata_blocks:0u;
    const auto minimum=plan->payload_blocks+file_entries.size()+
                        dirty_directories.size()+directory_blocks+iso_blocks+vat_blocks+1u;
    const auto padding=mbr_alignment?
        (mbr_alignment-(sectors+minimum)%mbr_alignment)%mbr_alignment:0u;
    const auto required=minimum+padding;
    vat_need(required<=available && required<(max_image-plan->original_size)/block,
             "insufficient unrecorded partition capacity for VAT append/alignment");
    plan->final_size=plan->original_size+required*block;
    if(mbr_alignment) {
        std::ifstream mbr_source(image,std::ios::binary);
        vat_need(bool(mbr_source),"cannot reopen original VAT MBR");
        plan->mbr=vat_grown_mbr(mbr_source,plan->original_size,
                               plan->final_size,mbr_alignment);
    }
    auto physical=next_physical();
    for(auto& [logical,fe]:file_entries) {
        put32(vat,header+std::size_t(logical)*4,static_cast<std::uint32_t>(physical++));
        plan->append.push_back(std::move(fe));
    }
    for(const auto& key:dirty_directories) {
        auto& dir=directories.at(key);
        auto& body=dir.body;
        auto& fe=dir.fe;
        VatFileLayout layout;
        layout.directory=true;
        // TagLocation is the virtual block holding the start of each FID.
        // Packed descriptors may cross block boundaries. Their CRCs cover
        // the complete record, including required four-byte alignment.
        for(std::size_t pos=0;pos<body.size();) {
            vat_need(body.size()-pos>=38,"invalid planned VAT FID");
            const auto count=(38u+std::size_t(u16(body,pos+36))+body[pos+19]+3u)&~std::size_t(3);
            vat_need(count<=body.size()-pos,"planned VAT FID exceeds directory");
            put32(body,pos+12,dir.old_data.empty()?dir.lba:dir.old_data.at(pos/block));
            tag_update(body,pos,count);
            pos+=count;
        }
        std::fill(fe.begin()+176,fe.end(),std::uint8_t{0});
        put64(fe,56,body.size());
        if(dir.old_data.empty()) {
            put16(fe,34,3);put64(fe,64,0);
            put32(fe,172,static_cast<std::uint32_t>(body.size()));
            std::copy(body.begin(),body.end(),fe.begin()+176);
            tag_update(fe,0,176+body.size());
            layout.embedded=std::move(body);
        } else {
            put16(fe,34,0);put64(fe,64,dir.old_data.size());
            put32(fe,172,8);put32(fe,176,static_cast<std::uint32_t>(body.size()));
            put32(fe,180,dir.old_data.front());
            tag_update(fe,0,184);
            std::size_t pos=0;
            for(const auto logical:dir.old_data) {
                const auto n=std::min<std::size_t>(block,body.size()-pos);
                put32(vat,header+std::size_t(logical)*4,static_cast<std::uint32_t>(physical));
                layout.extents.push_back({(std::uint64_t(partition)+physical)*block,n,false});
                ++physical;
                Bytes sector(block,0);
                std::copy_n(body.begin()+pos,n,sector.begin());
                plan->append.push_back(std::move(sector));
                pos+=n;
            }
            vat_need(pos==body.size(),"VAT directory allocation plan differs");
        }
        put32(vat,header+std::size_t(dir.lba)*4,static_cast<std::uint32_t>(physical++));
        if(!key.empty())plan->expected.at(key)=std::move(layout);
        plan->append.push_back(std::move(fe));
    }
    // Keep ISO metadata unmapped in the VAT. The final table/ICB follow
    // the private zero-filled reservation; historical bytes are untouched.
    if(iso_blocks) {
        plan->iso_reservation_at=(std::uint64_t(partition)+physical)*block;
        plan->iso_reservation_bytes=iso_blocks*block;
        plan->append.emplace_back(static_cast<std::size_t>(plan->iso_reservation_bytes),0);
        physical+=iso_blocks;
    }
    if(padding) {
        plan->append.emplace_back(static_cast<std::size_t>(padding*block),0);
        physical+=padding;
    }
    const auto new_table=physical;
    const auto new_vat=new_table+vat_blocks;
    if(legacy) {
        put32(legacy_trailer,32,old_vat);
        vat.insert(vat.end(),legacy_trailer.begin(),legacy_trailer.end());
    } else {
        put32(vat,132,old_vat);
        const auto directory_count=static_cast<std::size_t>(std::count_if(
            plan->expected.begin(),plan->expected.end(),
            [](const auto& item){return item.second.directory;}));
        put32(vat,136,static_cast<std::uint32_t>(plan->expected.size()-directory_count));
        put32(vat,140,static_cast<std::uint32_t>(directory_count+1));
    }
    vat_need(vat.size()==final_vat_bytes,"VAT serialization size mismatch");
    put64(vat_fe,160,new_uid);
    std::fill(vat_fe.begin()+176,vat_fe.end(),std::uint8_t{0});
    put64(vat_fe,56,vat.size());
    put32(vat_fe,12,static_cast<std::uint32_t>(new_vat));
    if(vat_blocks) {
        put16(vat_fe,34,0);put64(vat_fe,64,vat_blocks);
        put32(vat_fe,172,8);
        put32(vat_fe,176,static_cast<std::uint32_t>(vat.size()));
        put32(vat_fe,180,static_cast<std::uint32_t>(new_table));
        tag_update(vat_fe,0,184);
        vat.resize(static_cast<std::size_t>(vat_blocks*block),0);
        plan->append.push_back(std::move(vat));
    } else {
        put16(vat_fe,34,3);put64(vat_fe,64,0);
        put32(vat_fe,172,static_cast<std::uint32_t>(vat.size()));
        std::copy(vat.begin(),vat.end(),vat_fe.begin()+176);
        tag_update(vat_fe,0,176+vat.size());
    }
    plan->append.push_back(std::move(vat_fe));
    return plan;
}
void edit_vat_append(const std::filesystem::path& image,
        const std::vector<ArchiveChange>& changes,const EditProgress& progress,
        const EditStageValidator& validate_stage,
        const CoordinatedIsoUpdate* coordinated,std::uint64_t mbr_alignment) {
    // Pin source identity before reading any plan data.
    EditTransaction tx(image);
    const auto plan=prepare_vat_append(image,changes,progress,coordinated,mbr_alignment);
    tx.assert_source_unchanged();
    prepare_udf_stage(image,tx.output,plan->original_size,progress);
    {
        std::fstream out(tx.output,std::ios::binary|std::ios::in|std::ios::out);
        vat_need(bool(out),"cannot open private append stage");
        auto at=plan->original_size;
        for(const auto& payload:plan->payloads) {
            edit_tick(progress);
            std::ifstream source(payload.source,std::ios::binary);
            vat_need(bool(source) && std::filesystem::file_size(payload.source)==
                     payload.size,"replacement source changed before streaming");
            out.seekp(static_cast<std::streamoff>(at));
            vat_need(bool(out),"cannot seek append payload");
            copy(source,out,payload.size,progress);
            vat_need(source.peek()==std::char_traits<char>::eof(),
                     "replacement source grew during streaming");
            const auto padding=payload.blocks*block-payload.size;
            if(padding)staged_zero(out,at+payload.size,padding,progress);
            at+=payload.blocks*block;
        }
        vat_need(at==plan->original_size+plan->payload_blocks*block,
                 "VAT batch payload length differs from plan");
        const auto metadata_start=at;
        for(const auto& bytes:plan->append) {
            edit_tick(progress);
            staged_write(out,at,bytes);at+=bytes.size();
        }
        vat_need(at==plan->final_size,"VAT append write length differs from plan");
        out.flush();vat_need(bool(out),"cannot flush VAT append");
        at=metadata_start;
        for(const auto& bytes:plan->append) {
            edit_tick(progress);
            vat_need(staged_read(out,at,bytes.size())==bytes,
                     "appended VAT transaction failed byte readback");
            at+=bytes.size();
        }
    }
    vat_need(std::filesystem::file_size(tx.output)==plan->final_size,
             "unexpected VAT append size");
    // Before any ISO coordination callback, the complete sequential
    // stage must preserve the entire source prefix. Afterwards, only
    // prevalidated ISO metadata fields may differ from that prefix.
    if(coordinated) {
        OpticalEditGuard whole(plan->original_size);
        whole.protect(0,plan->original_size);
        whole.verify_appended(image,tx.output,progress);
    }
    if(!plan->mbr.empty()) {
        // All original bytes except the independently derived 512-byte
        // header must remain unchanged. Only CHS end/count differ in it.
        edit_tick(progress);
        std::fstream out(tx.output,std::ios::binary|std::ios::in|std::ios::out);
        vat_need(bool(out),"cannot open staged VAT MBR");
        staged_write(out,0,plan->mbr);
        vat_need(staged_read(out,0,512)==plan->mbr,"staged VAT MBR readback differs");
    }
    if(validate_stage)validate_stage(tx.output);
    vat_need(std::filesystem::file_size(tx.output)==plan->final_size,
             "VAT ISO coordination changed the append size");
    OpticalEditGuard guard(plan->original_size);
    if(coordinated) {
        std::uint64_t at=0;
        for(const auto& [start,length]:coordinated->metadata_writes) {
            if(start>at)guard.protect(at,start-at);
            at=start+length;
        }
        guard.protect(at,plan->original_size-at);
        // The callback owns only original ISO metadata fields and the
        // explicitly reserved new ISO tree. Recheck every VAT run.
        std::fstream in(tx.output,std::ios::binary|std::ios::in);
        vat_need(bool(in),"cannot reopen coordinated VAT stage");
        auto pos=plan->original_size+plan->payload_blocks*block;
        for(const auto& bytes:plan->append) {
            edit_tick(progress);
            if(plan->iso_reservation_bytes && pos==plan->iso_reservation_at)
                vat_need(bytes.size()==plan->iso_reservation_bytes,
                         "VAT ISO reservation differs from planned range");
            else vat_need(staged_read(in,pos,bytes.size())==bytes,
                          "ISO coordination modified appended VAT metadata");
            pos+=bytes.size();
        }
    } else if(!plan->mbr.empty()) {
        std::fstream in(tx.output,std::ios::binary|std::ios::in);
        vat_need(bool(in) && staged_read(in,0,512)==plan->mbr,
                 "staged ISO validation changed VAT MBR");
        guard.protect(512,plan->original_size-512);
        // Validate every appended metadata/padding byte after the
        // bridge callback as well as the independently parsed payload.
        auto pos=plan->original_size+plan->payload_blocks*block;
        for(const auto& bytes:plan->append) {
            edit_tick(progress);
            vat_need(staged_read(in,pos,bytes.size())==bytes,
                     "MBR VAT append readback changed");
            pos+=bytes.size();
        }
    } else guard.protect(0,plan->original_size);
    guard.verify_appended(image,tx.output,progress);
    {
        const Disc checked(tx.output);
        vat_need(checked.format()=="UDF" && checked.items().size()==plan->expected.size(),
                 "staged VAT namespace differs");
        std::set<std::string> seen;
        for(const auto& item:checked.items()) {
            edit_tick(progress);
            const auto found=plan->expected.find(item.path);
            vat_need(found!=plan->expected.end() && seen.insert(item.path).second &&
                     vat_layout_matches(item,found->second),
                     "staged VAT payload differs");
            const auto source=plan->verify_sources.find(item.path);
            if(source!=plan->verify_sources.end())
                vat_need(same_staged_item(tx.output,item,source->second,progress),
                         "replacement changed during VAT transaction");
        }
    }
    tx.commit(progress);
}
} // namespace

void edit_udf(const std::filesystem::path& image,
              const std::vector<ArchiveChange>& changes,
              const EditProgress& progress,bool allow_bridge,
              const EditStageValidator& validate_stage,
              const EditProtectedRanges& protected_ranges,
              const CoordinatedIsoUpdate* coordinated,
              bool bounded_partitioned_bridge,
              std::uint64_t mbr_append_alignment_blocks,
              std::uint64_t gpt_append_trailer_blocks) {
    edit_tick(progress);
    need((!mbr_append_alignment_blocks && !gpt_append_trailer_blocks) ||
         (bounded_partitioned_bridge &&
          !(mbr_append_alignment_blocks && gpt_append_trailer_blocks)),
         "hybrid UDF mastering needs one validated partition geometry");
    need(!bounded_partitioned_bridge ||
         (allow_bridge && validate_stage &&
          (!coordinated || mbr_append_alignment_blocks || gpt_append_trailer_blocks)),
         "partitioned ISO/UDF coordinated edits need validated ROM growth");
    need(!allow_bridge || bool(validate_stage),
         "ISO/UDF Bridge writes require a staged ISO/boot validator");
    // Run the strict Type-1 write preflight before the permissive read-only
    // Disc parser. This preserves the precise FSD mapping failure reason
    // even when Disc's general UDF reader cannot resolve that map.
    std::unique_ptr<Type1> owner;
    try {
        owner=std::make_unique<Type1>(image,allow_bridge,bounded_partitioned_bridge);
    } catch(const VatWriteRequired&) {
        // The VAT bridge provides the external ISO validator and
        // immutable ranges. Shared-file coordination may only change
        // the explicitly checked pre-anchor ISO metadata fields.
        if(gpt_append_trailer_blocks ||
           (bounded_partitioned_bridge && (!mbr_append_alignment_blocks || coordinated)) ||
           (allow_bridge && (!validate_stage || protected_ranges.empty())) ||
           (!allow_bridge && (validate_stage || !protected_ranges.empty() || coordinated)))throw;
        edit_vat_append(image,changes,progress,validate_stage,coordinated,
                        mbr_append_alignment_blocks);
        return;
    }
    // A mirrored sequential deletion contract cannot be interpreted
    // by the physical Type-1 allocator if the mapping family changed.
    need(!coordinated || coordinated->vat_tree_deletions.empty(),
         "VAT-only recursive coordination plan cannot use Type-1");
    auto& volume=*owner;
    if(bounded_partitioned_bridge) {
        const bool rom=(mbr_append_alignment_blocks || gpt_append_trailer_blocks) &&
            volume.partition_access_type()==1 && !volume.has_allocation_map();
        need(rom || (volume.has_allocation_map() &&
             (volume.partition_access_type()==3 ||
              volume.partition_access_type()==4)),
             "partitioned ISO/UDF requires a rewritable allocator or validated ROM mastering");
        if(rom) {
            need(gpt_append_trailer_blocks<256 &&
                 gpt_append_trailer_blocks<=volume.image_size()/block,
                 "invalid GPT/UDF trailer geometry");
            for(const auto& object:volume.intervals()) {
                need(!intersects({0,gpt_append_trailer_blocks?1024u:512u},object.range),
                     "hybrid growth header aliases live UDF data");
                if(gpt_append_trailer_blocks)
                    need(!intersects({volume.image_size()-gpt_append_trailer_blocks*block,
                                      gpt_append_trailer_blocks*block},object.range),
                         "GPT backup trailer aliases live UDF data");
            }
        }
    }
    const Disc before(image);
    need(before.format()=="UDF","not a plain readable UDF volume");
    need(!allow_bridge || !protected_ranges.empty(),
         "ISO/UDF Bridge metadata writes need protected ISO ranges");
    volume.protect_external_ranges(protected_ranges);
    need(!changes.empty(),"no UDF changes");
    if(coordinated) {
        need(allow_bridge && validate_stage &&
             volume.partition_access_type()==1 && !volume.has_allocation_map() &&
             coordinated->shared_replacements.size()+
                 coordinated->shared_deletions.size()+
                 coordinated->shared_additions.size()+
                 coordinated->shared_directories.size()==changes.size() &&
             !coordinated->metadata_writes.empty(),
             "shared ISO/UDF edit requires read-only Type-1 mastering");
        for(const auto& change:changes) {
            const auto key=edit_path(change.path);
            const bool existing=volume.files().count(key)!=0;
            need(existing?
                    (!change.directory &&
                     (change.remove?coordinated->shared_deletions.count(key):
                                    coordinated->shared_replacements.count(key))):
                    (!change.remove && !volume.directories().count(key) &&
                     (change.directory?coordinated->shared_directories.count(key):
                                       coordinated->shared_additions.count(key))),
                 "invalid coordinated edit target");
        }
        for(const auto& [at,bytes]:coordinated->metadata_writes) {
            need(bytes && at<=volume.image_size() &&
                 bytes<=volume.image_size()-at,
                 "coordinated ISO field outside source image");
            for(const auto& object:volume.intervals())
                need(!intersects({at,bytes},object.range),
                     "coordinated ISO field overlaps a live UDF object");
        }
        auto plan=volume.prepare_readonly_append(changes,protected_ranges,
                                                coordinated->iso_metadata_blocks,
                                                mbr_append_alignment_blocks,
                                                gpt_append_trailer_blocks);
        create_udf_type1_batch(image,volume,changes,progress,true,
                               validate_stage,&plan,coordinated);
        return;
    }
    if(volume.partition_access_type()==1 &&
       !volume.has_allocation_map() &&
       std::any_of(changes.begin(),changes.end(),[&](const ArchiveChange& c) {
           const auto key=edit_path(c.path);
           if(c.remove)return volume.directory_links().count(key)!=0;
           const auto it=volume.files().find(key);
           if(it!=volume.files().end() && !c.directory &&
              std::filesystem::is_regular_file(std::filesystem::symlink_status(c.source)))
               return std::filesystem::file_size(c.source)>
                      replacement_capacity(it->second.fe);
           return !volume.files().count(key) && !volume.directories().count(key);
       })) {
        auto plan=volume.prepare_readonly_append(changes,protected_ranges,0,
                                                mbr_append_alignment_blocks,
                                                gpt_append_trailer_blocks);
        create_udf_type1_batch(image,volume,changes,progress,allow_bridge,
                               validate_stage,&plan);
        return;
    }
    for(const auto& change:changes)
        if(change.remove && volume.directory_links().count(edit_path(change.path)))
            need(volume.has_allocation_map(),
                 "UDF directory deletion requires valid allocation map and LVID");
    // A mixed creation/replacement/deletion batch shares one allocator and
    // one atomic image commit. Existing-only edits retain their original
    // bounded, non-allocating transaction path.
    if(volume.has_allocation_map() && std::any_of(changes.begin(),changes.end(),
        [&](const ArchiveChange& change) {
            const auto key=edit_path(change.path);
            return change.remove ||
                   (!volume.files().count(key) &&
                    !volume.directories().count(key));
        })) {
        create_udf_type1_batch(image,volume,changes,progress,
                               allow_bridge,validate_stage);
        return;
    }
    if(changes.size()==1 && !changes[0].remove) {
        const auto path=edit_path(changes[0].path);
        if(!volume.files().count(path) &&
           !volume.directories().count(path)) {
            create_udf_type1(image,volume,changes[0],progress,
                             allow_bridge,validate_stage);
            return;
        }
    }
    std::set<std::string> distinct;
    for(const auto& change:changes) {
        const auto name=edit_path(change.path);
        need(distinct.insert(name).second,"duplicate UDF change path");
        validate_udf_existing_change(volume,change);
    }

    EditTransaction tx(image);
    prepare_udf_stage(image,tx.output,std::filesystem::file_size(image),progress);
    std::fstream out(tx.output,std::ios::binary|std::ios::in|
                     std::ios::out);
    need(bool(out),"cannot open prepared UDF transaction stage");
    volume.close();
    for(const auto& change:changes) {
        edit_tick(progress);
        const auto& target=volume.files().at(edit_path(change.path));
        if(change.remove) {
            auto fid=staged_read(out,target.fid_at,target.fid_bytes);
            tag_check(fid,0,257);
            need(!(fid[18]&0x0cu),"UDF directory entry is already deleted");
            fid[18]|=0x04; // UDF FID deleted characteristic.
            tag_update(fid,0,fid.size());
            if(target.parent_embedded) {
                // The FID is part of the parent's allocation descriptor
                // bytes and therefore covered by the FE/EFE CRC. Validate
                // the OLD descriptor before mutating its payload; writing
                // the FID separately first would invalidate that CRC.
                auto parent=staged_read(out,target.parent_at,block);
                const auto tag=u16(parent,0);
                need(tag==261 || tag==266,"invalid UDF embedded parent descriptor");
                tag_check(parent,0,tag);
                const auto base=tag==261?176u:216u;
                const auto ea=u32(parent,tag==261?168:208);
                const auto ad=u32(parent,tag==261?172:212);
                need((u16(parent,34)&7u)==3 && parent[27]==4 &&
                     ea<=block-base && ad<=block-base-ea &&
                     target.fid_at>=target.parent_at,
                     "invalid UDF embedded parent allocation bounds");
                const auto start=std::uint64_t(base)+ea;
                const auto offset=target.fid_at-target.parent_at;
                need(offset>=start && offset<=start+ad &&
                     fid.size()<=start+ad-offset,
                     "deleted UDF FID lies outside embedded parent payload");
                std::copy(fid.begin(),fid.end(),
                          parent.begin()+static_cast<std::ptrdiff_t>(offset));
                tag_update(parent,0,base+ea+ad);
                tag_check(parent,0,tag);
                staged_write(out,target.parent_at,parent);
            } else {
                staged_write(out,target.fid_at,fid);
            }
            continue;
        }
        write_udf_existing_replacement(out,target,change.source,progress);
    }
    out.flush();
    need(bool(out),"staged UDF flush failed");
    out.close();

    {
        const Disc checked(tx.output);
        need(checked.format()=="UDF","staged UDF metadata did not validate");
        std::map<std::string,std::size_t> by_path;
        for(std::size_t i=0;i<checked.items().size();++i)
            need(by_path.emplace(checked.items()[i].path,i).second,
                 "duplicate files in staged UDF namespace");
        for(std::size_t i=0;i<changes.size();++i) {
            edit_tick(progress);
            const auto path=edit_path(changes[i].path);
            const auto it=by_path.find(path);
            if(changes[i].remove) {
                need(it==by_path.end(),"deleted UDF file remains visible");
                continue;
            }
            need(it!=by_path.end() &&
                 checked.items()[it->second].size==
                     std::filesystem::file_size(changes[i].source),
                 "staged UDF file size mismatch");
            need(same_staged_item(tx.output,
                                  checked.items()[it->second],
                                  changes[i].source,progress),
                 "UDF staged file differs from replacement source");
        }
    }
    if(validate_stage)validate_stage(tx.output);
    tx.commit(progress);
}

// Reuse exactly the same strict parser used before a real write. The report
// deliberately does not claim that a standalone parse authorizes mutation
// of shared ISO/El Torito extents or guarantees allocation for a file.
UdfEditInspection inspect_udf_edit(
    const std::filesystem::path& image,bool allow_bridge,
    bool bounded_partitioned_bridge) {
    UdfEditInspection result;
    try {
        Type1 volume(image,allow_bridge,bounded_partitioned_bridge);
        result.parser_compatible=true;
        result.allocation_bitmap=volume.has_bitmap();
        result.allocation_table=volume.has_unallocated_table();
        result.partition_access_type=volume.partition_access_type();
        if(!volume.has_allocation_map())
            result.reason=volume.missing_allocation_map_reason();
    }catch(const std::exception& ex) {
        result.reason=ex.what();
        if(result.reason.size()>600)result.reason.resize(600);
    }
    return result;
}
} // namespace iso_wcx
