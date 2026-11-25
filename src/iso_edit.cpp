#include "iso_wcx/archive_edit.hpp"
#include "iso_wcx/disc.hpp"
#include "iso_wcx/edit_transaction.hpp"
#include "iso_wcx/optical_edit_guard.hpp"
#include "iso_wcx/rock_ridge.hpp"
#include "iso_wcx/udf_edit_inspection.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>

namespace iso_wcx { namespace {
using Bytes=std::vector<unsigned char>;
constexpr std::uint32_t block=2048;
// Defined below Image; needed while validating hidden El Torito extents.
bool intersect(std::uint64_t a,std::uint64_t a_bytes,
               std::uint64_t b,std::uint64_t b_bytes) noexcept;
void need(bool ok,const char* message) { if(!ok)throw EditUnsupported(message); }
std::uint32_t le(const Bytes& b,std::size_t p,unsigned n=4) {
    need(p+n<=b.size(),"truncated ISO field");std::uint32_t v=0;
    for(unsigned i=0;i<n;++i)v|=std::uint32_t(b[p+i])<<(8*i);
    return v;
}
std::uint64_t le64(const Bytes& b,std::size_t p) {
    return std::uint64_t(le(b,p))|(std::uint64_t(le(b,p+4))<<32);
}
// UEFI GPT uses the IEEE CRC32 polynomial, not the UDF tag CRC16.
std::uint32_t gpt_crc32(const Bytes& bytes) {
    std::uint32_t crc=0xffffffffu;
    for(const auto value:bytes) {
        crc^=value;
        for(unsigned i=0;i<8;++i)
            crc=(crc>>1)^((crc&1u)?0xedb88320u:0u);
    }
    return ~crc;
}
void put(Bytes& b,std::size_t p,std::uint32_t v,unsigned n=4,bool big=false) {
    for(unsigned i=0;i<n;++i)b.at(p+i)=static_cast<unsigned char>(v>>(8*(big?n-1-i:i)));
}
void both(Bytes& b,std::size_t p,std::uint32_t v,unsigned n=4){put(b,p,v,n);put(b,p+n,v,n,true);}
std::string upper(std::string s){for(auto& c:s)if(c>='a'&&c<='z')c=char(c-32);return s;}
std::u16string utf16(const std::string& text) {
    std::u16string out;
    for(std::size_t i=0;i<text.size();) {
        const auto c=static_cast<unsigned char>(text[i++]);
        unsigned value=c,count=0;
        if(c<128) {
            need(c>=32 && c!=127,"invalid control in Joliet name");
        } else if(c>=0xc2&&c<=0xdf) {
            value=c&31;count=1;
        } else if(c>=0xe0&&c<=0xef) {
            value=c&15;count=2;
        } else if(c>=0xf0&&c<=0xf4) {
            value=c&7;count=3;
        } else {
            throw EditUnsupported("invalid leading UTF-8 byte in Joliet name");
        }
        for(unsigned j=0;j<count;++j) {
            need(i<text.size(),"truncated UTF-8");
            const auto x=static_cast<unsigned char>(text[i++]);
            need((x&0xc0)==0x80,"invalid UTF-8 continuation");
            value=(value<<6)|(x&63);
        }
        need((count!=1||value>=128) &&
             (count!=2||value>=2048) &&
             (count!=3||value>=65536) &&
             value<=0x10ffff &&
             !(value>=0xd800&&value<=0xdfff),"overlong or invalid UTF-8");
        if(value<=0xffff) out.push_back(static_cast<char16_t>(value));
        else {
            value-=0x10000;
            out.push_back(static_cast<char16_t>(0xd800+(value>>10)));
            out.push_back(static_cast<char16_t>(0xdc00+(value&0x3ff)));
        }
    }
    return out;
}
std::string decode(const Bytes& name,bool joliet,bool directory) {
    std::string s;
    if(!joliet) s.assign(name.begin(),name.end());
    else {
        need((name.size()%2)==0,"invalid Joliet UTF-16 byte length");
        for(std::size_t i=0;i<name.size();i+=2) {
            unsigned cp=(unsigned(name[i])<<8)|name[i+1];
            if(cp>=0xd800&&cp<=0xdbff) {
                need(i+3<name.size(),"truncated Joliet surrogate pair");
                const auto low=(unsigned(name[i+2])<<8)|name[i+3];
                need(low>=0xdc00&&low<=0xdfff,"invalid Joliet surrogate pair");
                cp=0x10000+((cp-0xd800)<<10)+(low-0xdc00);i+=2;
            } else {
                need(cp<0xdc00||cp>0xdfff,"isolated Joliet low surrogate");
            }
            if(cp<128) s+=static_cast<char>(cp);
            else if(cp<2048) {
                s+=static_cast<char>(0xc0|(cp>>6));
                s+=static_cast<char>(0x80|(cp&63));
            } else if(cp<65536) {
                s+=static_cast<char>(0xe0|(cp>>12));
                s+=static_cast<char>(0x80|((cp>>6)&63));
                s+=static_cast<char>(0x80|(cp&63));
            } else {
                s+=static_cast<char>(0xf0|(cp>>18));
                s+=static_cast<char>(0x80|((cp>>12)&63));
                s+=static_cast<char>(0x80|((cp>>6)&63));
                s+=static_cast<char>(0x80|(cp&63));
            }
        }
    }
    if(!directory && s.ends_with(";1"))s.resize(s.size()-2);
    return edit_path(s);
}
struct FileSection {
    std::uint32_t lba{},length{};
    bool operator==(const FileSection&) const = default;
};
struct Node {
    std::string name,logical;
    Bytes record,identifier,joliet_identifier,joliet_record;
    std::uint64_t record_at=0,joliet_record_at=0;
    Bytes rr_record,rr_dot,rr_parent;
    bool rr=false,rr_uneditable=false;
    std::size_t old_subdirs=0;
    bool directory=false;
    std::uint32_t lba=0;
    std::uint64_t size=0;
    std::vector<FileSection> sections;
    std::filesystem::path source;
    Node* parent=nullptr;
    std::vector<std::unique_ptr<Node>> children;
    std::array<std::uint32_t,2> new_lba{},new_size{};
};
// File sections need not be adjacent on the medium.
template<class F> void each_extent(const Node& n,F&& visit) {
    if(n.directory || n.sections.empty())
        visit(std::uint64_t(n.lba)*block,n.size);
    else for(const auto& part:n.sections)
        visit(std::uint64_t(part.lba)*block,std::uint64_t(part.length));
}
std::string iso_alias(const std::string& name,bool directory,unsigned salt=0);
class Image {
public:
    std::ifstream file;
    std::uint64_t size;
    std::vector<std::pair<std::uint32_t,Bytes>> descriptors;
    std::set<std::uint32_t> boot_lbas;
    std::set<std::uint32_t> boot_descriptor_lbas;
    struct BootEntry {
        std::uint32_t lba=0;
        std::uint8_t media=0;
        std::uint16_t initial_512_sectors=0;
        bool operator==(const BootEntry&) const = default;
    };
    std::vector<BootEntry> boot_entries;
    std::vector<std::pair<std::uint64_t,std::uint64_t>> boot_regions;
    bool boot_regions_bounded=true;
    std::set<std::uint32_t> unbounded_boot_lbas;
    std::vector<std::pair<std::uint64_t,std::uint64_t>> udf_metadata_regions;
    std::set<std::uint64_t> udf_anchor_lbas;
    EditProtectedRanges iso_auxiliary_regions;
    EditProtectedRanges hybrid_protected_regions;
    bool gpt_hybrid=false;
    std::optional<std::uint8_t> susp_skip;
    // Joliet directories may occupy different sectors from Primary ISO.
    // Retain both namespaces' extents when protecting hidden boot media.
    std::vector<std::pair<std::uint64_t,std::uint64_t>> joliet_regions;
    std::uint32_t terminator_lba=0;
    std::unique_ptr<Node> root;
    bool joliet=false;
    bool udf_bridge=false;
    bool hybrid_system_area=false; // MBR/GPT present; only byte-stable payload edits
    bool iso_system_use=false; // RRIP/SUSP metadata must never be discarded
    bool rr_rebuild=false;
    std::uint32_t declared_volume_blocks=0;
    std::uint32_t primary_volume_blocks=0;
    std::uint32_t supplementary_volume_blocks=0;
    std::size_t items=0;
    explicit Image(const std::filesystem::path& p,bool allow_udf_bridge=false)
        :file(p,std::ios::binary),size(std::filesystem::file_size(p)) {
        need(file.good()&&size>=18*block&&size%block==0&&size/block<0xffffffffu,"unsupported ISO size/layout");
        const auto system=read(0,16*block);
        hybrid_system_area=std::any_of(system.begin(),system.end(),
                                     [](auto c){return c!=0;});
        if(hybrid_system_area) {
            // Partition-bearing media may only use the equal-length
            // payload writer. No partition resizing or GPT relocation.
            need(system[510]==0x55 && system[511]==0xaa,
                 "nonzero ISO system area is not a recognized MBR");
            gpt_hybrid=std::memcmp(system.data()+512,"EFI PART",8)==0;
            for(const std::size_t offset:{2048u,4096u})
                need(std::memcmp(system.data()+offset,"EFI PART",8)!=0,
                     "GPT with non-512-byte logical sectors is not yet writable");
            const auto limit=size/512u;
            bool has_partition=false,protective=false;
            for(std::size_t i=0;i<4;++i) {
                const auto at=446u+i*16u;
                const auto type=system[at+4];
                need(system[at]==0 || system[at]==0x80,
                     "isohybrid MBR has an invalid boot status");
                const auto begin=le(system,at+8);
                const auto length=le(system,at+12);
                if(type) {
                    need(length && begin<limit && length<=limit-begin,
                         "isohybrid MBR partition exceeds the image");
                    has_partition=true;
                    if(type==0xee) {
                        need(!protective && gpt_hybrid && system[at]==0 &&
                             begin==1 &&
                             length==std::min<std::uint64_t>(limit-1,0xffffffffull),
                             "invalid protective MBR for GPT hybrid ISO");
                        protective=true;
                    } else if(begin!=0) {
                        // A partition starting at disk LBA 0 is the
                        // traditional whole-ISO isohybrid container.
                        // Every separately addressed MBR partition,
                        // including an ESP or extended container, stays
                        // byte-for-byte immutable.
                        hybrid_protected_regions.emplace_back(
                            std::uint64_t(begin)*512u,std::uint64_t(length)*512u);
                    }
                }else{
                    need(!begin && !length,
                         "isohybrid MBR contains an inconsistent empty partition");
                }
            }
            need(has_partition,"nonzero ISO system area has no valid MBR partition");
            if(gpt_hybrid) {
                need(protective,"GPT hybrid ISO lacks a protective MBR");
                validate_gpt();
            }
        }
        // Detect UDF bridges. General directory rebuilding is still
        // forbidden: only a separately validated equal-length data edit
        // may leave both on-disc ISO and UDF namespaces unchanged.
        for(std::uint32_t lba=16;lba<256&&std::uint64_t(lba+1)*block<=size;++lba) {
            auto b=read(std::uint64_t(lba)*block,8);
            if(!std::memcmp(b.data()+1,"BEA01",5)||
               !std::memcmp(b.data()+1,"NSR02",5)||
               !std::memcmp(b.data()+1,"NSR03",5))
                udf_bridge=true;
        }
        // A damaged or nonstandard bridge can be missing its VRS sequence
        // while still carrying live UDF anchors. Never rewrite only the ISO
        // namespace of an image with a plausible ECMA-167 AVDP.
        const auto sectors=size/block;
        auto avdp=[&](std::uint64_t lba) {
            if(lba>=sectors)return false;
            const auto tag=read(lba*block,16);
            if(le(tag,0,2)!=2 || (le(tag,2,2)!=2 && le(tag,2,2)!=3) ||
               le(tag,12)!=lba)return false;
            unsigned sum=0;
            for(std::size_t i=0;i<tag.size();++i)
                if(i!=4)sum+=tag[i];
            return static_cast<unsigned char>(sum)==tag[4];
        };
        auto add_udf_anchor=[&](std::uint64_t lba) {
            if(!avdp(lba))return false;
            udf_anchor_lbas.insert(lba);
            udf_metadata_regions.emplace_back(lba*block,block);
            // Both descriptor sequences described by the AVDP are immutable
            // when the payload-only bridge patcher is active.
            const auto descriptor=read(lba*block,32);
            for(const std::size_t offset:{std::size_t(16),std::size_t(24)}) {
                const auto length=le(descriptor,offset);
                const auto start=le(descriptor,offset+4);
                if(!length)continue;
                need(start<=sectors && length<=size-std::uint64_t(start)*block,
                     "UDF volume descriptor sequence outside ISO image");
                udf_metadata_regions.emplace_back(
                    std::uint64_t(start)*block,length);
            }
            return true;
        };
        // The UDF reader also accepts a valid AVDP at sector 512. Missing
        // this position would let the ISO writer misclassify a real UDF
        // bridge as plain ISO9660 and rebuild its metadata destructively.
        // Inspect every candidate, even if VRS or an earlier anchor matched,
        // so backup anchor and descriptor extents stay protected too.
        if(add_udf_anchor(256))udf_bridge=true;
        if(sectors>512 && add_udf_anchor(512))udf_bridge=true;
        // ECMA-167 N is the last logical sector, not the sector count.
        // Accept both N-256 and the legacy count-256 probe.
        if(sectors>257 && add_udf_anchor(sectors-257))udf_bridge=true;
        if(sectors>256 && add_udf_anchor(sectors-256))udf_bridge=true;
        if(sectors && add_udf_anchor(sectors-1))udf_bridge=true;
        need(allow_udf_bridge || !udf_bridge,
             "ISO/UDF Bridge directory edits require a synchronized UDF writer");
        Bytes primary,supplementary;bool end=false;
        for(std::uint32_t lba=16;lba<64;++lba){auto b=read(std::uint64_t(lba)*block,block);need(std::memcmp(b.data()+1,"CD001",5)==0&&b[6]==1,"unsupported ISO descriptors");
            if(b[0]==255){end=true;terminator_lba=lba;break;}
            if(b[0]==0){need(std::memcmp(b.data()+7,"EL TORITO SPECIFICATION",23)==0,"unsupported boot descriptor");boot_descriptor_lbas.insert(lba);boot_catalog(le(b,71));continue;}
            need(b[0]==1||b[0]==2,"unsupported ISO descriptor type");
            if(b[0]==1){need(primary.empty(),"multiple primary descriptors");primary=b;}
            else {need(supplementary.empty()&&b[88]=='%'&&b[89]=='/'&&(b[90]=='@'||b[90]=='C'||b[90]=='E'),"unsupported supplementary descriptor");supplementary=b;joliet=true;}
            need(le(b,128,2)==block&&le(b,120,2)==1&&le(b,124,2)==1,"unsupported volume geometry");
            // Some valid ISO files are padded after the declared ISO9660
            // volume. A strict equality test rejected F5/delete on them,
            // even though none of the ISO metadata or data lives in the pad.
            const auto volume_blocks=le(b,80);
            need(volume_blocks>=18 && volume_blocks<=sectors,
                 "ISO descriptor volume exceeds actual image bounds");
            // Primary and Joliet are independent volume descriptors.
            // Some mastering tools declare different bounded view sizes.
            // Preserve the larger view's complete source prefix; the writer
            // will normalize both descriptors to the rebuilt image size.
            if(b[0]==1)primary_volume_blocks=volume_blocks;
            else supplementary_volume_blocks=volume_blocks;
            declared_volume_blocks=std::max(declared_volume_blocks,volume_blocks);
            descriptors.emplace_back(lba,std::move(b));
        }
        need(end&&!primary.empty(),"missing ISO primary/terminator");
        // Detect UDF anchors relative to the logical volume as well as the
        // physical file end. ISO/UDF bridges may have zero padding after the
        // actual ECMA-167 anchor, and cannot use the plain ISO rebuild path.
        for(const auto volume_blocks:
            {primary_volume_blocks,supplementary_volume_blocks}) {
            if(!volume_blocks || volume_blocks>=sectors)continue;
            if(volume_blocks>257)
                udf_bridge=add_udf_anchor(volume_blocks-257) || udf_bridge;
            if(volume_blocks>256)
                udf_bridge=add_udf_anchor(volume_blocks-256) || udf_bridge;
            udf_bridge=add_udf_anchor(volume_blocks-1) || udf_bridge;
        }
        need(allow_udf_bridge || !udf_bridge,
             "ISO/UDF Bridge directory edits require a synchronized UDF writer");
        // Inspect UDF anchors before classifying a hybrid MBR image.
        // Hybrid ISO without UDF is parsed only for the strictly
        // same-size, no-metadata writer in edit_iso(). No ISO rebuild
        // or partition-table update is permitted for these images.
        // A non-UDF ISO can have an all-zero sector-aligned trailer. Preserve
        // it during staging, then update the descriptor's volume size when
        // appending changed files/metadata. Never accept hidden nonzero
        // partitions, boot payloads or other data after the declared volume.
        // A detected ISO/UDF bridge takes the separate equal-size-only path,
        // which validates both filesystem namespaces and never moves extents.
        // A validated GPT backup header and entry array may be outside
        // the ISO volume. The hybrid writer never changes that trailer.
        if(!udf_bridge && !gpt_hybrid && declared_volume_blocks<sectors) {
            const auto trailer_bytes=(sectors-declared_volume_blocks)*block;
            constexpr std::uint64_t max_padding=256ull*1024*1024;
            need(trailer_bytes<=max_padding,
                 "ISO trailer exceeds safe zero-padding inspection limit (256 MiB)");
            file.clear();
            file.seekg(static_cast<std::streamoff>(
                std::uint64_t(declared_volume_blocks)*block));
            std::array<char,64*1024> chunk{};
            auto remaining=trailer_bytes;
            while(remaining) {
                const auto n=static_cast<std::streamsize>(
                    std::min<std::uint64_t>(remaining,chunk.size()));
                file.read(chunk.data(),n);
                need(bool(file),"cannot validate ISO trailer");
                need(std::all_of(chunk.begin(),chunk.begin()+n,
                    [](char c){return c==0;}),
                    "nonzero data beyond ISO volume (possible boot/partition metadata)");
                remaining-=static_cast<std::uint64_t>(n);
            }
        }
        if(udf_bridge) {
            // A path table is a separate extent, not a directory record.
            // Its original bytes must survive all UDF-only edits.
            for(const auto& [lba,vd]:descriptors) {
                (void)lba;
                const auto table_bytes=le(vd,132);
                if(!table_bytes)continue;
                for(const std::size_t at:{140u,144u,148u,152u}) {
                    const auto table_lba=(at<148u)?le(vd,at):
                        ((std::uint32_t(vd[at])<<24)|
                         (std::uint32_t(vd[at+1])<<16)|
                         (std::uint32_t(vd[at+2])<<8)|vd[at+3]);
                    if(!table_lba)continue;
                    const auto offset=std::uint64_t(table_lba)*block;
                    need(offset<=size && table_bytes<=size-offset,
                         "ISO/UDF path table exceeds image bounds");
                    iso_auxiliary_regions.emplace_back(offset,table_bytes);
                }
            }
        }
        std::set<std::uint32_t> seen;root=parse(Bytes(primary.begin()+156,primary.begin()+190),false,nullptr,0,seen);
        root->name="";root->logical="";
        if(joliet) {
            seen.clear();
            auto other=parse(Bytes(supplementary.begin()+156,
                                   supplementary.begin()+190),true,nullptr,0,seen);
            if(primary_volume_blocks!=supplementary_volume_blocks &&
               !(hybrid_system_area && iso_system_use)) {
                // A mismatched descriptor is safe to rebuild only when
                // EACH independent namespace fits inside its OWN declared
                // volume. Reject truncated trees and path tables rather than
                // trusting the larger descriptor or physical ISO length.
                const auto validate_view=[&](const Node& view,const Bytes& vd,
                                             std::uint32_t blocks) {
                    const auto limit=std::uint64_t(blocks)*block;
                    std::vector<const Node*> nodes{&view};
                    for(std::size_t i=0;i<nodes.size();++i) {
                        const auto* node=nodes[i];
                        each_extent(*node,[&](std::uint64_t at,std::uint64_t length) {
                            need(at<=limit && length<=limit-at,
                                 "ISO/Joliet view extent exceeds its own declared volume");
                        });
                        for(const auto& child:node->children)
                            nodes.push_back(child.get());
                        need(nodes.size()<=200000,"ISO/Joliet view entry limit exceeded");
                    }
                    const auto be32=[&](std::size_t at) {
                        return (std::uint32_t(vd[at])<<24) |
                               (std::uint32_t(vd[at+1])<<16) |
                               (std::uint32_t(vd[at+2])<<8) |
                                std::uint32_t(vd[at+3]);
                    };
                    need(be32(84)==blocks,
                         "ISO/Joliet volume size byte orders disagree");
                    const auto table_bytes=le(vd,132);
                    if(table_bytes) {
                        for(const std::size_t at:{140u,144u,148u,152u}) {
                            const auto lba=(at==140u||at==144u)?
                                le(vd,at):be32(at);
                            if(!lba)continue;
                            const auto offset=std::uint64_t(lba)*block;
                            need(offset<=limit && table_bytes<=limit-offset,
                                 "ISO/Joliet path table exceeds its own declared volume");
                        }
                    }
                };
                validate_view(*root,primary,primary_volume_blocks);
                validate_view(*other,supplementary,supplementary_volume_blocks);
            }
            // A few RRIP/isohybrid masters intentionally advertise a
            // different Joliet Volume Space Size while their data extents
            // remain physically bounded. On the STRICT unchanged-length
            // patcher, that inconsistency is preserved verbatim rather
            // than normalized or used to authorize new allocation.
            std::vector<const Node*> supplementary_nodes{other.get()};
            for(std::size_t i=0;i<supplementary_nodes.size();++i) {
                const auto* node=supplementary_nodes[i];
                each_extent(*node,[&](std::uint64_t at,std::uint64_t length) {
                    if(length)joliet_regions.emplace_back(at,length);
                });
                for(const auto& child:node->children)
                    supplementary_nodes.push_back(child.get());
                need(supplementary_nodes.size()<=200000,
                     "Joliet boot-protection extent limit exceeded");
            }
            // SUSP/Rock Ridge names need not match Joliet's case and aliases.
            // An MBR hybrid cannot rebuild metadata, so do not rewrite or
            // 'normalize' the independent supplementary namespace here.
            // Both trees were parsed, and all original bytes will be guarded.
            if(!(hybrid_system_area && iso_system_use))
                pair(*root,*other);
        }
        // Some genuine bootable ISO/UDF bridges set El Torito's 512-byte
        // sector count to zero or to just one boot sector. When the ISO9660
        // namespace independently describes the full boot image extent,
        // protect the WHOLE referenced file, not merely the catalog count.
        // Unknown boot images absent from ISO remain strictly read-only.
        std::set<std::uint32_t> resolved_boot_images;
        std::vector<const Node*> boot_nodes{root.get()};
        std::vector<const Node*> all_boot_nodes;
        while(!boot_nodes.empty()) {
            const Node* node=boot_nodes.back();
            boot_nodes.pop_back();
            all_boot_nodes.push_back(node);
            for(const auto& child:node->children)
                boot_nodes.push_back(child.get());
            if(node->directory || !node->size)continue;
            if(node->sections.size()>1) {
                // El Torito consumes physical sectors, not logical
                // multi-extent streams. Refuse ambiguous boot aliases.
                each_extent(*node,[&](std::uint64_t at,std::uint64_t length) {
                    for(const auto lba:boot_lbas)
                        need(!intersect(at,length,std::uint64_t(lba)*block,block),
                             "multi-extent file contains an El Torito reference");
                    for(const auto& [offset,bytes]:boot_regions)
                        need(!intersect(at,length,offset,bytes),
                             "multi-extent file overlaps El Torito data");
                });
                continue;
            }
            const auto start=std::uint64_t(node->lba)*block;
            const auto end=start+node->size;
            for(const auto lba:boot_lbas) {
                const auto reference=std::uint64_t(lba)*block;
                if(reference>=start && reference<end) {
                    boot_regions.emplace_back(start,node->size);
                    unbounded_boot_lbas.erase(lba);
                    for(const auto& entry:boot_entries) {
                        if(entry.lba!=lba)continue;
                        need(resolved_boot_images.insert(lba).second,
                             "El Torito boot image maps to multiple ISO extents");
                        // No-emulation catalogs created by standard ISO
                        // mastering tools can advertise loader sector counts
                        // that are not the size of their ISO-visible boot file.
                        // They remain byte-protected, as before this change.
                        if(!entry.media)continue; // no-emulation has no disk geometry
                        // For floppy/HDD emulation, verify loader bytes fit
                        // the independently validated disk image extent.
                        need(std::uint64_t(entry.initial_512_sectors)*512<=end-reference,
                             "emulated El Torito initial load exceeds disk image file");
                        need(reference==start,
                             "emulated El Torito image must start at ISO file extent");
                        if(entry.media<=3) {
                            constexpr std::uint32_t floppy_sizes[]={
                                0u,1200u*1024u,1440u*1024u,2880u*1024u};
                            need(node->size==floppy_sizes[entry.media],
                                 "emulated El Torito floppy image size disagrees with media");
                        } else {
                            need(node->size>=1024u && node->size%512u==0,
                                 "emulated El Torito hard disk image is not sector aligned");
                            // A hard-disk-emulated image must be an independently
                            // bounded ISO file with a self-contained MBR.
                            // Reject partitions pointing outside that file;
                            // never infer disk length from SectorCount.
                            const auto mbr=read(start,512);
                            need(mbr[510]==0x55 && mbr[511]==0xaa,
                                 "emulated El Torito hard disk MBR signature missing");
                            const std::uint64_t sectors=node->size/512u;
                            bool has_partition=false;
                            for(unsigned i=0;i<4;++i) {
                                const auto at=446u+i*16u;
                                const auto status=mbr[at],type=mbr[at+4u];
                                const auto first=le(mbr,at+8u);
                                const auto length=le(mbr,at+12u);
                                need(status==0 || status==0x80,
                                     "emulated El Torito hard disk partition flag invalid");
                                if(type) {
                                    need(type!=0xee && first && length &&
                                         first<sectors && length<=sectors-first,
                                         "emulated El Torito hard disk partition outside image");
                                    has_partition=true;
                                } else need(!first && !length,
                                            "emulated El Torito hard disk empty partition invalid");
                            }
                            need(has_partition,
                                 "emulated El Torito hard disk has no bounded MBR partition");
                        }
                    }
                }
            }
        }
        // Floppy emulation has an exact size defined by El Torito media
        // type (1.2/1.44/2.88 MiB). Even when the floppy is not a named
        // ISO9660 file, its FULL extent can be independently bounded and
        // byte-protected. HDD emulation has no such geometry: keep a hidden
        // HDD strictly read-only rather than guessing its last sector.
        std::set<std::uint32_t> resolved_hidden_floppies;
        for(const auto& entry:boot_entries) {
            if(!entry.media || resolved_boot_images.contains(entry.lba))continue;
            need(entry.media<=3,
                 "emulated El Torito hard disk is not an ISO-visible file");
            constexpr std::uint32_t floppy_sizes[]={
                0u,1200u*1024u,1440u*1024u,2880u*1024u};
            const auto bytes=std::uint64_t(floppy_sizes[entry.media]);
            // Validate EVERY catalog entry, including duplicate references
            // to the same floppy LBA with a different initial load count.
            need(std::uint64_t(entry.initial_512_sectors)*512<=bytes,
                 "hidden El Torito floppy load exceeds media geometry");
            if(!resolved_hidden_floppies.insert(entry.lba).second)continue;
            const auto start=std::uint64_t(entry.lba)*block;
            const auto volume=std::uint64_t(declared_volume_blocks)*block;
            need(start<=size && bytes<=size-start &&
                 start<=volume && bytes<=volume-start,
                 "hidden El Torito floppy extent exceeds ISO volume");
            for(const auto* node:all_boot_nodes) {
                each_extent(*node,[&](std::uint64_t at,std::uint64_t length) {
                    need(!intersect(start,bytes,at,length),
                         "hidden El Torito floppy overlaps an ISO file or directory");
                });
            }
            for(const auto& [offset,length]:joliet_regions)
                need(!intersect(start,bytes,offset,length),
                     "hidden El Torito floppy overlaps Joliet file or directory");
            need(terminator_lba>=16 &&
                 !intersect(start,bytes,16ull*block,
                            std::uint64_t(terminator_lba-15u)*block),
                 "hidden El Torito floppy overlaps ISO descriptor sequence");
            for(const auto other_lba:boot_lbas)
                if(other_lba!=entry.lba)
                    need(!intersect(start,bytes,
                                    std::uint64_t(other_lba)*block,block),
                         "hidden El Torito floppy overlaps another boot resource");
            for(const auto lba:boot_descriptor_lbas)
                need(!intersect(start,bytes,std::uint64_t(lba)*block,block),
                     "hidden El Torito floppy overlaps boot descriptor");
            for(const auto& [offset,length]:boot_regions)
                if(offset!=start)
                    need(!intersect(start,bytes,offset,length),
                         "hidden El Torito floppy overlaps protected boot data");
            for(const auto& [offset,length]:udf_metadata_regions)
                need(!intersect(start,bytes,offset,length),
                     "hidden El Torito floppy overlaps UDF metadata");
            for(const auto& [lba,vd]:descriptors) {
                need(!intersect(start,bytes,std::uint64_t(lba)*block,block),
                     "hidden El Torito floppy overlaps volume descriptor");
                const auto table_bytes=le(vd,132);
                if(!table_bytes)continue;
                const auto be32=[&](std::size_t at) {
                    return (std::uint32_t(vd[at])<<24) |
                           (std::uint32_t(vd[at+1])<<16) |
                           (std::uint32_t(vd[at+2])<<8) |
                            std::uint32_t(vd[at+3]);
                };
                for(const std::size_t at:{140u,144u,148u,152u}) {
                    const auto table_lba=(at==140u||at==144u)?
                        le(vd,at):be32(at);
                    if(!table_lba)continue;
                    const auto offset=std::uint64_t(table_lba)*block;
                    need(offset<=size && table_bytes<=size-offset,
                         "hidden El Torito floppy has invalid ISO path table");
                    need(!intersect(start,bytes,offset,table_bytes),
                         "hidden El Torito floppy overlaps ISO path table");
                }
            }
            boot_regions.emplace_back(start,bytes);
            unbounded_boot_lbas.erase(entry.lba);
        }
        boot_regions_bounded=unbounded_boot_lbas.empty();
    }
    Bytes read(std::uint64_t at,std::size_t count){need(at<=size&&count<=size-at,"ISO extent outside image");Bytes b(count);file.clear();file.seekg(static_cast<std::streamoff>(at));file.read(reinterpret_cast<char*>(b.data()),static_cast<std::streamsize>(count));need(bool(file),"ISO read failed");return b;}
    void boot_catalog(std::uint32_t lba) {
        auto b=read(std::uint64_t(lba)*block,block);need(b[0]==1&&b[30]==0x55&&b[31]==0xaa,"invalid boot catalog");unsigned checksum=0;for(unsigned i=0;i<32;i+=2)checksum+=le(b,i,2);need((checksum&0xffff)==0,"invalid boot catalog checksum");boot_lbas.insert(lba);
        boot_regions.emplace_back(std::uint64_t(lba)*block,block);
        auto entry=[&](std::size_t at){
            need(b[at]==0||b[at]==0x88,"unsupported boot entry");
            if(b[at]!=0x88)return;
            const auto media=b[at+1];
            need(media<=4,"unsupported El Torito boot media type");
            const auto loc=le(b,at+8);
            need(loc && std::uint64_t(loc)*block<size,
                 "invalid boot image extent");
            // Reject contradictory claims about the same boot LBA.
            for(const auto& previous:boot_entries)
                need(previous.lba!=loc || previous.media==media,
                     "conflicting El Torito boot media for one extent");
            const auto count=le(b,at+6,2);
            boot_entries.push_back({loc,media,static_cast<std::uint16_t>(count)});
            boot_lbas.insert(loc);
            // A 512-byte initial load cannot bound the full emulated disk.
            // Require a separately validated ISO file extent for floppy/HDD.
            if(media || !count) {
                boot_regions_bounded=false;
                unbounded_boot_lbas.insert(loc);
            }
            if(!count)return;
            const auto bytes=(std::uint64_t(count)*512+block-1)/block*block;
            const auto start=std::uint64_t(loc)*block;
            need(bytes<=size-start,"El Torito boot image outside ISO bounds");
            boot_regions.emplace_back(start,bytes);
        };
        entry(32);std::size_t at=64;
        while(at<block&&b[at]){need(b[at]==0x90||b[at]==0x91,"unsupported boot catalog extension");bool last=b[at]==0x91;unsigned count=le(b,at+2,2);at+=32;need(count<=(block-at)/32,"large boot catalog unsupported");for(unsigned i=0;i<count;++i,at+=32)entry(at);if(last)break;}
    }
    // Follow only SUSP continuation records here. Unlike RR name decoding,
    // this also supports dot/parent entries and unknown SUSP extensions.
    // Every continuation byte is part of the immutable ISO namespace.
    void validate_gpt() {
        constexpr std::uint64_t sector=512;
        const auto count=size/sector;
        need(count>=68,"GPT image too small for primary and backup metadata");
        const auto final_lba=count-1;
        struct Header {
            Bytes bytes,entries;
            std::uint64_t first{},last{},table{},table_sectors{};
            std::uint32_t number{},entry_size{};
        };
        auto parse_header=[&](std::uint64_t lba,std::uint64_t alternate) {
            Header h;
            h.bytes=read(lba*sector,sector);
            need(std::memcmp(h.bytes.data(),"EFI PART",8)==0 &&
                 le(h.bytes,8)==0x00010000u && le(h.bytes,20)==0,
                 "invalid or unsupported GPT header");
            const auto header_size=le(h.bytes,12);
            need(header_size>=92 && header_size<=sector,
                 "GPT header size is out of bounds");
            auto crc_data=Bytes(h.bytes.begin(),h.bytes.begin()+header_size);
            const auto crc=le(crc_data,16);
            put(crc_data,16,0);
            need(gpt_crc32(crc_data)==crc,"GPT header CRC32 mismatch");
            need(le64(h.bytes,24)==lba && le64(h.bytes,32)==alternate,
                 "GPT header locations disagree");
            h.first=le64(h.bytes,40);
            h.last=le64(h.bytes,48);
            h.table=le64(h.bytes,72);
            h.number=le(h.bytes,80);
            h.entry_size=le(h.bytes,84);
            need(h.first>=2 && h.first<=h.last && h.last<final_lba &&
                 h.number>0 && h.number<=16384 &&
                 h.entry_size>=128 && h.entry_size<=4096 &&
                 h.entry_size%128u==0,
                 "unsupported GPT usable range or entry geometry");
            const auto bytes=std::uint64_t(h.number)*h.entry_size;
            need(bytes<=16ull*1024*1024,"GPT entry array exceeds safety budget");
            h.table_sectors=(bytes+sector-1)/sector;
            need(h.table>=2 && h.table<count &&
                 h.table_sectors<=count-h.table,
                 "GPT entry array is outside the image");
            if(lba==1)need(h.table+h.table_sectors<=h.first,
                           "primary GPT entry array overlaps usable sectors");
            else need(h.table>h.last && h.table+h.table_sectors<=lba,
                      "backup GPT entry array overlaps usable sectors or header");
            h.entries=read(h.table*sector,static_cast<std::size_t>(bytes));
            need(gpt_crc32(h.entries)==le(h.bytes,88),
                 "GPT partition entry array CRC32 mismatch");
            return h;
        };
        const auto primary=parse_header(1,final_lba);
        const auto backup=parse_header(final_lba,1);
        need(primary.first==backup.first && primary.last==backup.last &&
             primary.number==backup.number &&
             primary.entry_size==backup.entry_size &&
             std::equal(primary.bytes.begin()+56,primary.bytes.begin()+72,
                        backup.bytes.begin()+56) &&
             std::any_of(primary.bytes.begin()+56,primary.bytes.begin()+72,
                         [](auto c){return c!=0;}) &&
             primary.entries==backup.entries,
             "primary and backup GPT metadata disagree");
        hybrid_protected_regions.emplace_back(sector,sector);
        hybrid_protected_regions.emplace_back(final_lba*sector,sector);
        for(const auto* h:{&primary,&backup})
            hybrid_protected_regions.emplace_back(
                h->table*sector,h->table_sectors*sector);
        std::vector<std::pair<std::uint64_t,std::uint64_t>> partitions;
        // A GUID is exactly 128 bits. Use two fixed-width words as the
        // identity key instead of comparing variable-length byte vectors.
        // GCC 13/14 can misdiagnose the vector spaceship operator's
        // optimized memcmp bound as -Wstringop-overread under -Werror.
        std::set<std::pair<std::uint64_t,std::uint64_t>> unique_guids;
        for(std::uint32_t i=0;i<primary.number;++i) {
            const auto at=std::size_t(i)*primary.entry_size;
            const auto* type=primary.entries.data()+at;
            if(std::all_of(type,type+16,[](auto c){return c==0;}))continue;
            const auto guid=std::pair{le64(primary.entries,at+16),
                                      le64(primary.entries,at+24)};
            need((guid.first!=0 || guid.second!=0) &&
                 unique_guids.insert(guid).second,
                 "GPT partition has a zero or duplicate unique GUID");
            const auto first=le64(primary.entries,at+32);
            const auto last=le64(primary.entries,at+40);
            need(first>=primary.first && first<=last && last<=primary.last,
                 "GPT partition exceeds the usable sector range");
            partitions.emplace_back(first,last);
        }
        need(!partitions.empty(),"GPT hybrid contains no partitions");
        std::sort(partitions.begin(),partitions.end());
        for(std::size_t i=0;i<partitions.size();++i) {
            const auto [first,last]=partitions[i];
            need(i==0 || partitions[i-1].second<first,
                 "overlapping GPT partitions are not writable");
            // Never guess whether an ISO file is also live partition
            // payload. Protect ALL GPT partitions, including EFI and data.
            hybrid_protected_regions.emplace_back(
                first*sector,(last-first+1)*sector);
        }
    }
    void protect_susp(const Bytes& record,bool* uneditable=nullptr) {
        if((!udf_bridge && !rr_rebuild && !hybrid_system_area) || !susp_skip)return;
        const auto idlen=std::size_t(record[32]);
        const auto base=33+idlen+(idlen%2==0?1u:0u);
        need(base<=record.size(),"invalid ISO SUSP record");
        if(record.size()==base)return;
        need(*susp_skip<=record.size()-base,"invalid ISO SUSP skip");
        Bytes area(record.begin()+static_cast<std::ptrdiff_t>(base+*susp_skip),
                   record.end());
        std::set<std::pair<std::uint64_t,std::uint64_t>> visited;
        std::uint64_t total=0;
        for(unsigned hop=0;;++hop) {
            std::optional<std::pair<std::uint64_t,std::uint64_t>> next;
            for(std::size_t p=0;p+4<=area.size();) {
                if(!area[p])break;
                const auto n=std::size_t(area[p+2]);
                need(n>=4 && n<=area.size()-p && area[p+3]==1,
                     "invalid ISO SUSP extension");
                const std::string_view signature(
                    reinterpret_cast<const char*>(area.data()+p),2);
                if(rr_rebuild) {
                    // Relocation, compression, sparse-file and unknown
                    // extensions may contain geometry-dependent metadata.
                    // Preserve only records whose semantics are understood.
                    need(signature=="SP" || signature=="ST" ||
                         signature=="CE" || signature=="ER" ||
                         signature=="ES" || signature=="PD" ||
                         signature=="RR" || signature=="PX" ||
                         signature=="NM" || signature=="TF" ||
                         signature=="SL" || signature=="PN",
                         "Rock Ridge extension needs a specialized writer");
                    if(uneditable) {
                        if(signature=="SL" || signature=="PN")*uneditable=true;
                        if(signature=="PX") {
                            need(n==36 || n==44,"invalid Rock Ridge PX");
                            const auto mode=le(area,p+4);
                            const auto kind=mode&0170000u;
                            if((kind!=0 && kind!=0100000u && kind!=0040000u) ||
                               (kind!=0040000u && le(area,p+12)>1))
                                *uneditable=true;
                        }
                    }
                }
                if(signature=="ST")break;
                if(signature=="CE") {
                    need(n>=28 && !next,"invalid ISO SUSP continuation");
                    const auto mirrored=[&](std::size_t q) {
                        const auto value=le(area,q);
                        const auto big=(std::uint32_t(area[q+4])<<24)|
                            (std::uint32_t(area[q+5])<<16)|
                            (std::uint32_t(area[q+6])<<8)|area[q+7];
                        need(value==big,"ISO SUSP continuation byte orders differ");
                        return value;
                    };
                    const auto lba=mirrored(p+4),offset=mirrored(p+12),
                               bytes=mirrored(p+20);
                    need(offset<block && bytes>0 && bytes<=65536,
                         "invalid ISO SUSP continuation bounds");
                    next=std::pair<std::uint64_t,std::uint64_t>{
                        std::uint64_t(lba)*block+offset,bytes};
                }
                p+=n;
            }
            if(!next)break;
            need(hop<8 && next->second<=65536-total &&
                 visited.insert(*next).second,
                 "cyclic or oversized ISO SUSP continuation");
            need(iso_auxiliary_regions.size()<200000,
                 "ISO auxiliary metadata count exceeds safety limit");
            iso_auxiliary_regions.push_back(*next);
            total+=next->second;
            area=read(next->first,static_cast<std::size_t>(next->second));
        }
    }
    std::unique_ptr<Node> parse(const Bytes& r,bool j,Node* parent,unsigned depth,std::set<std::uint32_t>& seen) {
        need(r.size()>=34&&r[0]==r.size()&&r[1]==0&&r[26]==0&&r[27]==0&&le(r,28,2)==1,"unsupported ISO record");
        need((r[25]&~0x83u)==0 && !((r[25]&0x80u) && (r[25]&2u)),
             "associated or unsupported ISO record flags");
        if(r[25]&0x80u)
            need(!udf_bridge && !hybrid_system_area,
                 "multi-extent hybrid ISO requires partition-aware editing");
        auto n=std::make_unique<Node>();n->record=r;n->directory=(r[25]&2)!=0;n->lba=le(r,2);n->size=le(r,10);n->parent=parent;
        need(std::uint64_t(n->lba)*block+n->size<=size,"ISO resource out of bounds");
        if(!n->directory) {
            n->sections.push_back({n->lba,static_cast<std::uint32_t>(n->size)});
            if(r[25]&0x80u)
                need(n->size>0 && n->size%block==0,
                     "nonfinal ISO file section must be block-aligned");
        }
        const std::size_t idlen=r[32],base=33+idlen+(idlen%2==0?1:0);need(idlen&&base<=r.size(),"bad ISO identifier");
        const bool has_system_use=std::any_of(
            r.begin()+static_cast<std::ptrdiff_t>(base),r.end(),
            [](auto c){return c!=0;});
        iso_system_use|=has_system_use;
        if(!j && parent && has_system_use)
            protect_susp(r,rr_rebuild?&n->rr_uneditable:nullptr);
        need(udf_bridge || hybrid_system_area ||
             (!j && rr_rebuild) || !has_system_use,
             "unrecognized ISO System Use metadata requires a preserving writer");
        n->identifier.assign(r.begin()+33,r.begin()+33+static_cast<std::ptrdiff_t>(idlen));
        if(parent)n->name=n->logical=decode(n->identifier,j,n->directory);
        if(!j && rr_rebuild) {
            n->rr=true;
            n->rr_record=r;
            need(!(r[25]&0x80u),
                 "Rock Ridge multi-extent metadata needs a specialized writer");
            if(parent && has_system_use) {
                const auto rr=rock_ridge_entry(r,*susp_skip,
                    [&](std::uint64_t offset,std::size_t length) {
                        return read(offset,length);
                    });
                if(rr.alternate_name)n->logical=edit_path(*rr.alternate_name);
                need(n->logical.find('/')==std::string::npos,
                     "invalid Rock Ridge filename component");
                n->rr_uneditable|=rr.symlink;
                if(rr.posix_mode) {
                    const auto kind=*rr.posix_mode&0170000u;
                    need(kind==0 || kind!=0040000u || n->directory,
                         "Rock Ridge directory type disagrees with ISO record");
                    if(kind && kind!=0040000u && kind!=0100000u)
                        n->rr_uneditable=true;
                }
            }
        }
        need(++items<=200000&&depth<=64,"ISO edit tree limit exceeded");
        if(n->directory){need(seen.insert(n->lba).second&&n->size<=64*1024*1024,"cyclic or oversized ISO directory");auto data=read(std::uint64_t(n->lba)*block,static_cast<std::size_t>(n->size));std::set<std::string> names;
            Node* pending=nullptr;
            for(std::size_t at=0;at<data.size();){unsigned len=data[at];if(!len){at=(at/block+1)*block;continue;}need(len>=34&&len<=data.size()-at&&at%block+len<=block,"bad ISO directory record");Bytes child(data.begin()+static_cast<std::ptrdiff_t>(at),data.begin()+static_cast<std::ptrdiff_t>(at+len));at+=len;
                if(child[32]==1&&child[33]<=1){ // Validate dot records too (Rock Ridge SP/CE can live here).
                    need(!pending,"ISO multi-extent sequence interrupted by dot record");
                    auto tail=std::size_t(34);
                    const bool has_system_use=std::any_of(
                        child.begin()+static_cast<std::ptrdiff_t>(tail),
                        child.end(),[](auto c){return c!=0;});
                    iso_system_use|=has_system_use;
                    if(!j) {
                        if(!parent && child[33]==0) {
                            susp_skip=rock_ridge_skip(child);
                            if(!udf_bridge && !hybrid_system_area && susp_skip) {
                                need(*susp_skip==0,
                                     "Rock Ridge with nonzero SUSP skip is not yet writable");
                                rr_rebuild=rock_ridge_entry(child,*susp_skip,
                                    [&](std::uint64_t offset,std::size_t length) {
                                        return read(offset,length);
                                    }).extension;
                                n->rr=rr_rebuild;
                            }
                        }
                        if(has_system_use)protect_susp(child);
                        if(rr_rebuild) {
                            auto& dot=child[33]==0?n->rr_dot:n->rr_parent;
                            need(dot.empty(),"duplicate Rock Ridge dot record");
                            dot=child;
                        }
                    }
                    need(udf_bridge || hybrid_system_area ||
                         (!j && rr_rebuild) || !has_system_use,
                         "unrecognized ISO directory System Use metadata");
                    continue;}
                auto c=parse(child,j,n.get(),depth+1,seen);
                c->record_at=std::uint64_t(n->lba)*block+at-len;
                const bool more=(child[25]&0x80u)!=0;
                if(pending) {
                    need(!c->directory && pending->identifier==c->identifier &&
                         (pending->record[25]&0x7fu)==(child[25]&0x7fu) &&
                         c->size>0 && pending->sections.size()<65536u,
                         "invalid ISO multi-extent continuation");
                    need(c->size<=size-pending->size,
                         "ISO multi-extent size exceeds image");
                    pending->sections.push_back(c->sections.front());
                    pending->size+=c->size;
                    if(!more)pending=nullptr;
                } else {
                    need(names.insert(upper(c->logical)).second,"ambiguous ISO names");
                    if(more)pending=c.get();
                    n->children.push_back(std::move(c));
                }
            }
            need(!pending,"unterminated ISO multi-extent file");
            if(n->rr) {
                need(!n->rr_dot.empty() && !n->rr_parent.empty(),
                     "Rock Ridge directory lacks dot or parent record");
                for(const auto& child:n->children)
                    if(child->directory)++n->old_subdirs;
            }
        }return n;
    }
    void pair(Node& a,const Node& b) {
        a.joliet_identifier=b.identifier;
        a.joliet_record=b.record;
        a.joliet_record_at=b.record_at;
        if(!rr_rebuild)a.logical=b.logical;
        need(a.children.size()==b.children.size(),"ISO and Joliet trees differ");std::set<const Node*> used;
        for(auto& x:a.children){std::vector<const Node*> found;
            for(const auto& y:b.children)if(!used.count(y.get())&&x->directory==y->directory&&
                    (x->directory?(rr_rebuild||x->size==y->size):
                     (x->size==y->size&&x->sections==y->sections)))found.push_back(y.get());
            if(found.size()>1) {
                std::vector<const Node*> named;
                for(const auto* y:found)
                    if(upper(x->name)==upper(y->name) ||
                       (rr_rebuild && upper(x->logical)==upper(y->logical)))
                        named.push_back(y);
                // A generated ISO alias is never equal to the Unicode
                // name. Preserve candidates to attempt exact hash pairing.
                if(named.size()==1)found=std::move(named);
            }
            if(found.size()>1) {
                // Resolve a generated primary ISO alias against the long
                // Joliet name, without guessing based on directory order.
                std::vector<const Node*> aliased;
                for(const auto* y:found) {
                    for(unsigned salt=0;salt<128;++salt) {
                        if(upper(x->name)==iso_alias(y->logical,y->directory,salt)) {
                            aliased.push_back(y);break;
                        }
                    }
                }
                if(aliased.size()==1) found=std::move(aliased);
            }
            need(found.size()==1,"cannot safely match ISO and Joliet names");used.insert(found[0]);pair(*x,*found[0]);
        }
    }
};
// The directory record is stored in one 255-byte field: 33-byte header,
// identifier and an optional pad byte. Relaxed Joliet Long(110) uses up to
// 110 UTF-16 code units, including the ;1 version suffix for files.
Bytes identifier(const std::string& name,bool j,bool dir) {
    Bytes b;
    if(j) {
        auto wide=utf16(name);
        if(!dir) wide+=u";1";
        need(wide.size()<=110,"Joliet identifier exceeds 110 UTF-16 units");
        b.reserve(wide.size()*2);
        for(const char16_t c:wide) {
            b.push_back(static_cast<unsigned char>(c>>8));
            b.push_back(static_cast<unsigned char>(c));
        }
    } else {
        auto text=upper(name);
        // The semicolon is reserved for the generated ISO file-version
        // field. A literal semicolon in a Joliet name needs an ISO alias,
        // otherwise a directory like "folder;1" loses part of its name
        // when an ISO reader interprets it as a version suffix.
        for(const unsigned char c:text)
            need((c>='A'&&c<='Z')||(c>='0'&&c<='9')||
                 c=='_'||c=='.',
                 "primary ISO identifier contains nonportable characters");
        if(!dir)text+=";1";
        need(text.size()<=221,"ISO identifier exceeds directory-record limit");
        b.assign(text.begin(),text.end());
    }
    need(!b.empty() && (33+b.size()+(b.size()%2==0?1u:0))<=255,
         "ISO/Joliet directory record exceeds 255 bytes");
    return b;
}
// Deterministic collision-resistant aliases for Unicode names unrepresentable
// in primary ISO. No unstable parent-child array indexes, no truncation that
// can collapse two distinct filenames.
std::string iso_alias(const std::string& name,bool directory,unsigned salt) {
    std::uint64_t hash=14695981039346656037ull;
    for(const unsigned char c:name) {
        hash^=c;hash*=1099511628211ull;
    }
    hash^=directory?0x44u:0x46u;
    hash*=1099511628211ull;
    hash^=salt;hash*=1099511628211ull;
    static constexpr char digits[]="0123456789ABCDEF";
    std::string out="WCX_";
    for(unsigned i=0;i<16;++i)
        out+=digits[(hash>>(4*(15-i)))&15u];
    if(!directory) out+=".BIN";
    return out;
}
void protect_boot(const Node& n,const std::set<std::uint32_t>& boot) {
    // El Torito image/catalog locations can be contained inside an extent
    // whose first sector precedes the referenced LBA. Protect the entire
    // original file extent, not only a matching first-sector address.
    each_extent(n,[&](std::uint64_t at,std::uint64_t length) {
        const auto sectors=(length+block-1)/block;
        for(const auto lba:boot)
            need(!(lba>=at/block && std::uint64_t(lba)-at/block<sectors),
                 "editing an extent containing an El Torito boot image/catalog is not supported");
    });
    for(const auto& child:n.children) protect_boot(*child,boot);
}
// Existing SUSP bytes, including CE pointers, stay in the original
// prefix. Only inline directory PX link counts change with topology.
// A continuation containing an unknown PX location cannot be rewritten
// in place: refuse that topology change instead of corrupting metadata.
std::size_t rr_subdirs(const Node& n) {
    return static_cast<std::size_t>(std::count_if(n.children.begin(),n.children.end(),
        [](const auto& child){return child->directory;}));
}
Bytes rr_px(const Node& directory) {
    const auto& dot=directory.rr_dot;
    if(dot.size()>=34) {
        for(std::size_t at=34;at+4<=dot.size() && dot[at];) {
            const auto bytes=std::size_t(dot[at+2]);
            need(bytes>=4 && bytes<=dot.size()-at,"invalid Rock Ridge dot metadata");
            if(dot[at]=='P' && dot[at+1]=='X')
                return Bytes(dot.begin()+static_cast<std::ptrdiff_t>(at),
                             dot.begin()+static_cast<std::ptrdiff_t>(at+bytes));
            if(dot[at]=='S' && dot[at+1]=='T')break;
            at+=bytes;
        }
    }
    Bytes px(36,0);
    px[0]='P';px[1]='X';px[2]=36;px[3]=1;
    both(px,4,directory.directory?0040755u:0100644u);
    both(px,12,directory.directory?2u:1u);
    return px;
}
void rr_link_count(Bytes& r,const Node& directory,bool generated) {
    const auto count=rr_subdirs(directory);
    if(!generated && count==directory.old_subdirs)return;
    const auto id=std::size_t(r[32]);
    auto at=33+id+(id%2==0?1u:0u);
    bool px=false,ce=false;
    for(;at+4<=r.size() && r[at];) {
        const auto bytes=std::size_t(r[at+2]);
        need(bytes>=4 && bytes<=r.size()-at,"invalid Rock Ridge PX record");
        if(r[at]=='P' && r[at+1]=='X') {
            need(!px && (bytes==36 || bytes==44),"invalid Rock Ridge PX link count");
            both(r,at+12,static_cast<std::uint32_t>(2+count));
            px=true;
        }
        if(r[at]=='C' && r[at+1]=='E')ce=true;
        if(r[at]=='S' && r[at+1]=='T')break;
        at+=bytes;
    }
    need(px || !ce,"Rock Ridge directory link count in continuation requires relocation");
}
Bytes record(const Node& n,const Bytes& id,unsigned view,const Node* directory=nullptr,
             const FileSection* section=nullptr,bool more=false,
             const Bytes* preserved_rr=nullptr,bool rr_dot_record=false) {
    Bytes b(33+id.size()+(id.size()%2==0?1:0),0);need(b.size()<=255,"ISO record too long");
    std::copy_n(n.record.begin(),std::min<std::size_t>(33,n.record.size()),b.begin());b[0]=static_cast<unsigned char>(b.size());b[32]=static_cast<unsigned char>(id.size());
    const auto& loc=directory?*directory:n;
    need(loc.directory || section || loc.size<=0xffffffffu,
         "ISO file needs explicit multi-extent sections");
    both(b,2,loc.directory?loc.new_lba[view]:section?section->lba:loc.lba);
    both(b,10,loc.directory?loc.new_size[view]:section?section->length:static_cast<std::uint32_t>(loc.size));
    b[25]=static_cast<unsigned char>((n.record.empty()?0:n.record[25])&1u);
    if(loc.directory)b[25]|=2;
    if(more)b[25]|=0x80u;
    both(b,28,1,2);std::copy(id.begin(),id.end(),b.begin()+33);
    if(view==0 && preserved_rr) {
        const auto base=b.size();
        const bool generated=preserved_rr->empty();
        if(!generated) {
            need(preserved_rr->size()>=base &&
                 (*preserved_rr)[32]==id.size() &&
                 std::equal(id.begin(),id.end(),preserved_rr->begin()+33),
                 "Rock Ridge original identifier changed");
            b.insert(b.end(),preserved_rr->begin()+static_cast<std::ptrdiff_t>(base),
                     preserved_rr->end());
        } else {
            auto px=rr_px(loc);
            b.insert(b.end(),px.begin(),px.end());
            if(!rr_dot_record) {
                const auto& name=n.logical;
                (void)utf16(name); // validate UTF-8 before encoding RRIP NM
                need(name.size()<=250 && b.size()+5+name.size()<=255,
                     "Rock Ridge new filename requires a continuation area");
                b.push_back('N');b.push_back('M');
                b.push_back(static_cast<unsigned char>(5+name.size()));
                b.push_back(1);b.push_back(0);
                b.insert(b.end(),name.begin(),name.end());
            }
        }
        need(b.size()<=255,"Rock Ridge directory record exceeds 255 bytes");
        b[0]=static_cast<unsigned char>(b.size());
        if(loc.directory)rr_link_count(b,loc,generated);
    }
    return b;
}
Bytes directory_data(Node& n,unsigned view) {
    Bytes b;
    auto append=[&](const Bytes& r){if(b.size()%block+r.size()>block)b.resize((b.size()/block+1)*block);b.insert(b.end(),r.begin(),r.end());};
    append(record(n,Bytes{0},view,&n,nullptr,false,
                  n.rr?&n.rr_dot:nullptr,true));
    append(record(n,Bytes{1},view,n.parent?n.parent:&n,nullptr,false,
                  n.rr?&n.rr_parent:nullptr,true));
    std::vector<Node*> sorted;for(auto& x:n.children)sorted.push_back(x.get());std::sort(sorted.begin(), sorted.end(), [&](const Node* a, const Node* c) {
        // Avoid the GCC 13/14 -Wstringop-overread false positive in the
        // optimized libstdc++ vector three-way comparison (with -Werror).
        const Bytes& lhs = view ? a->joliet_identifier : a->identifier;
        const Bytes& rhs = view ? c->joliet_identifier : c->identifier;
        const auto count = std::min(lhs.size(), rhs.size());
        for (std::size_t i = 0; i < count; ++i) {
            if (lhs[i] != rhs[i]) return lhs[i] < rhs[i];
        }
        return lhs.size() < rhs.size();
    });
    for(auto* x:sorted) {
        const auto& id=view?x->joliet_identifier:x->identifier;
        const auto* rr=x->rr?&x->rr_record:nullptr;
        if(x->directory || x->sections.empty())
            append(record(*x,id,view,nullptr,nullptr,false,rr));
        else for(std::size_t part=0;part<x->sections.size();++part)
            append(record(*x,id,view,nullptr,&x->sections[part],
                          part+1<x->sections.size(),rr));
    }
    b.resize((b.size()+block-1)/block*block);return b;
}
void write(std::fstream& out,const Bytes& b){out.write(reinterpret_cast<const char*>(b.data()),static_cast<std::streamsize>(b.size()));if(!out)throw std::runtime_error("ISO output write failed");}
void copy_bytes(std::istream& in,std::fstream& out,std::uint64_t size,const EditProgress& progress) {
    Bytes b(1024*1024);while(size){edit_tick(progress);auto count=std::min<std::uint64_t>(size,b.size());in.read(reinterpret_cast<char*>(b.data()),static_cast<std::streamsize>(count));need(bool(in),"source shortened during ISO edit");out.write(reinterpret_cast<const char*>(b.data()),static_cast<std::streamsize>(count));if(!out)throw std::runtime_error("ISO output write failed");size-=count;}
}

struct BridgePatch {
    std::string path;
    std::filesystem::path source;
    std::uint64_t at{};
    std::uint64_t bytes{};
};

bool intersect(std::uint64_t a,std::uint64_t a_bytes,
               std::uint64_t b,std::uint64_t b_bytes) noexcept {
    // Callers validate that both bounded extents fit the image file.
    return a_bytes && b_bytes && a<b+b_bytes && b<a+a_bytes;
}

// A non-UDF ISO with a real MBR cannot use the append/rebuild path without
// risking a desynchronized partition/boot map. Only equal-length edits to
// already named, non-shared ISO file extents are allowed here.
void edit_iso_hybrid_equal_length(
    const std::filesystem::path& path,Image& image,
    const std::vector<ArchiveChange>& changes,const EditProgress& progress) {
    need(image.hybrid_system_area && !image.udf_bridge,
         "hybrid replacement requires a non-UDF partitioned ISO image");
    need(image.boot_regions_bounded,
         "hybrid ISO has unbounded El Torito boot resources");
    need(!changes.empty(),"hybrid ISO has no replacement files");
    // A Rock Ridge WCX path is an RRIP NM name, not necessarily the ISO
    // primary alias (nor the Joliet name). Resolve that path through the
    // independent reader and match its exactly bounded physical extent
    // against a single parsed primary ISO file. All RRIP/PX/NM/CE bytes
    // remain immutable; F5 can replace existing same-size file content,
    // NEVER create/delete an RRIP directory record in this route.
    const Disc rr_disc(path);
    const bool rock_ridge=rr_disc.format()=="Rock Ridge";
    need(!image.iso_system_use || rock_ridge,
         "hybrid ISO has unrecognized SUSP metadata; only recognized "
         "Rock Ridge supports verified same-size replacement");
    std::vector<const Node*> nodes{image.root.get()};
    for(std::size_t i=0;i<nodes.size();++i) {
        for(const auto& child:nodes[i]->children)nodes.push_back(child.get());
        need(nodes.size()<=200000,"hybrid ISO has too many directory entries");
    }
    std::vector<BridgePatch> patches;
    for(const auto& change:changes) {
        edit_tick(progress);
        need(!change.remove && !change.directory,
             "hybrid ISO supports only equal-length file replacement");
        const auto key=edit_path(change.path);
        const Node* node=nullptr;
        if(rock_ridge) {
            const Item* selected=nullptr;
            for(const auto& item:rr_disc.items()) {
                if(item.path!=key)continue;
                need(!selected,"ambiguous Rock Ridge file path");
                selected=&item;
            }
            need(selected && !selected->directory && selected->size>0 &&
                 selected->inline_data.empty() &&
                 selected->direct_source.empty() &&
                 selected->extents.size()==1 &&
                 !selected->extents[0].sparse &&
                 selected->extents[0].offset%block==0 &&
                 selected->extents[0].length==selected->size,
                 "Rock Ridge replacement requires a single physical "
                 "extent for an existing regular file");
            std::size_t exact_matches=0;
            for(const Node* candidate:nodes) {
                if(candidate->directory || !candidate->size)continue;
                if(std::uint64_t(candidate->lba)*block==
                       selected->extents[0].offset &&
                   candidate->size==selected->size) {
                    node=candidate;
                    ++exact_matches;
                }
            }
            need(exact_matches==1,
                 "Rock Ridge file extent is missing or ambiguously "
                 "shared in the primary ISO namespace");
        } else {
            node=image.root.get();
            std::istringstream parts(key);
            std::string component;
            while(node && std::getline(parts,component,'/')) {
                const auto it=std::find_if(
                    node->children.begin(),node->children.end(),
                    [&](const auto& child){
                        return child->logical==component;
                    });
                node=it==node->children.end()?nullptr:it->get();
            }
        }
        need(node && !node->directory && node->size>0,
             "hybrid replacement target must be an existing nonempty file");
        need(std::filesystem::is_regular_file(
                 std::filesystem::symlink_status(change.source)),
             "hybrid replacement must be a regular non-symlink file");
        need(std::filesystem::file_size(change.source)==node->size,
             "hybrid replacement must have exactly the original file size");
        const auto offset=std::uint64_t(node->lba)*block;
        const auto bytes=std::uint64_t(node->size);
        const auto volume=std::uint64_t(image.declared_volume_blocks)*block;
        need(offset>=16ull*block && offset<=volume && bytes<=volume-offset,
             "hybrid ISO target lies outside the declared volume");
        protect_boot(*node,image.boot_lbas);
        for(const auto& [start,length]:image.hybrid_protected_regions)
            need(!intersect(offset,bytes,start,length),
                 "hybrid replacement overlaps a protected MBR/GPT partition or table");
        for(const auto& [start,length]:image.iso_auxiliary_regions)
            need(!intersect(offset,bytes,start,length),
                 "hybrid replacement overlaps SUSP continuation metadata");
        for(const auto& [start,length]:image.joliet_regions)
            need((offset==start && bytes==length) ||
                 !intersect(offset,bytes,start,length),
                 "hybrid replacement overlaps an independent Joliet extent");
        for(const auto& [start,length]:image.boot_regions)
            need(!intersect(offset,bytes,start,length),
                 "hybrid replacement overlaps an El Torito resource");
        for(const auto lba:image.boot_descriptor_lbas)
            need(!intersect(offset,bytes,std::uint64_t(lba)*block,block),
                 "hybrid replacement overlaps a boot descriptor");
        for(const auto& [lba,descriptor]:image.descriptors) {
            need(!intersect(offset,bytes,std::uint64_t(lba)*block,block),
                 "hybrid replacement overlaps an ISO volume descriptor");
            const auto table=le(descriptor,132);
            if(!table)continue;
            // ECMA-119 stores L path-table locations little-endian at
            // 140/144, but M locations big-endian at 148/152. Reading
            // the M table as LE can invent a huge LBA and falsely reject
            // a valid hybrid image.
            const auto be32=[&](std::size_t at) {
                need(at+4<=descriptor.size(),"truncated ISO path-table LBA");
                return (std::uint32_t(descriptor[at])<<24) |
                       (std::uint32_t(descriptor[at+1])<<16) |
                       (std::uint32_t(descriptor[at+2])<<8) |
                        std::uint32_t(descriptor[at+3]);
            };
            need(le(descriptor,140) && be32(148),
                 "hybrid ISO has missing path-table locations");
            const auto padded=(std::uint64_t(table)+block-1)/block*block;
            for(const std::size_t at:{140u,144u,148u,152u}) {
                const auto address=(at==140u || at==144u)?
                    le(descriptor,at):be32(at);
                if(!address)continue;
                const auto begin=std::uint64_t(address)*block;
                need(begin<=image.size && padded<=image.size-begin,
                     "hybrid ISO path table exceeds image size");
                need(!intersect(offset,bytes,begin,padded),
                     "hybrid replacement overlaps a path table");
            }
        }
        for(std::uint32_t lba=16;lba<64;++lba) {
            const auto descriptor=image.read(std::uint64_t(lba)*block,7);
            if(descriptor[0]==255) {
                need(!intersect(offset,bytes,std::uint64_t(lba)*block,block),
                     "hybrid replacement overlaps ISO terminator");
                break;
            }
        }
        for(const Node* other:nodes) {
            if(other==node || !other->size)continue;
            need(!intersect(offset,bytes,
                            std::uint64_t(other->lba)*block,other->size),
                 "hybrid replacement overlaps another ISO entry or alias");
        }
        for(const auto& previous:patches)
            need(!intersect(offset,bytes,previous.at,previous.bytes),
                 "hybrid batch contains overlapping replacements");
        patches.push_back({key,change.source,offset,bytes});
    }
    std::sort(patches.begin(),patches.end(),
        [](const auto& lhs,const auto& rhs){return lhs.at<rhs.at;});
    EditTransaction tx(path);
    std::fstream out(tx.output,std::ios::in|std::ios::out|
                     std::ios::binary|std::ios::trunc);
    need(bool(out),"cannot stage hybrid ISO");
    image.file.clear();image.file.seekg(0);
    copy_bytes(image.file,out,image.size,progress);
    std::vector<char> data(256u*1024u);
    for(const auto& patch:patches) {
        out.clear();out.seekp(static_cast<std::streamoff>(patch.at));
        need(bool(out),"cannot seek staged hybrid ISO");
        std::ifstream source(patch.source,std::ios::binary);
        need(bool(source),"cannot open hybrid replacement source");
        for(auto left=patch.bytes;left;) {
            edit_tick(progress);
            const auto n=static_cast<std::streamsize>(
                std::min<std::uint64_t>(left,data.size()));
            source.read(data.data(),n);
            need(source.gcount()==n,"hybrid source truncated during F5");
            out.write(data.data(),n);
            need(bool(out),"hybrid payload write failed");
            left-=static_cast<std::uint64_t>(n);
        }
        need(source.peek()==std::char_traits<char>::eof(),
             "hybrid replacement source grew during F5");
    }
    out.flush();need(bool(out),"cannot flush staged hybrid ISO");
    out.close();image.file.close();
    {
        Image verified(tx.output,true);
        need(verified.hybrid_system_area && !verified.udf_bridge &&
             verified.gpt_hybrid==image.gpt_hybrid &&
             verified.hybrid_protected_regions==image.hybrid_protected_regions &&
             verified.size==image.size &&
             verified.descriptors==image.descriptors &&
             verified.boot_descriptor_lbas==image.boot_descriptor_lbas &&
             verified.boot_lbas==image.boot_lbas &&
             verified.boot_entries==image.boot_entries &&
             verified.boot_regions==image.boot_regions,
             "staged hybrid ISO metadata or El Torito references changed");
        if(rock_ridge) {
            const Disc independent(tx.output);
            need(independent.format()=="Rock Ridge" &&
                 independent.items().size()==rr_disc.items().size(),
                 "staged hybrid ISO lost Rock Ridge file structure");
            for(const auto& patch:patches) {
                auto it=std::find_if(independent.items().begin(),
                    independent.items().end(),[&](const Item& entry) {
                        return entry.path==patch.path && !entry.directory;
                    });
                need(it!=independent.items().end() &&
                     it->size==patch.bytes &&
                     it->extents.size()==1 &&
                     !it->extents[0].sparse &&
                     it->extents[0].offset==patch.at,
                     "Rock Ridge staged extent or namespace changed");
            }
        }
    }
    // Verify every byte outside the requested file extents, including MBR,
    // all ISO/Joliet metadata, hidden vendor sectors, boot data and padding.
    OpticalEditGuard unchanged(image.size);
    std::uint64_t end=0;
    for(const auto& patch:patches) {
        need(end<=patch.at,"overlapping staged hybrid file extents");
        unchanged.protect(end,patch.at-end);
        end=patch.at+patch.bytes;
    }
    unchanged.protect(end,image.size-end);
    unchanged.verify(path,tx.output,progress);
    // Close both verification streams before MoveFileExW. An open staged
    // stream can deny FILE_SHARE_DELETE and prevent the atomic rename.
    {
    std::ifstream staged(tx.output,std::ios::binary);
    need(bool(staged),"cannot read staged hybrid ISO");
    std::vector<char> first(data.size()),second(data.size());
    for(const auto& patch:patches) {
        std::ifstream input(patch.source,std::ios::binary);
        need(bool(input),"cannot reopen hybrid F5 source");
        staged.clear();staged.seekg(static_cast<std::streamoff>(patch.at));
        need(bool(staged),"cannot locate hybrid file after staging");
        for(auto left=patch.bytes;left;) {
            edit_tick(progress);
            const auto n=static_cast<std::streamsize>(
                std::min<std::uint64_t>(left,first.size()));
            input.read(first.data(),n);
            staged.read(second.data(),n);
            need(input.gcount()==n && staged.gcount()==n &&
                 std::memcmp(first.data(),second.data(),
                             static_cast<std::size_t>(n))==0,
                 "staged hybrid file differs from its F5 source");
            left-=static_cast<std::uint64_t>(n);
        }
    }
    }
    tx.commit(progress);
}

void edit_iso_udf_bridge_equal_length(
    const std::filesystem::path& path,Image& iso,
    const std::vector<ArchiveChange>& changes,const EditProgress& progress) {
    need(iso.udf_bridge,"not an ISO/UDF bridge");
    // Payload-only edits are allowed in bootable bridges only if every
    // El Torito boot entry has a bounded data span. We do not regenerate or
    // relocate the catalog, boot images or any UDF descriptors.
    need(iso.boot_regions_bounded,
         "El Torito boot image has no bounded sector count; editing refused");
    const Disc udf(path);
    need(udf.format()=="UDF","UDF filesystem could not be validated");
    std::map<std::string,const Item*> udf_files;
    for(const auto& item:udf.items())
        if(!item.directory)
            need(udf_files.emplace(item.path,&item).second,
                 "duplicate UDF bridge path");

    std::vector<Node*> iso_nodes{iso.root.get()};
    for(std::size_t i=0;i<iso_nodes.size();++i)
        for(const auto& child:iso_nodes[i]->children)
            iso_nodes.push_back(child.get());

    std::vector<BridgePatch> patches;
    patches.reserve(changes.size());
    for(const auto& change:changes) {
        edit_tick(progress);
        need(!change.remove && !change.directory,
             "ISO/UDF Bridge supports equal-length replacement only");
        const auto key=edit_path(change.path);
        Node* node=iso.root.get();
        std::istringstream parts(key);
        std::string component;
        while(node && std::getline(parts,component,'/')) {
            auto it=std::find_if(node->children.begin(),node->children.end(),
                [&](const auto& candidate){return candidate->logical==component;});
            node=it==node->children.end()?nullptr:it->get();
        }
        need(!node || (!node->directory && node->size>0),
             "bridge target maps to an ISO directory");
        const auto it=udf_files.find(key);
        need(it!=udf_files.end(),"file missing from UDF bridge namespace");
        const auto& entry=*it->second;
        need(entry.size>0 && entry.inline_data.empty() &&
             entry.direct_source.empty() &&
             entry.extents.size()==1 && !entry.extents[0].sparse,
             "bridge payload requires a single nonembedded, nonsparse UDF extent");
        const auto offset=entry.extents[0].offset;
        need(offset>=257ull*block && entry.extents[0].length==entry.size &&
             offset<=iso.size && entry.size<=iso.size-offset,
             "UDF file extent is not a bounded standalone payload");
        if(node) {
            need(std::uint64_t(node->lba)*block==offset &&
                 node->size==entry.size,
                 "ISO and UDF aliases disagree about file extent");
            protect_boot(*node,iso.boot_lbas);
        }
        need(std::filesystem::is_regular_file(
                 std::filesystem::symlink_status(change.source)),
             "replacement must be a regular file, not a link");
        need(std::filesystem::file_size(change.source)==entry.size,
             "bridge replacement must have exactly the original file size");
        // A payload-only edit must not overlap any El Torito boot range,
        // descriptor, ISO9660 directory, or UDF VDS/anchor.
        need(offset>=256ull*block,
             "replacement overlaps ISO/VRS/reserved optical descriptors");
        for(const auto& [start,length]:iso.boot_regions)
            need(!intersect(offset,entry.size,start,length),
                 "replacement intersects El Torito boot image or catalog");
        for(const auto& [start,length]:iso.udf_metadata_regions)
            need(!intersect(offset,entry.size,start,length),
                 "replacement intersects UDF anchor or descriptor sequence");
        for(const auto& [start,length]:iso.hybrid_protected_regions)
            need(!intersect(offset,entry.size,start,length),
                 "bridge replacement intersects protected MBR/GPT metadata or partition");
        for(const auto& [start,length]:iso.iso_auxiliary_regions)
            need(!intersect(offset,entry.size,start,length),
                 "bridge replacement intersects ISO path table or SUSP metadata");
        for(const auto& [start,length]:iso.joliet_regions)
            need((offset==start && entry.size==length) ||
                 !intersect(offset,entry.size,start,length),
                 "bridge replacement intersects independent Joliet extent");
        for(const auto* other:iso_nodes) {
            if(other==node || other->size==0)continue;
            need(!intersect(offset,entry.size,
                            std::uint64_t(other->lba)*block,other->size),
                 "bridge file overlaps ISO directory, boot image or other entry");
        }
        for(const auto& other:udf.items()) {
            if(other.path==key)continue;
            for(const auto& extent:other.extents)
                if(!extent.sparse)
                    need(!intersect(offset,entry.size,
                                    extent.offset,extent.length),
                         "bridge file shares an extent with another UDF entry");
        }
        for(const auto& previous:patches)
            need(!intersect(offset,entry.size,previous.at,previous.bytes),
                 "overlapping or repeated bridge file replacements");
        patches.push_back({key,change.source,offset,entry.size});
    }
    need(!patches.empty(),"empty ISO/UDF Bridge replacement list");

    // Full sibling copy and byte-restricted patches: no UDF/ISO metadata,
    // file extents or boot sectors are modified by this operation.
    EditTransaction tx(path);
    std::fstream out(tx.output,std::ios::in|std::ios::out|
                     std::ios::binary|std::ios::trunc);
    need(bool(out),"cannot create staged bridge ISO");
    iso.file.clear();
    iso.file.seekg(0);
    copy_bytes(iso.file,out,iso.size,progress);
    Bytes chunk(1024*1024);
    for(const auto& item:patches) {
        edit_tick(progress);
        out.clear();
        out.seekp(static_cast<std::streamoff>(item.at),std::ios::beg);
        need(bool(out),"cannot seek staged bridge extent");
        std::ifstream source(item.source,std::ios::binary);
        need(bool(source),"cannot open bridge replacement data");
        auto remaining=item.bytes;
        while(remaining) {
            edit_tick(progress);
            const auto n=static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining,chunk.size()));
            source.read(reinterpret_cast<char*>(chunk.data()),
                        static_cast<std::streamsize>(n));
            need(bool(source),"replacement file shortened during bridge edit");
            out.write(reinterpret_cast<const char*>(chunk.data()),
                      static_cast<std::streamsize>(n));
            need(bool(out),"cannot write replacement UDF bridge data");
            remaining-=n;
        }
    }
    out.flush();
    need(bool(out),"staged bridge flush failed");
    out.close();
    iso.file.close();

    // Independently reparse both namespaces in the staged image and
    // compare every requested replacement, byte for byte.
    {
        Image primary(tx.output,true);
        need(primary.udf_bridge && primary.size==iso.size &&
             primary.gpt_hybrid==iso.gpt_hybrid &&
             primary.hybrid_protected_regions==iso.hybrid_protected_regions,
             "staged image lost its ISO/UDF bridge or partition descriptors");
        const Disc checked(tx.output);
        need(checked.format()=="UDF","staged UDF could not be validated");
        std::ifstream staged(tx.output,std::ios::binary);
        for(const auto& item:patches) {
            const auto v=std::find_if(checked.items().begin(),checked.items().end(),
                [&](const Item& entry) {
                    return !entry.directory && entry.path==item.path;
                });
            need(v!=checked.items().end() && v->size==item.bytes &&
                 v->inline_data.empty() && v->extents.size()==1 &&
                 v->extents[0].offset==item.at,
                 "staged UDF bridge file map changed");
            std::ifstream input(item.source,std::ios::binary);
            need(bool(input),"cannot reopen bridge verification source");
            staged.clear();
            staged.seekg(static_cast<std::streamoff>(item.at));
            auto remain=item.bytes;
            Bytes from_file(chunk.size()),from_stage(chunk.size());
            while(remain) {
                edit_tick(progress);
                const auto n=static_cast<std::size_t>(
                    std::min<std::uint64_t>(remain,chunk.size()));
                input.read(reinterpret_cast<char*>(from_file.data()),
                           static_cast<std::streamsize>(n));
                staged.read(reinterpret_cast<char*>(from_stage.data()),
                            static_cast<std::streamsize>(n));
                need(bool(input) && bool(staged) &&
                     std::equal(from_file.begin(),
                                from_file.begin()+static_cast<std::ptrdiff_t>(n),
                                from_stage.begin()),
                     "ISO/UDF staged data differs from replacement source");
                remain-=n;
            }
        }
    }
    // Independently compare all bytes outside the replacement payloads.
    // This includes GPT backups beyond the declared ISO volume.
    std::sort(patches.begin(),patches.end(),[](const auto& a,const auto& b){
        return a.at<b.at;
    });
    OpticalEditGuard unchanged(iso.size);
    std::uint64_t end=0;
    for(const auto& patch:patches) {
        need(end<=patch.at,"overlapping bridge patches");
        unchanged.protect(end,patch.at-end);
        end=patch.at+patch.bytes;
    }
    unchanged.protect(end,iso.size-end);
    unchanged.verify(path,tx.output,progress);
    tx.commit(progress);
}
// Native Universal Optical Edit Guard.  ISO9660/Joliet and boot data are
// immutable while a UDF Type-1 writer modifies FIDs/FE/bitmap/LVID.
// A single staged-image validator handles create, delete and resize.
void protect_iso_udf_stage(const std::filesystem::path& original,
                           const std::filesystem::path& staged,
                           const Image& before,const EditProgress& progress) {
    need(before.udf_bridge && before.boot_regions_bounded,
         "bootable ISO/UDF validation needs bounded boot extents");
    const auto stage_size=std::filesystem::file_size(staged);
    const bool mastering=stage_size>before.size;
    need(std::filesystem::file_size(original)==before.size &&
         stage_size>=before.size,
         "ISO/UDF image size changed during UDF edit");
    Image after(staged,true);
    need(after.udf_bridge && after.boot_regions_bounded &&
         after.size==stage_size &&
         after.descriptors==before.descriptors &&
         after.boot_regions==before.boot_regions &&
         after.boot_lbas==before.boot_lbas,
         "ISO/Joliet or El Torito boot descriptors changed in UDF staging");

    // Compare all visible ISO namespace metadata before bytes are committed.
    // UDF-only files are intentionally absent from these trees.
    auto tree_same=[&](auto&& self,const Node& first,const Node& second)->bool {
        if(first.name!=second.name || first.logical!=second.logical ||
           first.identifier!=second.identifier ||
           first.joliet_identifier!=second.joliet_identifier ||
           first.directory!=second.directory ||
           first.lba!=second.lba || first.size!=second.size ||
           first.sections!=second.sections ||
           first.record!=second.record ||
           first.children.size()!=second.children.size())
            return false;
        for(std::size_t i=0;i<first.children.size();++i)
            if(!self(self,*first.children[i],*second.children[i]))
                return false;
        return true;
    };
    need(tree_same(tree_same,*before.root,*after.root),
         "ISO9660 namespace changed during UDF-only editing");
    OpticalEditGuard guard(before.size);
    guard.protect(0,256ull*block); // ISO PVD, VRS, boot/system area
    for(const auto& [start,length]:before.boot_regions)
        guard.protect(start,length);
    for(const auto& [start,length]:before.udf_metadata_regions) {
        // Only anchor sectors may change during append mastering. The old
        // main/reserve descriptor sequences remain byte-for-byte intact.
        if(mastering && length==block &&
           before.udf_anchor_lbas.count(start/block))continue;
        guard.protect(start,length);
    }
    if(mastering) {
        // A previous physical-end anchor is no longer a discovery
        // candidate after another append. Verify it directly at its
        // original LBA rather than requiring the parser to rediscover it.
        const auto primary=after.read(256ull*block,block);
        auto verify_anchor=[&](std::uint64_t lba) {
            const auto anchor=after.read(lba*block,block);
            unsigned checksum=0;
            for(std::size_t p=0;p<16;++p)if(p!=4)checksum+=anchor[p];
            need(le(anchor,0,2)==2 && le(anchor,12)==lba &&
                 static_cast<unsigned char>(checksum)==anchor[4] &&
                 std::equal(anchor.begin()+16,anchor.end(),primary.begin()+16) &&
                 le(anchor,8,2)==le(primary,8,2) &&
                 le(anchor,10,2)==le(primary,10,2),
                 "UDF mastering anchor copies disagree");
        };
        for(const auto at:before.udf_anchor_lbas)verify_anchor(at);
        verify_anchor(stage_size/block-1);
        verify_anchor(stage_size/block-257);
        need(after.udf_anchor_lbas.count(stage_size/block-1)!=0 &&
             after.udf_anchor_lbas.count(stage_size/block-257)!=0,
             "UDF mastering lost new end anchors");
    }
    for(const auto& [start,length]:before.joliet_regions)
        guard.protect(start,length);
    for(const auto& [start,length]:before.iso_auxiliary_regions)
        guard.protect(start,length);
    std::vector<const Node*> nodes{before.root.get()};
    for(std::size_t at=0;at<nodes.size();++at) {
        const auto* node=nodes[at];
        if(node->size)
            guard.protect(std::uint64_t(node->lba)*block,node->size);
        for(const auto& child:node->children)
            nodes.push_back(child.get());
        need(nodes.size()<=200000,"ISO namespace exceeds validation limit");
    }
    // The common verifier coalesces protected spans and checks every byte
    // after staged UDF metadata/payload verification, before atomic commit.
    if(mastering)guard.verify_appended(original,staged,progress);
    else guard.verify(original,staged,progress);
}
// Sector-local ISO directory deletion. ECMA-119 treats a zero-length
// record as padding to the NEXT sector, not a deleted-entry marker.
// Compact surviving records within each affected sector, never across
// a sector boundary. The directory LBAs, sizes and path tables stay fixed.
struct SharedIsoDeleteSector {
    Bytes original;
    std::vector<std::pair<std::size_t,std::size_t>> removed;
};
using SharedIsoDeleteSectors=std::map<std::uint64_t,SharedIsoDeleteSector>;

void plan_shared_iso_delete(Image& image,const Node& node,bool joliet,
                            SharedIsoDeleteSectors& sectors) {
    const auto* parent=node.parent;
    need(parent && parent->directory,"shared ISO deletion lacks parent directory");
    const auto& record=joliet?node.joliet_record:node.record;
    const auto& directory=joliet?parent->joliet_record:parent->record;
    const auto at=joliet?node.joliet_record_at:node.record_at;
    need(directory.size()>=34 && record.size()>=34 &&
         record[0]==record.size(),"invalid shared ISO delete record");
    const auto start=std::uint64_t(le(directory,2))*block;
    const auto length=std::uint64_t(le(directory,10));
    need(at>=start && at-start<length &&
         record.size()<=length-(at-start),"shared ISO record outside parent");
    const auto sector=at/block*block;
    const auto span=static_cast<std::size_t>(
        std::min<std::uint64_t>(block,length-(sector-start)));
    const auto offset=static_cast<std::size_t>(at-sector);
    need(offset<=span && record.size()<=span-offset,
         "shared ISO delete record crosses a sector");
    auto& patch=sectors[sector];
    if(patch.original.empty())patch.original=image.read(sector,span);
    need(patch.original.size()==span &&
         std::equal(record.begin(),record.end(),patch.original.begin()+
                    static_cast<std::ptrdiff_t>(offset)),
         "shared ISO delete record differs from parent sector");
    // Confirm this is an actual record boundary, and that sector padding
    // contains no hidden vendor metadata that compaction would move.
    bool matched=false;
    std::size_t p=0;
    while(p<span && patch.original[p]) {
        const auto n=std::size_t(patch.original[p]);
        need(n>=34 && n<=span-p,"invalid ISO directory sector");
        if(p==offset)matched=n==record.size();
        p+=n;
    }
    need(matched && std::all_of(patch.original.begin()+
             static_cast<std::ptrdiff_t>(p),patch.original.end(),
             [](auto c){return c==0;}),"invalid or nonzero ISO directory padding");
    for(const auto& [begin,bytes]:patch.removed)
        need(!intersect(begin,bytes,offset,record.size()),
             "duplicate or overlapping shared ISO deletion record");
    patch.removed.emplace_back(offset,record.size());
}

// UDF verifies its replacement first. Verify the unmodified ISO view,
// then retarget its exact existing file records, not its directory tree.
void coordinate_shared_bridge_stage(
        const std::filesystem::path& path,const std::filesystem::path& staged,
        const Image& image,const std::map<std::string,const Node*>& targets,
        const CoordinatedIsoUpdate& plan,
        const SharedIsoDeleteSectors& deletion_sectors,
        const EditProgress& progress) {
    protect_iso_udf_stage(path,staged,image,progress);
    const auto size=std::filesystem::file_size(staged);
    need(size>image.size && size%block==0 && size/block<0xffffffffu,
         "shared ISO/UDF resize did not produce bounded append mastering");
    std::map<const Node*,FileSection> sections;
    std::set<const Node*> deleted;
    {
        const Disc udf(staged);
        need(udf.format()=="UDF","cannot read staged coordinated UDF");
        for(const auto& [key,node]:targets) {
            const auto item=std::find_if(udf.items().begin(),udf.items().end(),
                [&](const Item& v){return v.path==key && !v.directory;});
            if(plan.shared_deletions.count(key)) {
                need(item==udf.items().end(),"shared deleted file is still in UDF");
                deleted.insert(node);
                continue;
            }
            need(item!=udf.items().end() && item->size>0 &&
                 item->size<=0xffffffffull && item->inline_data.empty() &&
                 !item->extents.empty(),
                 "staged shared UDF file lacks an ISO-compatible extent");
            const auto start=item->extents.front().offset;
            need(start>=image.size && start%block==0 &&
                 start<=size && item->size<=size-start,
                 "shared replacement must occupy newly appended blocks");
            auto next=start;
            for(const auto& extent:item->extents) {
                need(!extent.sparse && extent.offset==next &&
                     extent.length<=start+item->size-next,
                     "shared replacement is not contiguous");
                next+=extent.length;
            }
            need(next-start==item->size,"shared replacement size mismatch");
            sections.emplace(node,FileSection{
                static_cast<std::uint32_t>(start/block),
                static_cast<std::uint32_t>(item->size)});
        }
    }
    std::fstream out(staged,std::ios::binary|std::ios::in|std::ios::out);
    need(bool(out),"cannot open coordinated ISO stage");
    auto patch=[&](std::uint64_t at,Bytes old,Bytes replacement) {
        edit_tick(progress);
        need(old.size()==replacement.size() &&
             std::find(plan.metadata_writes.begin(),plan.metadata_writes.end(),
                std::pair<std::uint64_t,std::uint64_t>{at,old.size()})!=
                     plan.metadata_writes.end(),"unplanned shared ISO metadata write");
        Bytes actual(old.size());
        out.clear();out.seekg(static_cast<std::streamoff>(at));
        out.read(reinterpret_cast<char*>(actual.data()),
                 static_cast<std::streamsize>(actual.size()));
        need(bool(out) && actual==old,"ISO field changed during UDF staging");
        out.clear();out.seekp(static_cast<std::streamoff>(at));
        write(out,replacement);
    };
    auto updated_record=[&](Bytes record,const Node& node) {
        const auto found=sections.find(&node);
        if(found!=sections.end() && !record.empty()) {
            both(record,2,found->second.lba);
            both(record,10,found->second.length);
        }
        return record;
    };
    std::map<std::uint64_t,Bytes> compacted;
    for(const auto& [sector,entry]:deletion_sectors)
        compacted.emplace(sector,entry.original);
    for(const auto& [node,section]:sections) {
        (void)section;
        auto patch_record=[&](std::uint64_t at,const Bytes& record) {
            auto updated=updated_record(record,*node);
            const auto sector=at/block*block;
            auto found=compacted.find(sector);
            if(found!=compacted.end()) {
                auto& data=found->second;
                const auto offset=static_cast<std::size_t>(at-sector)+2;
                need(offset<=data.size() && 16<=data.size()-offset &&
                     std::equal(record.begin()+2,record.begin()+18,
                                data.begin()+static_cast<std::ptrdiff_t>(offset)),
                     "shared replacement conflicts with directory compaction");
                std::copy(updated.begin()+2,updated.begin()+18,
                          data.begin()+static_cast<std::ptrdiff_t>(offset));
            } else patch(at+2,Bytes(record.begin()+2,record.begin()+18),
                              Bytes(updated.begin()+2,updated.begin()+18));
        };
        patch_record(node->record_at,node->record);
        if(image.joliet)patch_record(node->joliet_record_at,node->joliet_record);
    }
    for(const auto& [sector,entry]:deletion_sectors) {
        auto& data=compacted.at(sector);
        auto removed=entry.removed;
        std::sort(removed.rbegin(),removed.rend());
        for(const auto& [at,bytes]:removed) {
            need(at<=data.size() && bytes<=data.size()-at,
                 "shared ISO directory compaction exceeds sector");
            data.erase(data.begin()+static_cast<std::ptrdiff_t>(at),
                       data.begin()+static_cast<std::ptrdiff_t>(at+bytes));
            data.resize(entry.original.size(),0);
        }
        patch(sector,entry.original,std::move(data));
    }
    auto adjusted_record_at=[&](std::uint64_t at) {
        const auto sector=at/block*block;
        const auto found=deletion_sectors.find(sector);
        if(found==deletion_sectors.end())return at;
        const auto offset=at-sector;
        for(const auto& [begin,bytes]:found->second.removed)
            if(begin<offset)at-=bytes;
        return at;
    };
    auto expected=image.descriptors;
    for(auto& [lba,vd]:expected) {
        Bytes old(vd.begin()+80,vd.begin()+88);
        both(vd,80,static_cast<std::uint32_t>(size/block));
        patch(std::uint64_t(lba)*block+80,std::move(old),
              Bytes(vd.begin()+80,vd.begin()+88));
    }
    out.flush();need(bool(out),"cannot flush coordinated ISO stage");out.close();
    Image checked(staged,true);
    need(checked.udf_bridge && checked.size==size &&
         checked.descriptors==expected &&
         checked.boot_regions==image.boot_regions &&
         checked.boot_entries==image.boot_entries &&
         checked.boot_lbas==image.boot_lbas &&
         checked.boot_descriptor_lbas==image.boot_descriptor_lbas,
         "coordinated ISO descriptors or boot resources changed");
    auto verify=[&](auto&& self,const Node& a,const Node& b)->void {
        const auto found=sections.find(&a);
        const auto lba=found==sections.end()?a.lba:found->second.lba;
        const auto length=found==sections.end()?a.size:found->second.length;
        auto parts=a.sections;
        if(found!=sections.end())parts={found->second};
        need(a.name==b.name && a.logical==b.logical &&
             a.directory==b.directory && lba==b.lba && length==b.size &&
             a.identifier==b.identifier && a.joliet_identifier==b.joliet_identifier &&
             adjusted_record_at(a.record_at)==b.record_at &&
             adjusted_record_at(a.joliet_record_at)==b.joliet_record_at &&
             parts==b.sections && updated_record(a.record,a)==b.record &&
             updated_record(a.joliet_record,a)==b.joliet_record,
             "coordinated ISO/Joliet namespace differs from edit plan");
        std::size_t next=0;
        for(const auto& child:a.children) {
            if(deleted.count(child.get()))continue;
            need(next<b.children.size(),"shared ISO deletion removed unrelated entry");
            self(self,*child,*b.children[next++]);
        }
        need(next==b.children.size(),"deleted ISO entry remains visible");
    };
    verify(verify,*image.root,*checked.root);
    checked.file.close();
    const Disc final_udf(staged);
    need(final_udf.format()=="UDF","coordinated image lost its UDF namespace");
    for(const auto& [key,node]:targets) {
        const auto item=std::find_if(final_udf.items().begin(),final_udf.items().end(),
            [&](const Item& v){return v.path==key && !v.directory;});
        if(deleted.count(node)) {
            need(item==final_udf.items().end(),"deleted UDF entry remains visible");
            continue;
        }
        const auto& section=sections.at(node);
        need(item!=final_udf.items().end() && item->size==section.length &&
             !item->extents.empty() &&
             item->extents.front().offset==std::uint64_t(section.lba)*block,
             "final ISO/Joliet/UDF file mapping differs");
    }
}
// A new file is mirrored only when the old ISO and UDF trees are
// demonstrably the same namespace. Boot-only or split bridges retain
// their established UDF-only creation policy.
using IsoNodeIndex=std::map<std::string,const Node*>;
IsoNodeIndex index_iso_tree(const Node& root) {
    IsoNodeIndex result;
    auto gather=[&](auto&& self,const Node& parent,const std::string& prefix)->void {
        for(const auto& child:parent.children) {
            const auto key=prefix.empty()?child->logical:prefix+"/"+child->logical;
            need(result.emplace(upper(key),child.get()).second,
                 "ambiguous ISO namespace");
            if(child->directory)self(self,*child,key);
        }
    };
    gather(gather,root,"");
    return result;
}
// Reauthor only the two ISO directory trees and their path tables in
// a dedicated append area; UDF remains the owner of the file payloads.
// Retain every old raw file record and directory attribute byte.
struct SharedTreePlan {
    std::unique_ptr<Node> root;
    std::vector<Node*> dirs;
    std::map<const Node*,std::array<std::pair<Bytes,Bytes>,2>> dots;
    std::map<std::string,Node*> added;
    std::array<std::vector<Node*>,2> path_dirs;
    std::array<std::uint32_t,2> table_size{};
    std::array<std::array<std::uint32_t,2>,2> table_lba{};
};
bool iso_identifier_less(const Bytes& a,const Bytes& b) {
    const auto n=std::min(a.size(),b.size());
    for(std::size_t i=0;i<n;++i)if(a[i]!=b[i])return a[i]<b[i];
    return a.size()<b.size();
}
Bytes directory_identifier(const Bytes& r) {
    need(r.size()>=34 && r[32] && 33u+r[32]<=r.size(),
         "invalid ISO directory identifier");
    return Bytes(r.begin()+33,r.begin()+33+r[32]);
}
const Bytes& view_id(const Node& n,unsigned view) {
    return view?n.joliet_identifier:n.identifier;
}
Bytes shared_tree_record(const Node& n,unsigned view) {
    const auto& old=view?n.joliet_record:n.record;
    if(old.empty())return record(n,view_id(n,view),view);
    auto result=old;
    if(n.directory) {
        both(result,2,n.new_lba[view]);both(result,10,n.new_size[view]);
    }
    return result;
}
Bytes shared_tree_directory(const SharedTreePlan& tree,const Node& dir,
                            unsigned view) {
    Bytes data;
    auto append=[&](const Bytes& r) {
        need(r.size()>=34 && r.size()<=255 && r[0]==r.size(),
             "invalid mirrored directory record");
        if(data.size()%block+r.size()>block)
            data.resize((data.size()/block+1)*block,0);
        data.insert(data.end(),r.begin(),r.end());
        need(data.size()<=8u*1024u*1024u,"mirrored ISO directory exceeds budget");
    };
    const auto& dot=tree.dots.at(&dir)[view];
    auto make_dot=[&](const Bytes& old,const Node& dest,unsigned char id) {
        if(old.empty())return record(dir,Bytes{id},view,&dest);
        auto r=old;
        both(r,2,dest.new_lba[view]);both(r,10,dest.new_size[view]);
        return r;
    };
    append(make_dot(dot.first,dir,0));
    append(make_dot(dot.second,dir.parent?*dir.parent:dir,1));
    std::vector<const Node*> sorted;
    for(const auto& child:dir.children)sorted.push_back(child.get());
    std::sort(sorted.begin(),sorted.end(),[&](const Node* a,const Node* b){
        return iso_identifier_less(view_id(*a,view),view_id(*b,view));
    });
    for(const auto* child:sorted)append(shared_tree_record(*child,view));
    data.resize((data.size()+block-1)/block*block,0);
    return data;
}
Bytes shared_tree_path_table(const SharedTreePlan& tree,unsigned view,bool big) {
    const auto& dirs=tree.path_dirs[view];
    std::map<const Node*,std::uint32_t> numbers;
    for(std::size_t i=0;i<dirs.size();++i)
        numbers.emplace(dirs[i],static_cast<std::uint32_t>(i+1));
    Bytes table;
    for(const auto* d:dirs) {
        const auto id=d==tree.root.get()?Bytes{0}:view_id(*d,view);
        const auto at=table.size();
        table.resize(at+8+id.size()+(id.size()%2),0);
        table[at]=static_cast<unsigned char>(id.size());
        put(table,at+2,d->new_lba[view],4,big);
        put(table,at+6,numbers.at(d->parent?d->parent:d),2,big);
        std::copy(id.begin(),id.end(),table.begin()+static_cast<std::ptrdiff_t>(at+8));
    }
    return table;
}
bool plan_shared_additions(const std::filesystem::path& path,Image& image,
        const IsoNodeIndex& iso_files,const std::vector<ArchiveChange>& changes,
        CoordinatedIsoUpdate& plan,SharedTreePlan& tree) {
    if(image.hybrid_system_area || image.iso_system_use || changes.empty() ||
       !std::all_of(changes.begin(),changes.end(),[&](const ArchiveChange& c){
           return !c.remove && !iso_files.count(upper(edit_path(c.path)));
       }))return false;
    {
        const Disc udf(path);
        if(udf.format()!="UDF" || udf.items().size()!=iso_files.size())return false;
        std::set<std::string> seen;
        bool common_data=false;
        for(const auto& item:udf.items()) {
            const auto key=upper(item.path);
            const auto at=iso_files.find(key);
            if(!seen.insert(key).second || at==iso_files.end() ||
               at->second->directory!=item.directory)return false;
            if(item.directory)continue;
            const auto* n=at->second;
            if(n->size!=item.size || !item.size || !item.inline_data.empty() ||
               n->sections.size()!=1 || item.extents.size()!=1 ||
               item.extents[0].sparse ||
               item.extents[0].offset!=std::uint64_t(n->lba)*block ||
               item.extents[0].length!=item.size)return false;
            common_data=true;
        }
        if(!common_data)return false;
    }
    const auto inspection=inspect_udf_edit(path,true);
    if(!inspection.parser_compatible || inspection.partition_access_type!=1 ||
       inspection.allocation_bitmap || inspection.allocation_table)return false;
    const unsigned views=image.joliet?2u:1u;
    std::map<std::string,Node*> index;
    std::uint64_t original_bytes=0;
    auto clone=[&](auto&& self,const Node& old,Node* parent,
                   const std::string& key)->std::unique_ptr<Node> {
        auto n=std::make_unique<Node>();
        n->name=old.name;n->logical=old.logical;n->directory=old.directory;
        n->parent=parent;n->record=old.record;n->joliet_record=old.joliet_record;
        n->identifier=old.identifier;n->joliet_identifier=old.joliet_identifier;
        n->lba=old.lba;n->size=old.size;n->sections=old.sections;
        need(index.emplace(upper(key),n.get()).second,"duplicate mirrored ISO path");
        if(n->directory) {
            tree.dirs.push_back(n.get());
            auto& dots=tree.dots[n.get()];
            for(unsigned view=0;view<views;++view) {
                const auto& r=view?old.joliet_record:old.record;
                need(r.size()>=34,"missing original ISO directory record");
                const auto at=std::uint64_t(le(r,2))*block;
                const auto bytes=le(r,10);
                need(bytes && bytes%block==0 && bytes<=8u*1024u*1024u &&
                     original_bytes+bytes<=64ull*1024*1024,
                     "unsupported original mirrored directory geometry");
                original_bytes+=bytes;
                const auto data=image.read(at,bytes);
                std::size_t count=0;
                for(std::size_t p=0;p<data.size();) {
                    const auto length=std::size_t(data[p]);
                    if(!length) {
                        const auto end=std::min(data.size(),(p/block+1)*block);
                        need(std::all_of(data.begin()+static_cast<std::ptrdiff_t>(p),
                               data.begin()+static_cast<std::ptrdiff_t>(end),
                               [](auto c){return c==0;}),
                             "nonzero old ISO directory padding");
                        p=end;continue;
                    }
                    need(length>=34 && length<=data.size()-p && p%block+length<=block,
                         "invalid old mirrored ISO directory record");
                    if(count<2) {
                        Bytes dot(data.begin()+static_cast<std::ptrdiff_t>(p),
                                  data.begin()+static_cast<std::ptrdiff_t>(p+length));
                        need(directory_identifier(dot)==Bytes{static_cast<unsigned char>(count)} &&
                             (dot[25]&2u),"missing original ISO dot record");
                        if(!count)dots[view].first=std::move(dot);
                        else dots[view].second=std::move(dot);
                    }
                    ++count;p+=length;
                }
                need(count==old.children.size()+2,
                     "old mirrored ISO directory has unsupported extra records");
            }
        }
        for(const auto& child:old.children) {
            const auto child_key=key.empty()?child->logical:key+"/"+child->logical;
            n->children.push_back(self(self,*child,n.get(),child_key));
        }
        return n;
    };
    tree.root=clone(clone,*image.root,nullptr,"");
    std::vector<const ArchiveChange*> sorted;
    for(const auto& c:changes)sorted.push_back(&c);
    std::stable_sort(sorted.begin(),sorted.end(),[](const auto* a,const auto* b){
        return std::count(a->path.begin(),a->path.end(),'/')<
               std::count(b->path.begin(),b->path.end(),'/');
    });
    for(const auto* c:sorted) {
        const auto key=edit_path(c->path);
        const auto slash=key.rfind('/');
        const auto parent_key=slash==std::string::npos?std::string{}:key.substr(0,slash);
        const auto component=slash==std::string::npos?key:key.substr(slash+1);
        const auto found=index.find(upper(parent_key));
        need(found!=index.end() && found->second->directory,
             "new shared entry requires an existing or explicitly created parent");
        auto* parent=found->second;
        auto n=std::make_unique<Node>();
        n->parent=parent;n->logical=component;n->directory=c->directory;
        if(!c->directory) {
            need(std::filesystem::is_regular_file(
                     std::filesystem::symlink_status(c->source)),
                 "shared addition source must be a regular file");
            n->size=std::filesystem::file_size(c->source);
            need(n->size>0 && n->size<=0xffffffffull,
                 "shared addition requires nonempty single-extent ISO file");
        }
        auto collision=[&](const Bytes& id,unsigned view) {
            const auto name=upper(decode(id,view!=0,n->directory));
            return std::any_of(parent->children.begin(),parent->children.end(),
                [&](const auto& child) {
                    return upper(decode(view_id(*child,view),view!=0,
                                        child->directory))==name;
                });
        };
        try {n->identifier=identifier(component,false,n->directory);}
        catch(const EditUnsupported&) {
            need(image.joliet,"shared name cannot be represented in ISO9660");
            for(unsigned salt=0;salt<128;++salt) {
                n->identifier=identifier(iso_alias(component,n->directory,salt),
                                         false,n->directory);
                if(!collision(n->identifier,0))break;
            }
        }
        need(!collision(n->identifier,0),"shared primary ISO name collision");
        n->name=decode(n->identifier,false,n->directory);
        if(image.joliet) {
            n->joliet_identifier=identifier(component,true,n->directory);
            need(!collision(n->joliet_identifier,1),"shared Joliet name collision");
        } else n->logical=n->name;
        // Generated record() uses this minimal header for timestamps.
        // Leave the raw-record members empty until serialization.
        if(n->directory) {
            plan.shared_directories.insert(key);
            tree.dirs.push_back(n.get());
            tree.dots[n.get()]={};
        } else plan.shared_additions.insert(key);
        need(index.emplace(upper(key),n.get()).second &&
             tree.added.emplace(key,n.get()).second,
             "duplicate new mirrored entry");
        parent->children.push_back(std::move(n));
        need(index.size()<=200000 && tree.dirs.size()<=65535,
             "mirrored directory tree exceeds entry limit");
    }
    // The existing strict ISO/Joliet pairer accepts equal directory sizes.
    // Pad each pair to the larger size, preserving both views' record data.
    for(auto* dir:tree.dirs) {
        std::size_t largest=0;
        for(unsigned view=0;view<views;++view)
            largest=std::max(largest,shared_tree_directory(tree,*dir,view).size());
        for(unsigned view=0;view<views;++view)
            dir->new_size[view]=static_cast<std::uint32_t>(largest);
    }
    std::uint64_t blocks=0;
    for(unsigned view=0;view<views;++view) {
        auto& order=tree.path_dirs[view];
        order.push_back(tree.root.get());
        for(std::size_t i=0;i<order.size();++i) {
            std::vector<Node*> children;
            for(const auto& child:order[i]->children)
                if(child->directory)children.push_back(child.get());
            std::sort(children.begin(),children.end(),[&](const Node* a,const Node* b){
                return iso_identifier_less(view_id(*a,view),view_id(*b,view));
            });
            for(auto* child:children)order.push_back(child);
        }
        need(order.size()==tree.dirs.size(),"invalid mirrored path table count");
        const auto size=shared_tree_path_table(tree,view,false).size();
        need(size && size<=0xffffffffu,"mirrored path table exceeds size limit");
        tree.table_size[view]=static_cast<std::uint32_t>(size);
        for(const auto* dir:tree.dirs)blocks+=dir->new_size[view]/block;
        blocks+=2*((size+block-1)/block);
    }
    need(blocks && blocks<=32768,"mirrored ISO metadata exceeds 64 MiB budget");
    plan.iso_metadata_blocks=blocks;
    for(const auto& [lba,vd]:image.descriptors) {
        (void)vd;
        plan.metadata_writes.emplace_back(std::uint64_t(lba)*block+132,58);
    }
    return true;
}

void coordinate_shared_add_stage(const std::filesystem::path& path,
        const std::filesystem::path& staged,const Image& image,
        const IsoNodeIndex& original,const CoordinatedIsoUpdate& plan,
        SharedTreePlan& tree,const EditProgress& progress) {
    protect_iso_udf_stage(path,staged,image,progress);
    const auto size=std::filesystem::file_size(staged);
    need(size>image.size && size%block==0 && size/block<0xffffffffu,
         "shared addition requires append mastering");
    const unsigned views=image.joliet?2u:1u;
    std::uint64_t first=0,main=0;
    {
        Image mapped(staged,true);
        const auto anchor=mapped.read(256ull*block,block);
        main=le(anchor,20);
        need(main>=plan.iso_metadata_blocks &&
             main<size/block,"invalid reserved ISO metadata area");
        first=main-plan.iso_metadata_blocks;
        need(first>=image.size/block,"reserved ISO metadata overlaps source image");
        const auto vds_blocks=le(anchor,16)/block;
        need(vds_blocks && vds_blocks<=8192 && vds_blocks<=size/block-main,
             "invalid remastered UDF VDS bounds");
        bool pd=false;
        for(std::uint64_t i=0;i<vds_blocks;++i) {
            const auto d=mapped.read((main+i)*block,block);
            if(le(d,0,2)==8)break;
            if(le(d,0,2)!=5)continue;
            need(!pd && std::uint64_t(le(d,188))+le(d,192)==first,
                 "reserved ISO metadata overlaps UDF partition");
            pd=true;
        }
        need(pd,"missing remastered physical UDF partition");
    }
    {
        const Disc udf(staged);
        need(udf.format()=="UDF","cannot read staged UDF additions");
        for(const auto& [key,n]:tree.added) {
            const auto found=std::find_if(udf.items().begin(),udf.items().end(),
                [&](const Item& item){return item.path==key;});
            need(found!=udf.items().end() && found->directory==n->directory,
                 "new shared UDF path missing or wrong type");
            if(n->directory)continue;
            need(found->size==n->size && found->inline_data.empty() &&
                 !found->extents.empty(),"shared UDF addition has no ISO extent");
            const auto start=found->extents.front().offset;
            need(start>=image.size && start%block==0 && start<=first*block &&
                 found->size<=first*block-start,
                 "shared addition outside UDF append area");
            auto next=start;
            for(const auto& e:found->extents) {
                need(!e.sparse && e.offset==next &&
                     e.length<=start+found->size-next,
                     "shared addition data is not contiguous");
                next+=e.length;
            }
            need(next-start==found->size,"shared addition extent size mismatch");
            n->lba=static_cast<std::uint32_t>(start/block);
            n->sections={{n->lba,static_cast<std::uint32_t>(n->size)}};
        }
    }
    // Assign every directory and path table LBA before serializing any
    // parent, child, '.' or '..' reference.
    auto next=first;
    for(unsigned view=0;view<views;++view) {
        for(auto* dir:tree.dirs) {
            dir->new_lba[view]=static_cast<std::uint32_t>(next);
            next+=dir->new_size[view]/block;
        }
        for(unsigned big=0;big<2;++big) {
            tree.table_lba[view][big]=static_cast<std::uint32_t>(next);
            next+=(tree.table_size[view]+block-1)/block;
        }
    }
    need(next==main,"mirrored ISO metadata reservation mismatch");
    std::fstream out(staged,std::ios::binary|std::ios::in|std::ios::out);
    need(bool(out),"cannot open staged mirrored ISO metadata");
    auto write_reserved=[&](std::uint64_t at,const Bytes& data) {
        edit_tick(progress);
        need(at>=first*block && at<=main*block &&
             data.size()<=main*block-at,"mirrored ISO metadata outside reservation");
        Bytes old(data.size());
        out.clear();out.seekg(static_cast<std::streamoff>(at));
        out.read(reinterpret_cast<char*>(old.data()),
                 static_cast<std::streamsize>(old.size()));
        need(bool(out) && std::all_of(old.begin(),old.end(),[](auto c){return c==0;}),
             "reserved ISO metadata space is not empty");
        out.clear();out.seekp(static_cast<std::streamoff>(at));write(out,data);
    };
    for(unsigned view=0;view<views;++view) {
        for(const auto* dir:tree.dirs) {
            auto data=shared_tree_directory(tree,*dir,view);
            need(data.size()<=dir->new_size[view],"mirrored ISO directory size changed");
            data.resize(dir->new_size[view],0);
            write_reserved(std::uint64_t(dir->new_lba[view])*block,data);
        }
        for(unsigned big=0;big<2;++big) {
            auto data=shared_tree_path_table(tree,view,big!=0);
            need(data.size()==tree.table_size[view],"mirrored path table size changed");
            data.resize((data.size()+block-1)/block*block,0);
            write_reserved(std::uint64_t(tree.table_lba[view][big])*block,data);
        }
    }
    auto expected=image.descriptors;
    for(auto& [lba,vd]:expected) {
        const auto old=vd;
        const unsigned view=vd[0]==2?1u:0u;
        both(vd,80,static_cast<std::uint32_t>(size/block));
        both(vd,132,tree.table_size[view]);
        put(vd,140,tree.table_lba[view][0]);put(vd,144,0);
        put(vd,148,tree.table_lba[view][1],4,true);put(vd,152,0);
        const auto& root_record=view?tree.root->joliet_record:tree.root->record;
        need(root_record.size()==34,"invalid original ISO root record");
        auto root=root_record;
        both(root,2,tree.root->new_lba[view]);both(root,10,tree.root->new_size[view]);
        std::copy(root.begin(),root.end(),vd.begin()+156);
        for(const auto& [offset,length]:{std::pair<std::size_t,std::size_t>{80,8},{132,58}}) {
            const auto at=std::uint64_t(lba)*block+offset;
            need(std::find(plan.metadata_writes.begin(),plan.metadata_writes.end(),
                    std::pair<std::uint64_t,std::uint64_t>{at,length})!=
                         plan.metadata_writes.end(),"unplanned mirrored ISO descriptor field");
            Bytes actual(length);
            out.clear();out.seekg(static_cast<std::streamoff>(at));
            out.read(reinterpret_cast<char*>(actual.data()),
                     static_cast<std::streamsize>(actual.size()));
            need(bool(out) && std::equal(actual.begin(),actual.end(),old.begin()+offset),
                 "original ISO descriptor changed during UDF mastering");
            out.clear();out.seekp(static_cast<std::streamoff>(at));
            write(out,Bytes(vd.begin()+offset,vd.begin()+offset+length));
        }
    }
    out.flush();need(bool(out),"cannot flush mirrored ISO tree");out.close();
    Image checked(staged,true);
    need(checked.udf_bridge && checked.size==size && checked.descriptors==expected &&
         checked.boot_regions==image.boot_regions &&
         checked.boot_entries==image.boot_entries &&
         checked.boot_lbas==image.boot_lbas &&
         checked.boot_descriptor_lbas==image.boot_descriptor_lbas,
         "shared tree remaster changed boot descriptors");
    const auto after=index_iso_tree(*checked.root);
    const auto wanted=index_iso_tree(*tree.root);
    need(after.size()==original.size()+tree.added.size() && after.size()==wanted.size(),
         "shared tree remaster changed unrelated namespace");
    for(const auto& [key,n]:wanted) {
        const auto found=after.find(key);
        need(found!=after.end(),"remastered shared ISO entry missing");
        const auto* got=found->second;
        need(n->directory==got->directory && n->name==got->name &&
             n->logical==got->logical &&
             n->identifier==got->identifier && n->joliet_identifier==got->joliet_identifier &&
             got->record==shared_tree_record(*n,0) &&
             (!image.joliet || got->joliet_record==shared_tree_record(*n,1)) &&
             (n->directory?
                (got->lba==n->new_lba[0] && got->size==n->new_size[0]):
                (got->lba==n->lba && got->size==n->size && got->sections==n->sections)),
             "remastered shared ISO entry differs from plan");
    }
    for(unsigned view=0;view<views;++view) {
        for(const auto* dir:tree.dirs) {
            auto data=shared_tree_directory(tree,*dir,view);
            data.resize(dir->new_size[view],0);
            need(checked.read(std::uint64_t(dir->new_lba[view])*block,data.size())==data,
                 "remastered ISO directory readback differs from plan");
        }
        for(unsigned big=0;big<2;++big) {
            auto data=shared_tree_path_table(tree,view,big!=0);
            data.resize((data.size()+block-1)/block*block,0);
            need(checked.read(std::uint64_t(tree.table_lba[view][big])*block,data.size())==data,
                 "remastered ISO path table readback differs from plan");
        }
    }
    checked.file.close();
    const Disc udf(staged);
    need(udf.format()=="UDF","mirrored remaster lost UDF namespace");
    for(const auto& [key,n]:tree.added) {
        const auto found=std::find_if(udf.items().begin(),udf.items().end(),
            [&](const Item& item){return item.path==key;});
        need(found!=udf.items().end() && found->directory==n->directory &&
             (n->directory || (found->size==n->size && !found->extents.empty() &&
                found->extents.front().offset==std::uint64_t(n->lba)*block)),
             "final shared ISO/Joliet/UDF mapping differs");
    }
}
void edit_iso_udf_bridge_metadata(const std::filesystem::path& path,
                                  Image& image,
                                  const std::vector<ArchiveChange>& changes,
                                  const EditProgress& progress) {
    need(image.udf_bridge && image.boot_regions_bounded,
         "ISO/UDF metadata edit requires bounded El Torito boot images");
    // Shared regular files can be remastered or unlinked from both
    // namespaces; ISO-only and ambiguous aliases remain protected.
    std::map<std::string,const Node*> iso_files;
    auto gather=[&](auto&& self,const Node& node,const std::string& prefix)->void {
        for(const auto& child:node.children) {
            const auto key=prefix.empty()?child->logical:prefix+"/"+child->logical;
            need(iso_files.emplace(upper(key),child.get()).second,
                 "ambiguous ISO/UDF path");
            if(child->directory)self(self,*child,key);
        }
    };
    gather(gather,*image.root,"");
    CoordinatedIsoUpdate plan;
    SharedIsoDeleteSectors deletion_sectors;
    SharedTreePlan add_tree;
    std::map<std::string,const Node*> shared;
    const bool adding_shared=plan_shared_additions(path,image,iso_files,changes,
                                                   plan,add_tree);
    if(std::any_of(changes.begin(),changes.end(),[&](const ArchiveChange& c){
            return iso_files.count(upper(edit_path(c.path)))!=0;
        })) {
        need(!image.hybrid_system_area && !image.iso_system_use,
             "shared ISO/UDF resize needs a plain ISO/Joliet bridge");
        const Disc udf(path);
        for(const auto& change:changes) {
            const auto key=edit_path(change.path);
            const auto found=iso_files.find(upper(key));
            need(found!=iso_files.end() && !found->second->directory &&
                 !change.directory &&
                 (change.remove || std::filesystem::is_regular_file(
                     std::filesystem::symlink_status(change.source))),
                 "shared ISO/UDF edits require existing regular files");
            const auto* node=found->second;
            const auto item=std::find_if(udf.items().begin(),udf.items().end(),
                [&](const Item& v){return v.path==key && !v.directory;});
            need(item!=udf.items().end() && item->size==node->size &&
                 item->size>0 && item->inline_data.empty() &&
                 item->extents.size()==1 && !item->extents[0].sparse &&
                 item->extents[0].offset==std::uint64_t(node->lba)*block &&
                 item->extents[0].length==node->size,
                 "ISO and UDF original shared extents disagree");
            if(!change.remove) {
                const auto length=std::filesystem::file_size(change.source);
                need(length>0 && length<=0xffffffffull,
                     "shared replacement exceeds single ISO extent bounds");
            }
            protect_boot(*node,image.boot_lbas);
            for(const auto& [at,bytes]:image.boot_regions)
                need(!intersect(item->extents[0].offset,item->size,at,bytes),
                     "shared replacement overlaps boot data");
            need(node->record_at && (!image.joliet || node->joliet_record_at),
                 "missing shared ISO directory record location");
            shared.emplace(key,node);
            if(change.remove) {
                plan.shared_deletions.insert(key);
                plan_shared_iso_delete(image,*node,false,deletion_sectors);
                if(image.joliet)
                    plan_shared_iso_delete(image,*node,true,deletion_sectors);
            } else plan.shared_replacements.insert(key);
        }
        for(const auto& [key,node]:shared) {
            if(plan.shared_deletions.count(key))continue;
            auto add_field=[&](std::uint64_t at) {
                if(!deletion_sectors.count(at/block*block))
                    plan.metadata_writes.emplace_back(at+2,16);
            };
            add_field(node->record_at);
            if(image.joliet)add_field(node->joliet_record_at);
        }
        for(const auto& [sector,entry]:deletion_sectors)
            plan.metadata_writes.emplace_back(sector,entry.original.size());
    }
    if(adding_shared || !shared.empty()) {
        for(const auto& [lba,vd]:image.descriptors) {
            (void)vd;
            plan.metadata_writes.emplace_back(std::uint64_t(lba)*block+80,8);
        }
        // Verify every planned record field is disjoint from immutable
        // payload, boot resources, path tables and existing UDF anchors.
        std::sort(plan.metadata_writes.begin(),plan.metadata_writes.end());
        std::uint64_t end=0;
        for(const auto& [at,bytes]:plan.metadata_writes) {
            need(at>=end && at<=image.size && bytes<=image.size-at,
                 "overlapping or invalid shared ISO metadata fields");
            end=at+bytes;
            for(const auto* ranges:{&image.boot_regions,&image.udf_metadata_regions,
                                   &image.iso_auxiliary_regions})
                for(const auto& [start,length]:*ranges)
                    need(!intersect(at,bytes,start,length),
                         "shared ISO metadata field overlaps protected data");
            for(const auto& [key,node]:iso_files) {
                (void)key;
                if(!node->directory)each_extent(*node,[&](auto start,auto length) {
                    need(!intersect(at,bytes,start,length),
                         "shared ISO metadata field overlaps a file payload");
                });
            }
        }
    }
    // Release the original input handle so Windows can atomically replace
    // the image after the staged UDF writer and boot validator succeed.
    image.file.close();
    const EditStageValidator validate=
        [&path,&image,&progress,&shared,&plan,&deletion_sectors,
         &iso_files,&add_tree,adding_shared](const std::filesystem::path& staged) {
            if(adding_shared)
                coordinate_shared_add_stage(path,staged,image,iso_files,plan,
                                            add_tree,progress);
            else if(shared.empty())protect_iso_udf_stage(path,staged,image,progress);
            else coordinate_shared_bridge_stage(path,staged,image,shared,plan,
                                                deletion_sectors,progress);
        };
    // UDF may advertise free blocks that ISO9660 owns exclusively, e.g.
    // boot.catalog, BIOS/EFI image extents or a shared old ISO filename.
    // Exclude them *before* UDF's allocation scan so a new file uses a
    // genuinely unshared bitmap run rather than failing after a full copy.
    // All spans still go through the mandatory byte-exact staged guard.
    EditProtectedRanges immutable;
    immutable.emplace_back(0,256ull*block);
    immutable.insert(immutable.end(),image.boot_regions.begin(),
                     image.boot_regions.end());
    immutable.insert(immutable.end(),image.udf_metadata_regions.begin(),
                     image.udf_metadata_regions.end());
    immutable.insert(immutable.end(),image.joliet_regions.begin(),
                     image.joliet_regions.end());
    immutable.insert(immutable.end(),image.iso_auxiliary_regions.begin(),
                     image.iso_auxiliary_regions.end());
    std::vector<const Node*> nodes{image.root.get()};
    for(std::size_t at=0;at<nodes.size();++at) {
        const auto* node=nodes[at];
        if(node->size)
            immutable.emplace_back(std::uint64_t(node->lba)*block,
                                   node->size);
        for(const auto& child:node->children)
            nodes.push_back(child.get());
        need(nodes.size()<=200000,"ISO/UDF immutable namespace exceeds limit");
    }
    edit_udf(path,changes,progress,true,validate,immutable,
             shared.empty() && !adding_shared?nullptr:&plan);
}
}
void edit_iso(const std::filesystem::path& path,const std::vector<ArchiveChange>& changes,const EditProgress& progress) {
    edit_tick(progress);Image image(path,true);
    // Duplicate WCX entries can otherwise shadow each other in the rebuilt
    // ISO tree. Detect them before selecting either the ISO or UDF writer.
    // No source or staging bytes have been touched at this point.
    std::set<std::string> targets;
    for(const auto& change:changes) {
        const auto key=upper(edit_path(change.path));
        need(targets.insert(key).second,
             "ISO edit batch contains duplicate or case-colliding paths");
    }
    for(const auto& change:changes) {
        if(!change.remove)continue;
        const auto folder=upper(edit_path(change.path))+"/";
        for(const auto& other:changes)
            if(!other.remove)
                need(!upper(edit_path(other.path)).starts_with(folder),
                     "ISO edit batch adds into a deleted directory");
    }
    if(image.hybrid_system_area && !image.udf_bridge) {
        edit_iso_hybrid_equal_length(path,image,changes,progress);
        return;
    }
    if(image.udf_bridge) {
        // Prefer the non-metadata equal-length replacement for a pre-existing
        // single-extent file (including UDF-only paths).
        const Disc udf(path);
        need(udf.format()=="UDF","ISO/UDF filesystem is unreadable");
        bool same_size_replace=!changes.empty();
        for(const auto& change:changes) {
            if(change.remove || change.directory ||
               !std::filesystem::is_regular_file(
                   std::filesystem::symlink_status(change.source))) {
                same_size_replace=false;
                break;
            }
            const auto key=edit_path(change.path);
            const auto it=std::find_if(udf.items().begin(),udf.items().end(),
                [&](const Item& entry){return !entry.directory && entry.path==key;});
            if(it==udf.items().end() ||
               std::filesystem::file_size(change.source)!=it->size ||
               it->size==0 || it->extents.size()!=1 || it->extents[0].sparse ||
               !it->inline_data.empty()) {
                same_size_replace=false;
                break;
            }
        }
        if(same_size_replace)
            edit_iso_udf_bridge_equal_length(path,image,changes,progress);
        else {
            need(!image.hybrid_system_area,
                 "hybrid MBR ISO/UDF only supports equal-length file replacement; "
                 "add/delete/resize requires a partition-aware writer");
            edit_iso_udf_bridge_metadata(path,image,changes,progress);
        }
        return;
    }
    // Non-emulated catalog entries may refer to a short BIOS load whose
    // full image is not ISO-visible. An append-only ISO rebuild preserves
    // that original sector verbatim. Emulated disk entries are different:
    // their entire floppy/HDD extent MUST be mapped and validated.
    for(const auto& entry:image.boot_entries)
        if(entry.media)
            need(!image.unbounded_boot_lbas.contains(entry.lba),
                 "emulated El Torito full disk extent is not ISO-visible");
    for(const auto& change:changes){auto p=edit_path(change.path);std::istringstream split(p);std::string name;Node* parent=image.root.get();
        std::vector<std::string> parts;while(std::getline(split,name,'/'))parts.push_back(name);
        for(std::size_t i=0;i<parts.size();++i){const bool last=i+1==parts.size();const auto key=upper(parts[i]);auto it=std::find_if(parent->children.begin(),parent->children.end(),[&](const auto& n){return upper(n->logical)==key;});
            if(last&&change.remove){if(it!=parent->children.end()){
                if(image.rr_rebuild) {
                    std::vector<const Node*> removed{it->get()};
                    for(std::size_t j=0;j<removed.size();++j) {
                        need(!removed[j]->rr_uneditable,
                             "Rock Ridge hard-linked or special entry cannot be deleted");
                        for(const auto& child:removed[j]->children)removed.push_back(child.get());
                    }
                }
                protect_boot(**it,image.boot_lbas);parent->children.erase(it);}break;}
            if(it==parent->children.end()){
                if(change.remove)break;
                auto n=std::make_unique<Node>();n->parent=parent;n->logical=parts[i];n->name=upper(parts[i]);n->directory=!last||change.directory;n->rr=image.rr_rebuild;n->record.resize(34,0);n->record[18]=126;n->record[19]=1;n->record[20]=1;
                // For new Joliet names, generate a deterministic short ISO alias when necessary.
                try {
                    n->identifier=identifier(parts[i],false,n->directory);
                    if(n->rr && 33+n->identifier.size()+1+36+5+parts[i].size()>255)
                        throw EditUnsupported("Rock Ridge needs a shorter primary alias");
                } catch(const EditUnsupported&) {
                    need(image.joliet || image.rr_rebuild,"name cannot be represented in ISO9660");
                    bool unique=false;
                    for(unsigned salt=0;salt<128 && !unique;++salt) {
                        n->name=iso_alias(parts[i],n->directory,salt);
                        n->identifier=identifier(n->name,false,n->directory);
                        unique=std::none_of(parent->children.begin(),parent->children.end(),
                            [&](const auto& child){return child->identifier==n->identifier;});
                    }
                    need(unique,"too many generated ISO alias collisions");
                }
                for(const auto& c:parent->children)
                    need(c->identifier!=n->identifier,"ISO alias collision");
                if(image.joliet)n->joliet_identifier=identifier(parts[i],true,n->directory);
                parent->children.push_back(std::move(n));it=std::prev(parent->children.end());
            }
            auto& n=**it;
            if(!last){need(n.directory,"parent is not a directory");parent=&n;continue;}
            need(n.directory==change.directory,"file/directory replacement is not supported");
            if(!n.directory){
                need(!n.rr_uneditable,"Rock Ridge hard-linked or special entry cannot be replaced");
                protect_boot(n,image.boot_lbas);need(std::filesystem::is_regular_file(std::filesystem::symlink_status(change.source)),"edit source must be a regular file");auto size=std::filesystem::file_size(change.source);need(size<0xffffffffull*block,"ISO file exceeds volume address space");need(!image.rr_rebuild || size<=0xfffff800ull,
                     "Rock Ridge multi-extent writing is not yet supported");
                n.size=size;n.sections.clear();n.source=change.source;}
        }
    }
    EditTransaction tx(path);std::fstream out(tx.output,std::ios::in|std::ios::out|std::ios::binary|std::ios::trunc);need(bool(out),"cannot create staged ISO");image.file.clear();image.file.seekg(0);copy_bytes(image.file,out,image.size,progress);
    std::uint64_t next=image.size/block;
    std::vector<Node*> all{image.root.get()},dirs{image.root.get()};
    for(std::size_t i=0;i<all.size();++i){for(auto& c:all[i]->children){all.push_back(c.get());if(c->directory)dirs.push_back(c.get());}need(all.size()<=200000&&dirs.size()<=65535,"ISO edit tree too large");}
    for(auto* n:all)if(!n->source.empty()) {
        need(next+(n->size+block-1)/block<0xffffffffu,"ISO size limit");
        n->lba=static_cast<std::uint32_t>(next);
        std::ifstream source(n->source,std::ios::binary);
        need(bool(source),"cannot open ISO replacement source");
        // ECMA-119 Level 3 permits multiple records per file. Every
        // nonfinal section is aligned and fits the 32-bit data length.
        constexpr std::uint64_t max_section=0xfffff800ull;
        auto left=n->size;
        do {
            const auto bytes=static_cast<std::uint32_t>(
                std::min(left,max_section));
            n->sections.push_back({static_cast<std::uint32_t>(next),bytes});
            copy_bytes(source,out,bytes,progress);
            write(out,Bytes((block-bytes%block)%block));
            next+=(std::uint64_t(bytes)+block-1)/block;
            left-=bytes;
        } while(left);
        need(source.peek()==std::char_traits<char>::eof(),
             "ISO replacement source grew during write");
    }
    for(unsigned view=0;view<(image.joliet?2u:1u);++view){
        for(auto* d:dirs){d->new_size[view]=static_cast<std::uint32_t>(directory_data(*d,view).size());need(next+d->new_size[view]/block<0xffffffffu,"ISO size limit");d->new_lba[view]=static_cast<std::uint32_t>(next);next+=d->new_size[view]/block;}
        for(auto* d:dirs){edit_tick(progress);write(out,directory_data(*d,view));}
        // ISO 9660 path-table numbering must be breadth-first and sorted
        // by the directory identifier within each parent. Primary and
        // Joliet namespaces can have different identifier orders.
        std::vector<Node*> path_dirs{image.root.get()};
        for(std::size_t parent=0;parent<path_dirs.size();++parent){
            std::vector<Node*> child_dirs;
            for(const auto& child:path_dirs[parent]->children)
                if(child->directory)child_dirs.push_back(child.get());
            std::sort(child_dirs.begin(),child_dirs.end(),[&](const Node* a,const Node* b){
                const Bytes& lhs=view?a->joliet_identifier:a->identifier;
                const Bytes& rhs=view?b->joliet_identifier:b->identifier;
                const auto count=std::min(lhs.size(),rhs.size());
                for(std::size_t i=0;i<count;++i)if(lhs[i]!=rhs[i])return lhs[i]<rhs[i];
                return lhs.size()<rhs.size();
            });
            // Adding individually also avoids GCC 14's erroneous optimized
            // vector range-insert -Wstringop-overflow under -Werror.
            for(auto* child:child_dirs)path_dirs.push_back(child);
        }
        need(path_dirs.size()==dirs.size(),"inconsistent ISO path table directory count");
        std::map<Node*,unsigned> numbers;
        for(std::size_t i=0;i<path_dirs.size();++i)
            numbers[path_dirs[i]]=static_cast<unsigned>(i+1);
        std::array<std::uint32_t,2> table_lba{};std::uint32_t table_size=0;
        for(unsigned big=0;big<2;++big){Bytes table;for(auto* d:path_dirs){Bytes id=d==image.root.get()?Bytes{0}:(view?d->joliet_identifier:d->identifier);const auto at=table.size();table.resize(at+8+id.size()+(id.size()%2));table[at]=static_cast<unsigned char>(id.size());put(table,at+2,d->new_lba[view],4,big!=0);put(table,at+6,numbers[d->parent?d->parent:d],2,big!=0);std::copy(id.begin(),id.end(),table.begin()+static_cast<std::ptrdiff_t>(at+8));}
            table_size=static_cast<std::uint32_t>(table.size());table_lba[big]=static_cast<std::uint32_t>(next);table.resize((table.size()+block-1)/block*block);next+=table.size()/block;need(next<0xffffffffu,"ISO size limit");write(out,table);}
        for(auto& [lba,vd]:image.descriptors)if((vd[0]==2)==(view==1)){both(vd,132,table_size);put(vd,140,table_lba[0]);put(vd,144,0);put(vd,148,table_lba[1],4,true);put(vd,152,0);auto r=record(*image.root,Bytes{0},view);std::copy(r.begin(),r.end(),vd.begin()+156);(void)lba;}
    }
    for(auto& [lba,vd]:image.descriptors){both(vd,80,static_cast<std::uint32_t>(next));out.seekp(std::uint64_t(lba)*block);write(out,vd);}
    out.flush();need(bool(out),"ISO flush failed");out.close();image.file.close();
    // Reparse the staged namespaces and reject changed El Torito references
    // even if the ISO directory metadata itself remains parseable.
    {
        Image verified(tx.output);
        need(verified.size==next*block,"staged ISO size mismatch");
        need(verified.rr_rebuild==image.rr_rebuild,
             "Rock Ridge extension lost in staged ISO");
        need(verified.boot_descriptor_lbas==image.boot_descriptor_lbas &&
             verified.boot_entries==image.boot_entries &&
             verified.boot_lbas==image.boot_lbas &&
             verified.boot_regions==image.boot_regions &&
             verified.boot_regions_bounded==image.boot_regions_bounded,
             "El Torito boot references changed in staged ISO");
        // Parsing both ISO9660 and Joliet metadata is not enough to prove F5
        // wrote the user's bytes. Independently locate every requested path
        // in the rebuilt ISO tree, then compare the complete staged file
        // extent with the original source in bounded chunks.
        for(const auto& change:changes) {
            edit_tick(progress);
            Node* entry=verified.root.get();
            std::istringstream parts(edit_path(change.path));
            std::string component;
            while(entry && std::getline(parts,component,'/')) {
                const auto it=std::find_if(
                    entry->children.begin(),entry->children.end(),
                    [&](const auto& child){
                        return upper(child->logical)==upper(component);
                    });
                entry=it==entry->children.end()?nullptr:it->get();
            }
            if(change.remove) {
                need(!entry,"deleted ISO entry remains visible after rebuild");
                continue;
            }
            need(entry && entry->directory==change.directory,
                 "added ISO path is missing or has the wrong type");
            if(change.directory)continue;
            const auto expected=std::filesystem::file_size(change.source);
            need(expected==entry->size,
                 "staged ISO file length differs from F5 source");
            need(!entry->sections.empty(),
                 "staged ISO file has no section records");
            std::ifstream input(change.source,std::ios::binary);
            std::ifstream output(tx.output,std::ios::binary);
            need(bool(input) && bool(output),
                 "cannot open ISO file for independent F5 verification");
            std::array<char,64*1024> source_chunk{},stage_chunk{};
            std::uint64_t checked=0;
            for(const auto& section:entry->sections) {
                const auto at=std::uint64_t(section.lba)*block;
                need(at<=verified.size && section.length<=verified.size-at &&
                     section.length<=expected-checked,
                     "staged ISO section outside image or file bounds");
                output.clear();
                output.seekg(static_cast<std::streamoff>(at));
                need(bool(output),"cannot locate ISO file during verification");
                for(std::uint64_t left=section.length;left;) {
                edit_tick(progress);
                const auto n=static_cast<std::streamsize>(
                    std::min<std::uint64_t>(left,source_chunk.size()));
                input.read(source_chunk.data(),n);
                output.read(stage_chunk.data(),n);
                need(input.gcount()==n && output.gcount()==n &&
                     std::memcmp(source_chunk.data(),stage_chunk.data(),
                                 static_cast<std::size_t>(n))==0,
                     "staged ISO F5 data differs from original source");
                    left-=static_cast<std::uint64_t>(n);
                }
                checked+=section.length;
            }
            need(checked==expected,"staged ISO section lengths differ from source");
            need(input.peek()==std::char_traits<char>::eof(),
                 "ISO F5 source grew during staging");
        }
    }
    // The ordinary ISO writer appends data and regenerates PVD/Joliet trees.
    // It must never alter *any* original system-area, boot descriptor,
    // boot catalog or full boot-image byte, including emulated floppy/HDD
    // payloads. Only protected ranges in the original prefix are compared.
    OpticalEditGuard guard(image.size);
    guard.protect(0,16ull*block);
    for(const auto lba:image.boot_descriptor_lbas)
        guard.protect(std::uint64_t(lba)*block,block);
    for(const auto& [offset,length]:image.boot_regions)
        guard.protect(offset,length);
    for(const auto& [offset,length]:image.iso_auxiliary_regions)
        guard.protect(offset,length);
    guard.verify_appended(path,tx.output,progress);
    tx.commit(progress);
}
}
