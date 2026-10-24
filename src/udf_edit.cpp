#include "iso_wcx/archive_edit.hpp"
#include "iso_wcx/disc.hpp"
#include "iso_wcx/edit_transaction.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <queue>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

class Type1 {
public:
    explicit Type1(const std::filesystem::path& path)
        :file_(path,std::ios::binary),size_(std::filesystem::file_size(path)) {
        need(file_.good() && size_%block==0 && size_>=300*block &&
             size_<max_image,"unsupported UDF image size/sector geometry");
        const auto system=read(0,16*block);
        need(std::all_of(system.begin(),system.end(),
                         [](std::uint8_t x){return x==0;}),
             "UDF system-area hybrid image is read-only");
        bool vrs=false;
        for(std::uint64_t i=16;i<256 && (i+1)*block<=size_;++i) {
            const auto b=read(i*block,8);
            if(std::equal(b.begin()+1,b.begin()+6,"CD001"))
                throw EditUnsupported("ISO/UDF Bridge metadata rewriting unsupported");
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
        need(!pd.empty() && !lvd.empty() && u32(lvd,212)==block &&
             u32(lvd,264)==6 && u32(lvd,268)==1 &&
             lvd[440]==1 && lvd[441]==6 &&
             u16(lvd,444)==u16(pd,22),
             "only single physical UDF Type-1 partition maps are writable");
        partition_=u32(pd,188);
        partition_blocks_=u32(pd,192);
        need(partition_blocks_>2 && partition_<size_/block &&
             partition_blocks_<=size_/block-partition_,
             "UDF partition bounds outside image");
        // Partition header: only a single unallocated-space bitmap can
        // authorize allocation. Space tables, freed-space sets or multiple
        // physical partition maps cannot be safely synthesized.
        const auto bitmap_length=u32(pd,64);
        const auto bitmap_logical=u32(pd,68);
        const auto bitmap_span=
            (std::uint64_t(bitmap_length)+block-1u)/block*block;
        const auto has_conflicting_allocator=
            u32(pd,56)!=0 || u32(pd,72)!=0 || u32(pd,80)!=0 ||
            u32(pd,88)!=0;
        if(bitmap_length) {
            need((u32(pd,184)==3 || u32(pd,184)==4),
                 "UDF partition is not declared rewritable/overwritable");
            need(!has_conflicting_allocator && bitmap_length>=24 &&
                 bitmap_span<=max_dir &&
                 bitmap_logical<partition_blocks_ &&
                 bitmap_span/block<=partition_blocks_-bitmap_logical,
                 "unsupported UDF free-space allocation layout");
            Bitmap bm;
            bm.logical=bitmap_logical;
            bm.at=physical(bitmap_logical);
            bm.bytes=read(bm.at,static_cast<std::size_t>(bitmap_span));
            tag_check(bm.bytes,0,264);
            need(u32(bm.bytes,12)==bitmap_logical,
                 "incorrect UDF bitmap descriptor location");
            bm.bits=u32(bm.bytes,16);
            bm.byte_count=u32(bm.bytes,20);
            // A UDF bitmap can span many sectors; the PD extent bounds
            // the buffer while numOfBits must match the partition length.
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
            // mkudffs can protect only the 8-byte bitmap header in its
            // tag CRC; changing its declared length would be incorrect.
            const auto tagged=16u+u16(bm.bytes,10);
            need(tagged>=24u && tagged<=24ull+bm.byte_count,
                 "UDF bitmap has unsupported descriptor CRC coverage");
            bitmap_=std::move(bm);
            reserved_.push_back({{bitmap_->at,bitmap_span},0});
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
        const auto fsd_size=u32(lvd,248)&0x3fffffffu;
        need(fsd_size>=416 && fsd_size<=block && u16(lvd,256)==0,
             "unsupported UDF file-set mapping");
        const auto fsd=read(physical(u32(lvd,252)),block);
        tag_check(fsd,0,256);
        reserved_.push_back({{256*block,block},0});
        reserved_.push_back({{std::uint64_t(start)*block,len},0});
        reserved_.push_back({{physical(u32(lvd,252)),block},0});
        walk(u32(fsd,404),u16(fsd,408),"",0);
        if(bitmap_) {
            std::uint64_t free_bits=0;
            for(std::uint32_t i=0;i<bitmap_->bits;++i)
                if(bitmap_free(*bitmap_,i))++free_bits;
            need(integrity_ &&
                 u32(integrity_->bytes,80)==free_bits,
                 "UDF bitmap and LVID free-block counters disagree");
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
                             "UDF free bitmap overlaps a referenced file or descriptor");
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
    [[nodiscard]] Bytes directory_payload(const Fe& directory) {
        return directory_data(directory);
    }
    [[nodiscard]] bool has_bitmap() const noexcept {return bitmap_.has_value();}
    [[nodiscard]] std::uint64_t image_size() const noexcept {return size_;}
    [[nodiscard]] std::uint32_t part_start() const noexcept {return partition_;}
    // A single ShortAD requires one contiguous allocation. Reserve a
    // validated run before writing any staged file content; bitmap changes
    // stay in memory until the entire image transaction is committed.
    [[nodiscard]] std::uint32_t allocate_blocks(std::uint32_t count) {
        need(bitmap_.has_value(),
             "UDF creation requires a valid Space Bitmap Descriptor");
        need(count>0 && count<=partition_blocks_,
             "UDF requested allocation exceeds partition");
        auto& bitmap=*bitmap_;
        for(std::uint32_t start=0;start<bitmap.bits;) {
            if(!bitmap_free(bitmap,start)){++start;continue;}
            std::uint32_t run=0;
            while(run<count && run<bitmap.bits-start &&
                  bitmap_free(bitmap,start+run))++run;
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
        throw EditUnsupported("UDF bitmap has no sufficiently long free run");
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
        for(std::uint32_t at=0;at<bitmap.bits;) {
            if(!bitmap_free(bitmap,at)) {++at;continue;}
            const auto start=at;
            do {++at;} while(at<bitmap.bits && bitmap_free(bitmap,at));
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
             "UDF file cannot fit in the free bitmap/ShortAD slots");
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
        need(bitmap_.has_value(),"UDF directory release requires bitmap");
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
    [[nodiscard]] std::pair<std::uint64_t,Bytes> updated_bitmap() const {
        need(bitmap_.has_value(),"UDF bitmap is absent");
        auto b=bitmap_->bytes;
        // Preserve original bitmap tag's CRC coverage. The bitmap's data
        // bit changes may lie outside that descriptor CRC by design.
        tag_update(b,0,16u+u16(b,10));
        return {bitmap_->at,std::move(b)};
    }
    // Commit the entire batch to one LVID update. IDs and both object
    // counters must reflect *all* creations; subtract only directory blocks
    // that have passed the allocator's unique-ownership release check.
    [[nodiscard]] std::pair<std::uint64_t,Bytes>
    updated_integrity_batch(std::uint32_t files,std::uint32_t dirs,
                            std::uint32_t allocated_blocks) const {
        need(integrity_.has_value(),"UDF LVID is absent");
        auto b=integrity_->bytes;
        const auto entries=std::uint64_t(files)+dirs;
        need(entries>0 && entries<=
                 std::numeric_limits<std::uint64_t>::max()-integrity_->next_id,
             "UDF batch unique-ID sequence overflow");
        const auto old_free=u32(b,80);
        need(allocated_blocks>0 && old_free>=allocated_blocks,
             "UDF LVID reports insufficient free blocks");
        put32(b,80,old_free-allocated_blocks);
        put64(b,40,integrity_->next_id+entries);
        if(integrity_->impl_bytes>=40) {
            for(const auto [counter,change] :
                {std::pair{120u,files},std::pair{124u,dirs}}) {
                const auto previous=u32(b,counter);
                need(change<=std::numeric_limits<std::uint32_t>::max()-previous,
                     "UDF LVID file/directory count overflow");
                put32(b,counter,previous+change);
            }
        }
        tag_update(b,0,88u+integrity_->impl_bytes);
        return {integrity_->at,std::move(b)};
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
    void close() {file_.close();}
private:
    std::ifstream file_;
    std::uint64_t size_{};
    std::uint32_t partition_{},partition_blocks_{};
    std::map<std::string,File> files_;
    std::map<std::string,Fe> directory_files_;
    std::optional<Bitmap> bitmap_;
    std::optional<Integrity> integrity_;
    std::map<std::uint64_t,std::size_t> references_;
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
                    reserved_.push_back({{child.at,block},child.at});
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
// Allocate one FE and optionally a contiguous ShortAD run for file data.
// Embedded or external parent directories may grow to a larger contiguous
// ShortAD run. FIDs are tightly packed (and may straddle sector boundaries)
// for compatibility with independent readers including 7-Zip.
void create_udf_type1(const std::filesystem::path& image,Type1& volume,
                      const ArchiveChange& change,
                      const EditProgress& progress) {
    const auto key=edit_path(change.path);
    need(volume.has_bitmap(),"UDF create requires a validated allocation bitmap");
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
    std::fstream out(tx.output,std::ios::binary|std::ios::in|
                     std::ios::out|std::ios::trunc);
    need(bool(out),"cannot open staged UDF allocation image");
    std::ifstream original(image,std::ios::binary);
    need(bool(original),"cannot read original UDF for allocation");
    copy_image_sparse(original,out,volume.image_size(),progress);
    original.close();
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
        Type1 strict(tx.output);
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
    tx.commit(progress);
}
} // namespace

void edit_udf(const std::filesystem::path& image,
              const std::vector<ArchiveChange>& changes,
              const EditProgress& progress) {
    edit_tick(progress);
    const Disc before(image);
    need(before.format()=="UDF","not a plain readable UDF volume");
    Type1 volume(image);
    need(!changes.empty(),"no UDF changes");
    // Allocation is a separate one-file atomic transaction until multi-file
    // directory/bitmap/LVID updates can be planned and verified together.
    if(changes.size()==1 && !changes[0].remove) {
        const auto path=edit_path(changes[0].path);
        if(!volume.files().count(path) &&
           !volume.directories().count(path)) {
            create_udf_type1(image,volume,changes[0],progress);
            return;
        }
    }
    std::set<std::string> distinct;
    for(const auto& change:changes) {
        const auto name=edit_path(change.path);
        need(distinct.insert(name).second,"duplicate UDF change path");
        need(!change.directory,"UDF directory creation/deletion not yet supported");
        const auto it=volume.files().find(name);
        need(it!=volume.files().end(),"UDF only supports existing files");
        const auto& target=it->second;
        need(target.fe.type==5,
             "UDF deletion or replacement of a directory is unsupported");
        if(!change.remove) {
            need(std::filesystem::is_regular_file(
                     std::filesystem::symlink_status(change.source)),
                 "UDF replacement must be a regular file, not symlink");
            const auto len=std::filesystem::file_size(change.source);
            // Only the last ShortAD's unused final sector can grow without
            // moving data or changing the order of existing logical runs.
            // Earlier runs may have padding but extending them would shift
            // every subsequent file offset. Keep the growth fully in place.
            std::uint64_t available=target.fe.capacity;
            if(!target.fe.runs.empty()) {
                const auto& last=target.fe.runs.back();
                const auto last_allocated=std::uint64_t(last.blocks)*block;
                need(last.recorded<=last_allocated,
                     "UDF final ShortAD exceeds its allocated blocks");
                const auto last_limit=std::min<std::uint64_t>(
                    last_allocated,0x3fffffffull);
                need(last.recorded<=last_limit,
                     "UDF final ShortAD exceeds 30-bit recorded length");
                available=target.fe.recorded+(last_limit-last.recorded);
            }
            need(len<=target.fe.capacity && len<=available,
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

    EditTransaction tx(image);
    std::fstream out(tx.output,std::ios::binary|std::ios::in|
                     std::ios::out|std::ios::trunc);
    need(bool(out),"cannot create staged UDF image");
    std::ifstream original(image,std::ios::binary);
    need(bool(original),"cannot open original UDF image");
    copy_image_sparse(original,out,std::filesystem::file_size(image),progress);
    original.close();
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
            staged_write(out,target.fid_at,fid);
            if(target.parent_embedded) {
                auto parent=staged_read(out,target.parent_at,block);
                const auto tag=u16(parent,0);
                tag_check(parent,0,tag);
                const auto base=tag==261?176u:216u;
                const auto ea=u32(parent,tag==261?168:208);
                const auto ad=u32(parent,tag==261?172:212);
                tag_update(parent,0,base+ea+ad);
                staged_write(out,target.parent_at,parent);
            }
            continue;
        }
        const auto len=std::filesystem::file_size(change.source);
        auto fe=staged_read(out,target.fe.at,block);
        tag_check(fe,0,u16(fe,0));
        if(target.fe.storage==3) {
            Bytes data(static_cast<std::size_t>(len));
            std::ifstream input(change.source,std::ios::binary);
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
            std::ifstream source(change.source,std::ios::binary);
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
    tx.commit(progress);
}
} // namespace iso_wcx
