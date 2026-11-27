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
#include <iterator>
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
    std::uint64_t gpt_first_usable=0,gpt_last_usable=0;
    std::optional<std::uint8_t> susp_skip;
    // Joliet directories may occupy different sectors from Primary ISO.
    // Retain both namespaces' extents when protecting hidden boot media.
    std::vector<std::pair<std::uint64_t,std::uint64_t>> joliet_regions;
    std::uint32_t terminator_lba=0;
    std::unique_ptr<Node> root;
    bool joliet=false;
    bool udf_bridge=false;
    bool hybrid_system_area=false; // MBR/GPT present; partition-aware edits only
    bool has_multi_extent=false;
    bool iso_system_use=false; // RRIP/SUSP metadata must never be discarded
    bool rr_rebuild=false;
    // Opt-in strict RRIP remaster; the legacy byte-only patcher still
    // accepts independently named or unmatched Joliet namespaces.
    bool hybrid_rr_strict=false;
    std::uint32_t declared_volume_blocks=0;
    std::uint32_t primary_volume_blocks=0;
    std::uint32_t supplementary_volume_blocks=0;
    std::size_t items=0;
    explicit Image(const std::filesystem::path& p,bool allow_udf_bridge=false,
                   bool strict_hybrid_rr=false)
        :file(p,std::ios::binary),size(std::filesystem::file_size(p)),
         hybrid_rr_strict(strict_hybrid_rr) {
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
               !(hybrid_system_area && iso_system_use && !hybrid_rr_strict)) {
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
            // The byte-only path preserves independent namespaces.
            // Strict RRIP remastering requires a provable pairing.
            if(!(hybrid_system_area && iso_system_use && !hybrid_rr_strict))
                pair(*root,*other);
        }
        if(hybrid_rr_strict)
            need(hybrid_system_area && !udf_bridge && rr_rebuild,
                 "hybrid remaster requires recognized Rock Ridge RRIP metadata");
        // Do not permit multi-extent hybrid Rock Ridge/SUSP metadata
        // to enter the plain ISO/Joliet rebuild or single-extent patcher.
        need(!(hybrid_system_area && has_multi_extent && iso_system_use),
             "multi-extent hybrid SUSP metadata requires a specialized writer");
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
        gpt_first_usable=primary.first;
        gpt_last_usable=primary.last;
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
        // Multi-extent ISO resources may remain immutable inside a
        // partitioned UDF bridge. All physical sections are excluded
        // from allocation and verified before commit. The shared-file
        // writer still requires a single extent; SUSP hybrids have a
        // separate rejection after both namespace trees are parsed.
        if(r[25]&0x80u)has_multi_extent=true;
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
        need(udf_bridge || (hybrid_system_area && !hybrid_rr_strict) ||
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
                            if(!udf_bridge && (!hybrid_system_area || hybrid_rr_strict) &&
                               susp_skip) {
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
                    need(udf_bridge || (hybrid_system_area && !hybrid_rr_strict) ||
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
                     (x->size==y->size&&x->sections==y->sections)) &&
                    (!hybrid_rr_strict || !x->directory ||
                     upper(x->logical)==upper(y->logical) ||
                     upper(x->name)==upper(y->logical)))found.push_back(y.get());
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

// A bounded hybrid rebuild may consume only verified, unreferenced zero
// padding following the declared ISO volume. It does not grow the disk,
// change any partition geometry, or relocate a GPT backup.
struct HybridTailPlan {
    std::uint64_t first{},limit{}; // ISO blocks, exclusive limit
    EditProtectedRanges descriptor_fields;
    // Snapshot all live source extents BEFORE applying ArchiveChange to
    // the tree. A new/replaced node has no final LBA during preflight.
    EditProtectedRanges original_extents;
};
HybridTailPlan plan_hybrid_tail(Image& image,const EditProgress& progress) {
    need(image.hybrid_system_area && !image.udf_bridge &&
         (!image.iso_system_use || (image.hybrid_rr_strict && image.rr_rebuild)) &&
         image.boot_regions_bounded,
         "hybrid directory rebuild requires verified ISO/Joliet or RRIP and bounded boot data");
    HybridTailPlan plan;
    plan.first=image.declared_volume_blocks;
    plan.limit=image.size/block;
    const auto mbr=image.read(0,512);
    unsigned containers=0;
    for(std::size_t i=0;i<4;++i) {
        const auto at=446u+i*16u;
        if(!mbr[at+4] || le(mbr,at+8)!=0)continue;
        // A conventional whole-ISO isohybrid partition is a container,
        // not an independent filesystem. Never infer this for arbitrary
        // start-zero partition types or overlapping container entries.
        need(mbr[at+4]==0x17 && ++containers==1,
             "unknown whole-disk MBR partition cannot host ISO rebuild");
        plan.limit=std::min(plan.limit,std::uint64_t(le(mbr,at+12))/4u);
    }
    need(image.gpt_hybrid || containers==1,
         "hybrid rebuild requires a recognized whole-ISO MBR or validated GPT");
    if(image.gpt_hybrid) {
        need(plan.first*4>=image.gpt_first_usable,
             "ISO tail precedes GPT usable range");
        plan.limit=std::min(plan.limit,(image.gpt_last_usable+1)/4u);
    }
    const auto volume=plan.first*block;
    auto within_volume=[&](std::uint64_t at,std::uint64_t bytes) {
        need(at<=volume && bytes<=volume-at,
             "existing ISO extent exceeds declared volume; padding is not proven free");
    };
    auto protect_field=[&](std::uint64_t at,std::uint64_t bytes) {
        within_volume(at,bytes);
        for(const auto& [start,length]:image.hybrid_protected_regions)
            need(!intersect(at,bytes,start,length),
                 "ISO descriptor update overlaps MBR/GPT partition data");
        for(const auto& [start,length]:image.boot_regions)
            need(!intersect(at,bytes,start,length),
                 "ISO descriptor update overlaps El Torito data");
        for(const auto& [start,length]:image.iso_auxiliary_regions)
            need(!intersect(at,bytes,start,length),
                 "ISO descriptor update overlaps auxiliary data");
        plan.descriptor_fields.emplace_back(at,bytes);
    };
    auto be32=[](const Bytes& b,std::size_t at) {
        return (std::uint32_t(b[at])<<24) |
               (std::uint32_t(b[at+1])<<16) |
               (std::uint32_t(b[at+2])<<8) | b[at+3];
    };
    // Collect old path tables before authorizing any descriptor field.
    for(const auto& [lba,vd]:image.descriptors) {
        (void)lba;
        need(le(vd,80)==be32(vd,84) && le(vd,132)==be32(vd,136),
             "hybrid ISO descriptor byte orders disagree");
        const auto bytes=le(vd,132);
        if(!bytes)continue;
        need(le(vd,140) && be32(vd,148),"hybrid ISO path table is missing");
        const auto padded=(std::uint64_t(bytes)+block-1)/block*block;
        for(const auto offset:{140u,144u,148u,152u}) {
            const auto table=offset<148u?le(vd,offset):be32(vd,offset);
            if(!table)continue;
            const auto at=std::uint64_t(table)*block;
            within_volume(at,padded);
            image.iso_auxiliary_regions.emplace_back(at,padded);
        }
    }
    for(const auto& [lba,vd]:image.descriptors) {
        (void)vd;
        protect_field(std::uint64_t(lba)*block+80,8);
        protect_field(std::uint64_t(lba)*block+132,58);
    }
    auto check_extent=[&](std::uint64_t at,std::uint64_t bytes) {
        within_volume(at,bytes);
        if(bytes)plan.original_extents.emplace_back(at,bytes);
        for(const auto& [field,length]:plan.descriptor_fields)
            need(!intersect(at,bytes,field,length),
                 "ISO descriptor field aliases an existing namespace extent");
    };
    std::vector<const Node*> nodes{image.root.get()};
    for(std::size_t i=0;i<nodes.size();++i) {
        each_extent(*nodes[i],check_extent);
        for(const auto& child:nodes[i]->children)nodes.push_back(child.get());
        need(nodes.size()<=200000,"hybrid ISO namespace exceeds validation limit");
    }
    for(const auto& [at,bytes]:image.joliet_regions)check_extent(at,bytes);
    for(const auto& [at,bytes]:image.boot_regions)check_extent(at,bytes);
    for(const auto& [at,bytes]:image.iso_auxiliary_regions)check_extent(at,bytes);
    for(const auto& [at,bytes]:image.hybrid_protected_regions) {
        if(!bytes || at+bytes<=volume)continue;
        // Stop BEFORE the first independent partition, GPT entry array,
        // or GPT header. Never overwrite part of a 2048-byte sector.
        need(at>=volume,"ISO tail begins inside protected partition metadata");
        plan.limit=std::min(plan.limit,at/block);
    }
    need(plan.first<=plan.limit,
         "hybrid ISO exceeds its validated container");
    constexpr std::uint64_t max_padding=256ull*1024*1024;
    need((plan.limit-plan.first)*block<=max_padding,
         "hybrid ISO padding exceeds 256 MiB inspection budget");
    for(auto at=volume;at<plan.limit*block;) {
        edit_tick(progress);
        const auto count=static_cast<std::size_t>(
            std::min<std::uint64_t>(256u*1024u,plan.limit*block-at));
        const auto bytes=image.read(at,count);
        need(std::all_of(bytes.begin(),bytes.end(),[](auto c){return c==0;}),
             "nonzero bytes in hybrid ISO tail; cannot prove free space");
        at+=count;
    }
    return plan;
}
// Grow a standard GPT disk only by appending new ISO data beyond the
// entire original image. Existing partitions are never resized or moved.
// The old backup GPT is removed after its exact replacement is prepared.
struct GptGrowthPlan {
    std::uint64_t old_backup{},new_table{},new_header{},physical_size{};
    std::uint64_t logical_blocks{},mbr_length_at{};
    std::uint64_t trailer_blocks{};
    Bytes mbr,primary,backup,entries;
};
void set_gpt_growth_size(GptGrowthPlan& plan,std::uint64_t logical_blocks) {
    need(logical_blocks<0xffffffffull &&
         plan.trailer_blocks<0xffffffffull-logical_blocks,
         "GPT growth exceeds optical address space");
    plan.logical_blocks=logical_blocks;
    plan.physical_size=(logical_blocks+plan.trailer_blocks)*block;
    const auto table_sectors=plan.entries.size()/512u;
    const auto final_lba=plan.physical_size/512-1;
    plan.new_header=final_lba*512;
    plan.new_table=(final_lba-table_sectors)*512;
    need(plan.new_table>=logical_blocks*block,
         "new GPT backup overlaps remastered volume");
    auto put64le=[](Bytes& b,std::size_t at,std::uint64_t v) {
        put(b,at,static_cast<std::uint32_t>(v));
        put(b,at+4,static_cast<std::uint32_t>(v>>32));
    };
    auto refresh=[](Bytes& header) {
        put(header,16,0);
        put(header,16,gpt_crc32(Bytes(header.begin(),header.begin()+92)));
    };
    put(plan.mbr,static_cast<std::size_t>(plan.mbr_length_at),
        static_cast<std::uint32_t>(std::min<std::uint64_t>(final_lba,0xffffffffull)));
    put64le(plan.primary,32,final_lba);
    put64le(plan.primary,48,plan.new_table/512-1);
    refresh(plan.primary);
    put64le(plan.backup,24,final_lba);
    put64le(plan.backup,48,plan.new_table/512-1);
    put64le(plan.backup,72,plan.new_table/512);
    refresh(plan.backup);
}
GptGrowthPlan plan_gpt_growth(Image& image,const HybridTailPlan& tail,
                             std::uint64_t required,bool udf_mastering=false) {
    need(image.gpt_hybrid && image.udf_bridge==udf_mastering &&
         (!image.iso_system_use || (image.hybrid_rr_strict && image.rr_rebuild)) &&
         image.boot_regions_bounded,
         "GPT growth requires a validated, bounded ISO/Joliet or RRIP hybrid");
    GptGrowthPlan plan;
    plan.mbr=image.read(0,512);
    unsigned protective=0;
    for(std::size_t i=0;i<4;++i) {
        const auto at=446u+i*16u;
        if(plan.mbr[at+4]==0xee) {
            need(++protective==1,"duplicate protective MBR entry");
            plan.mbr_length_at=at+12;
        } else need(std::all_of(plan.mbr.begin()+static_cast<std::ptrdiff_t>(at),
                                plan.mbr.begin()+static_cast<std::ptrdiff_t>(at+16),
                                [](auto c){return c==0;}),
                    "GPT growth cannot resize a hybrid MBR container");
    }
    need(protective==1,"GPT growth requires a protective-only MBR");
    plan.primary=image.read(512,512);
    plan.backup=image.read(image.size-512,512);
    need(le(plan.primary,12)==92 && le(plan.backup,12)==92,
         "GPT growth requires standard 92-byte headers");
    const auto entries_bytes=std::uint64_t(le(plan.primary,80))*le(plan.primary,84);
    const auto table_sectors=(entries_bytes+511)/512;
    const auto old_table=le64(plan.backup,72);
    const auto old_last=image.size/512-1;
    need(table_sectors && table_sectors<=32768 &&
         old_table<old_last && table_sectors==old_last-old_table &&
         image.gpt_last_usable==old_table-1 &&
         old_table*512>=std::uint64_t(image.declared_volume_blocks)*block,
         "GPT backup is not a standard, separate trailing array");
    plan.old_backup=old_table*512;
    plan.entries=image.read(plan.old_backup,static_cast<std::size_t>(table_sectors*512));
    // Only the MBR size and primary GPT header may change in the system
    // area. Ensure no named ISO resource or boot data aliases them.
    auto check=[&](std::uint64_t at,std::uint64_t bytes) {
        need(!intersect(at,bytes,plan.mbr_length_at,4) &&
             !intersect(at,bytes,512,512) &&
             !intersect(at,bytes,plan.old_backup,image.size-plan.old_backup),
             "GPT growth metadata aliases live ISO or boot data");
    };
    // Inspect the original namespace, not the mutated working tree.
    // Otherwise a new file appears to occupy LBA zero, while a deleted
    // original extent disappears from the check entirely.
    for(const auto& [at,bytes]:tail.original_extents)check(at,bytes);
    const auto original=image.size/block;
    plan.trailer_blocks=(table_sectors+1+3)/4;
    need(required && original<0xffffffffull &&
         required<0xffffffffull-original,
         "GPT growth exceeds ISO address space");
    set_gpt_growth_size(plan,original+required);
    return plan;
}
void write_gpt_growth(std::fstream& out,const GptGrowthPlan& plan,
                      std::uint64_t original_size,std::uint64_t next,
                      const EditProgress& progress) {
    need(next==plan.logical_blocks,"GPT growth allocation differs from preflight");
    auto write_at=[&](std::uint64_t at,const Bytes& bytes) {
        edit_tick(progress);
        out.clear();out.seekp(static_cast<std::streamoff>(at));
        need(bool(out),"cannot seek GPT growth metadata");
        write(out,bytes);
    };
    write_at(0,plan.mbr);
    write_at(512,plan.primary);
    // The old table/header are outside all original ISO and partition
    // extents. Do not leave an obsolete backup claiming the old disk end.
    write_at(plan.old_backup,Bytes(static_cast<std::size_t>(
                                  original_size-plan.old_backup),0));
    write_at(next*block,Bytes(static_cast<std::size_t>(plan.new_table-next*block),0));
    write_at(plan.new_table,plan.entries);
    write_at(plan.new_header,plan.backup);
}
// Grow a conventional isohybrid container without relocating independent
// EFI partitions. Only explicit, consistent CHS geometry is accepted.
struct MbrGrowthPlan {
    std::uint64_t logical_blocks{},physical_size{};
    std::uint64_t alignment_blocks{},geometry_sectors{};
    std::size_t end_chs_at{},length_at{};
    Bytes mbr;
};
MbrGrowthPlan plan_mbr_growth(Image& image,const HybridTailPlan& tail,
                             std::uint64_t required,bool udf_mastering=false) {
    need(!image.gpt_hybrid && image.udf_bridge==udf_mastering &&
         (!image.iso_system_use || (image.hybrid_rr_strict && image.rr_rebuild)) &&
         image.boot_regions_bounded,
         "unsupported MBR growth layout");
    MbrGrowthPlan plan;
    plan.mbr=image.read(0,512);
    const auto original_sectors=image.size/512u;
    std::optional<std::size_t> container;
    EditProtectedRanges esp;
    for(std::size_t i=0;i<4;++i) {
        const auto at=446u+i*16u;
        const auto type=plan.mbr[at+4];
        if(!type) {
            need(std::all_of(plan.mbr.begin()+at,plan.mbr.begin()+at+16,
                            [](auto c){return c==0;}),
                 "MBR growth requires empty unused entries");
            continue;
        }
        const auto start=std::uint64_t(le(plan.mbr,at+8));
        const auto count=std::uint64_t(le(plan.mbr,at+12));
        if(type==0x17 && !start) {
            need(!container && plan.mbr[at]==0x80 &&
                 count==original_sectors && plan.mbr[at+1]==0 &&
                 plan.mbr[at+2]==1 && plan.mbr[at+3]==0,
                 "MBR growth requires a complete active LBA-zero ISO container");
            container=at;
        } else {
            need(type==0xef && plan.mbr[at]==0 &&
                 start>=16ull*block/512u && count &&
                 start<original_sectors && count<=original_sectors-start,
                 "MBR growth cannot relocate unknown independent partitions");
            for(const auto& [first,length]:esp)
                need(!intersect(start,count,first,length),"overlapping MBR EFI partitions");
            esp.emplace_back(start,count);
        }
    }
    need(container.has_value(),"MBR growth lacks a validated ISO container");
    const auto at=*container;
    const auto heads=std::uint64_t(plan.mbr[at+5])+1;
    const auto sectors=std::uint64_t(plan.mbr[at+6]&0x3fu);
    const auto cylinder=std::uint64_t(plan.mbr[at+7]) |
                        (std::uint64_t(plan.mbr[at+6]&0xc0u)<<2u);
    need(heads>=2 && heads<=255 && sectors>=1 && sectors<=63,
         "MBR growth requires explicit end CHS geometry");
    const auto geometry=heads*sectors;
    need(original_sectors%geometry==0 && original_sectors/geometry>0 &&
         cylinder==std::min<std::uint64_t>(original_sectors/geometry-1,1023),
         "MBR container length and end CHS disagree");
    // Align both to a cylinder and to an optical sector.
    const auto alignment=geometry*(geometry%4==0?1u:geometry%2==0?2u:4u);
    plan.alignment_blocks=alignment/4;
    plan.geometry_sectors=geometry;
    const auto original=image.size/block;
    need(required && required<0xffffffffull-original,
         "MBR growth exceeds ISO addressing limit");
    plan.logical_blocks=original+required;
    const auto minimum=plan.logical_blocks*4u;
    const auto rounded=((minimum+alignment-1)/alignment)*alignment;
    need(rounded<=0xffffffffull,"MBR growth exceeds 32-bit partition length");
    plan.physical_size=rounded*512u;
    plan.end_chs_at=at+5;plan.length_at=at+12;
    for(const auto& [begin,length]:tail.original_extents)
        need(!intersect(begin,length,plan.end_chs_at,3) &&
             !intersect(begin,length,plan.length_at,4),
             "MBR growth fields alias live ISO data");
    const auto last=std::min<std::uint64_t>(rounded/geometry-1,1023);
    plan.mbr[at+6]=static_cast<unsigned char>(sectors|((last>>2)&0xc0u));
    plan.mbr[at+7]=static_cast<unsigned char>(last&0xffu);
    put(plan.mbr,plan.length_at,static_cast<std::uint32_t>(rounded));
    return plan;
}
void write_mbr_growth(std::fstream& out,const MbrGrowthPlan& plan,
                      std::uint64_t next,const EditProgress& progress) {
    need(next==plan.logical_blocks && next*block<=plan.physical_size,
         "MBR allocation differs from preflight");
    out.clear();out.seekp(static_cast<std::streamoff>(next*block));
    need(bool(out),"cannot seek MBR alignment padding");
    for(auto left=plan.physical_size-next*block;left;) {
        edit_tick(progress);
        const auto amount=static_cast<std::size_t>(
            std::min<std::uint64_t>(left,256u*1024u));
        write(out,Bytes(amount,0));left-=amount;
    }
    out.clear();out.seekp(0);need(bool(out),"cannot seek MBR header");
    write(out,plan.mbr);
}
void protect_hybrid_target(const Node& node,const Image& image) {
    each_extent(node,[&](std::uint64_t at,std::uint64_t bytes) {
        for(const auto& [start,length]:image.hybrid_protected_regions)
            need(!intersect(at,bytes,start,length),
                 "hybrid ISO target aliases a protected partition");
        for(const auto& [start,length]:image.boot_regions)
            need(!intersect(at,bytes,start,length),
                 "hybrid ISO target aliases an El Torito resource");
    });
    for(const auto& child:node.children)protect_hybrid_target(*child,image);
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
        need(node && !node->directory && node->size>0 &&
             node->sections.size()==1 &&
             node->sections.front().length==node->size,
             "hybrid payload patch requires one nonempty physical extent");
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
            each_extent(*other,[&](auto start,auto length) {
                need(!intersect(offset,bytes,start,length),
                     "hybrid replacement overlaps another ISO entry or alias");
            });
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
            need(node->sections.size()==1 &&
                 std::uint64_t(node->sections.front().lba)*block==offset &&
                 node->sections.front().length==entry.size &&
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
            each_extent(*other,[&](auto start,auto length) {
                need(!intersect(offset,entry.size,start,length),
                     "bridge file overlaps ISO directory, boot image or other entry");
            });
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
                           const Image& before,const EditProgress& progress,
                           const MbrGrowthPlan* mbr_growth=nullptr,
                           const GptGrowthPlan* gpt_growth=nullptr) {
    need(before.udf_bridge && before.boot_regions_bounded,
         "bootable ISO/UDF validation needs bounded boot extents");
    const auto stage_size=std::filesystem::file_size(staged);
    const bool mastering=stage_size>before.size;
    need(!before.hybrid_system_area || stage_size==before.size ||
         (mastering && mbr_growth && !before.gpt_hybrid &&
          mbr_growth->physical_size==stage_size) ||
         (mastering && gpt_growth && before.gpt_hybrid &&
          gpt_growth->physical_size==stage_size),
         "partitioned ISO/UDF transaction has unplanned physical growth");
    need(std::filesystem::file_size(original)==before.size &&
         stage_size>=before.size,
         "ISO/UDF image size changed during UDF edit");
    Image after(staged,true);
    auto expected_regions=before.hybrid_protected_regions;
    if(gpt_growth) {
        for(auto& [at,length]:expected_regions) {
            if(at==before.size-512 && length==512)
                at=gpt_growth->new_header;
            else if(at==gpt_growth->old_backup &&
                    length==gpt_growth->entries.size())
                at=gpt_growth->new_table;
        }
        need(after.read(0,512)==gpt_growth->mbr &&
             after.read(512,512)==gpt_growth->primary &&
             after.read(gpt_growth->new_table,gpt_growth->entries.size())==
                 gpt_growth->entries &&
             after.read(gpt_growth->new_header,512)==gpt_growth->backup,
             "GPT/UDF staged partition metadata differs from growth plan");
        const auto old=after.read(gpt_growth->old_backup,
                         static_cast<std::size_t>(before.size-gpt_growth->old_backup));
        need(std::all_of(old.begin(),old.end(),[](auto c){return c==0;}),
             "obsolete GPT/UDF backup was not cleared");
    }
    need(after.udf_bridge && after.boot_regions_bounded &&
         after.size==stage_size &&
         after.descriptors==before.descriptors &&
         after.boot_regions==before.boot_regions &&
         after.boot_lbas==before.boot_lbas &&
         after.hybrid_system_area==before.hybrid_system_area &&
         after.gpt_hybrid==before.gpt_hybrid &&
         after.hybrid_protected_regions==expected_regions &&
         after.gpt_first_usable==before.gpt_first_usable &&
         after.gpt_last_usable==(gpt_growth?gpt_growth->new_table/512-1:
                                 before.gpt_last_usable),
         "ISO/Joliet, partition geometry or El Torito changed in UDF staging");

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
    if(gpt_growth) {
        need(mastering,"GPT/UDF geometry changes require append mastering");
        guard.protect(1024,256ull*block-1024);
    } else if(mbr_growth) {
        need(mastering && after.read(0,512)==mbr_growth->mbr,
             "MBR/UDF staged header differs from growth plan");
        guard.protect(512,256ull*block-512);
    } else guard.protect(0,256ull*block); // ISO PVD, VRS, boot/system area
    for(const auto& [start,length]:before.hybrid_protected_regions) {
        if(gpt_growth &&
           ((start==512 && length==512) ||
            (start==before.size-512 && length==512) ||
            (start==gpt_growth->old_backup &&
             length==gpt_growth->entries.size())))continue;
        guard.protect(start,length);
    }
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
        if(!gpt_growth)verify_anchor(stage_size/block-1);
        verify_anchor(stage_size/block-257);
        need((gpt_growth || after.udf_anchor_lbas.count(stage_size/block-1)!=0) &&
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
        each_extent(*node,[&](auto start,auto length) {
            if(length)guard.protect(start,length);
        });
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
// The sequential writer checks the complete old prefix independently.
// Unlike ROM mastering, a clean VAT transaction ends in a VAT ICB,
// not an AVDP. Do not require or manufacture ROM end anchors here.
void validate_vat_bridge_envelope(const std::filesystem::path& staged,
                                   const Image& image,bool mbr=false) {
    Image after(staged,true);
    need(after.udf_bridge && after.hybrid_system_area==mbr &&
         image.hybrid_system_area==mbr && !after.gpt_hybrid && !image.gpt_hybrid &&
         after.hybrid_protected_regions==image.hybrid_protected_regions &&
         after.boot_regions_bounded && after.size>image.size &&
         after.descriptors==image.descriptors &&
         after.boot_regions==image.boot_regions &&
         after.boot_entries==image.boot_entries &&
         after.boot_lbas==image.boot_lbas &&
         after.boot_descriptor_lbas==image.boot_descriptor_lbas,
         "staged VAT bridge changed ISO/El Torito geometry");
}
GptGrowthPlan plan_vat_gpt_bridge(Image& image,const EditProtectedRanges& immutable);
void validate_vat_gpt_bridge(const std::filesystem::path& staged,
                             const Image& before,GptGrowthPlan plan,bool remastered=false);

void coordinate_shared_bridge_stage(
        const std::filesystem::path& path,const std::filesystem::path& staged,
        const Image& image,const std::map<std::string,const Node*>& targets,
        const CoordinatedIsoUpdate& plan,
        const SharedIsoDeleteSectors& deletion_sectors,
        const EditProgress& progress,
        const MbrGrowthPlan* mbr_growth=nullptr,
        const GptGrowthPlan* gpt_growth=nullptr,
        bool sequential_vat=false) {
    if(sequential_vat) {
        need(!mbr_growth && image.hybrid_system_area==(gpt_growth!=nullptr) &&
             image.gpt_hybrid==(gpt_growth!=nullptr),
             "unsupported sequential shared-file envelope");
        if(gpt_growth)validate_vat_gpt_bridge(staged,image,*gpt_growth);
        else validate_vat_bridge_envelope(staged,image);
    } else protect_iso_udf_stage(path,staged,image,progress,mbr_growth,gpt_growth);
    const auto size=std::filesystem::file_size(staged);
    need(size>image.size && size%block==0 && size/block<0xffffffffu,
         "shared ISO/UDF resize did not produce bounded append mastering");
    const auto volume_blocks=gpt_growth?
        (sequential_vat?size/block-gpt_growth->trailer_blocks:gpt_growth->logical_blocks):
        size/block;
    const auto payload_limit=volume_blocks*block;
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
                 start<=payload_limit && item->size<=payload_limit-start,
                 "shared replacement must occupy newly appended optical blocks");
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
        both(vd,80,static_cast<std::uint32_t>(volume_blocks));
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
    if(sequential_vat && gpt_growth)
        validate_vat_gpt_bridge(staged,image,*gpt_growth,true);
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
    need(final_udf.format()=="UDF" &&
         (!sequential_vat || final_udf.udf_has_virtual_partition()),
         "coordinated image lost its UDF namespace");
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
    std::map<std::string,Node*> replaced;
    // Partial GPT mastering preserves every independent UDF entry.
    std::map<std::string,bool> independent_udf_paths;
    std::size_t removed=0;
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
        CoordinatedIsoUpdate& plan,SharedTreePlan& tree,bool sequential_vat=false) {
    if(image.iso_system_use || changes.empty() ||
       (!sequential_vat && !std::all_of(changes.begin(),changes.end(),[&](const ArchiveChange& c){
           return !c.remove && !iso_files.count(upper(edit_path(c.path)));
       })))return false;
    std::map<std::string,bool> original_udf_paths,independent;
    std::set<std::string> udf_names;
    const bool partial_gpt=sequential_vat && image.gpt_hybrid;
    {
        const Disc udf(path);
        if(udf.format()!="UDF" ||
           (!partial_gpt && udf.items().size()!=iso_files.size()))return false;
        std::size_t common_count=0;
        bool common_data=false;
        for(const auto& item:udf.items()) {
            const auto key=upper(item.path);
            const auto at=iso_files.find(key);
            if(!udf_names.insert(key).second)return false;
            if(sequential_vat &&
               !original_udf_paths.emplace(item.path,item.directory).second)return false;
            if(at==iso_files.end()) {
                if(!partial_gpt)return false;
                independent.emplace(item.path,item.directory);
                continue;
            }
            if(at->second->directory!=item.directory)return false;
            ++common_count;
            if(item.directory)continue;
            const auto* n=at->second;
            if(n->size!=item.size || !item.size || !item.inline_data.empty() ||
               n->sections.size()!=1 || item.extents.size()!=1 ||
               item.extents[0].sparse ||
               item.extents[0].offset!=std::uint64_t(n->lba)*block ||
               item.extents[0].length!=item.size)return false;
            common_data=true;
        }
        // A fully mirrored directory-only namespace remains writable
        // after its last regular file is removed. Type-1 keeps its
        // original requirement for a demonstrated shared payload.
        if(common_count!=iso_files.size() ||
           (!common_data && (!sequential_vat || !independent.empty())))return false;
        if(sequential_vat && !udf.udf_has_virtual_partition())return false;
    }
    if(!independent.empty()) {
        // Root placement in a mixed image is ambiguous. Author only
        // genuinely new entries within an already shared directory,
        // or within new descendants of that directory in this batch.
        // Existing-file edits retain the narrower shared-file route.
        std::map<std::string,bool> additions;
        for(const auto& change:changes) {
            const auto key=edit_path(change.path);
            const auto folded=upper(key);
            if(change.remove || udf_names.count(folded) ||
               !additions.emplace(folded,change.directory).second)return false;
        }
        for(const auto& [key,directory]:additions) {
            (void)directory;
            auto ancestor=key;
            for(;;) {
                const auto slash=ancestor.rfind('/');
                if(slash==std::string::npos)return false;
                ancestor.resize(slash);
                const auto existing=iso_files.find(ancestor);
                if(existing!=iso_files.end()) {
                    if(!existing->second->directory)return false;
                    break;
                }
                const auto created=additions.find(ancestor);
                if(created==additions.end() || !created->second)return false;
            }
        }
        tree.independent_udf_paths=std::move(independent);
    }
    if(sequential_vat) {
        // Do not infer a mirrored tree from the merged Joliet listing.
        // Independently confirm the same names and extents in BOTH views.
        // parse_iso can append a synthetic boot.images folder; it is not
        // an on-disc namespace entry when the ISO tree has no such root.
        for(bool primary:{true,false}) {
            const auto view=inspect_iso_namespace(path,primary);
            if(view.format!="ISO 9660" && view.format!="Joliet")return false;
            std::set<std::string> seen;
            for(const auto& item:view.items) {
                const auto key=upper(item.path);
                if(!image.boot_descriptor_lbas.empty() &&
                   !iso_files.count("BOOT.IMAGES") &&
                   (key=="BOOT.IMAGES" || key.starts_with("BOOT.IMAGES/")))continue;
                const auto at=iso_files.find(key);
                if(!seen.insert(key).second || at==iso_files.end() ||
                   item.directory!=at->second->directory)return false;
                if(!item.directory &&
                   (item.size!=at->second->size || item.extents.size()!=1 ||
                    item.extents[0].sparse ||
                    item.extents[0].offset!=std::uint64_t(at->second->lba)*block ||
                    item.extents[0].length!=item.size))return false;
            }
            if(seen.size()!=iso_files.size())return false;
        }
    } else {
        const auto inspection=inspect_udf_edit(path,true,image.hybrid_system_area);
        if(!inspection.parser_compatible || inspection.partition_access_type!=1 ||
           inspection.allocation_bitmap || inspection.allocation_table)return false;
    }
    // For a proven mirrored VAT namespace, expand selected directory
    // removals independently from the strict UDF mutation planner.
    // Keep the original exact requests as a distinct contract, so a
    // case alias or a missing descendant cannot be silently coalesced.
    std::vector<ArchiveChange> expanded;
    if(sequential_vat) {
        need(changes.size()<=4096,"VAT mirrored request exceeds batch limit");
        std::set<std::string> names;
        std::map<std::string,bool> selected;
        for(const auto& change:changes) {
            auto key=edit_path(change.path);
            need(names.insert(upper(key)).second,
                 "duplicate or case-colliding mirrored VAT request");
            if(change.remove) {
                const auto at=original_udf_paths.find(key);
                need(at!=original_udf_paths.end() &&
                     (!change.directory || at->second),
                     "mirrored VAT removal target does not exist or has wrong type");
                selected.emplace(key,at->second);
                plan.shared_deletions.insert(key);
            } else {
                auto normalized=change;
                normalized.path=std::move(key);
                expanded.push_back(std::move(normalized));
            }
        }
        for(const auto& [key,directory]:selected) {
            if(!plan.vat_tree_deletions.insert(key).second)continue;
            if(!directory)continue;
            const auto prefix=key+"/";
            for(auto at=original_udf_paths.lower_bound(prefix);
                at!=original_udf_paths.end() && at->first.starts_with(prefix);++at)
                plan.vat_tree_deletions.insert(at->first);
        }
        need(plan.vat_tree_deletions.size()<=200000,
             "VAT mirrored recursive deletion exceeds namespace limit");
        std::set<std::string> removed_folded;
        for(const auto& key:plan.vat_tree_deletions)
            removed_folded.insert(upper(key));
        for(const auto& change:expanded) {
            auto ancestor=upper(change.path);
            for(;;) {
                need(!removed_folded.count(ancestor),
                     "VAT mirrored addition/replacement conflicts with deleted subtree");
                const auto slash=ancestor.rfind('/');
                if(slash==std::string::npos)break;
                ancestor.resize(slash);
            }
        }
        for(const auto& key:plan.vat_tree_deletions)
            expanded.push_back({key,{},true,original_udf_paths.at(key)});
    }
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
    const auto& operations=sequential_vat?expanded:changes;
    sorted.reserve(operations.size());
    for(const auto& c:operations)sorted.push_back(&c);
    std::stable_sort(sorted.begin(),sorted.end(),[](const auto* a,const auto* b){
        if(a->remove!=b->remove)return a->remove;
        const auto da=std::count(a->path.begin(),a->path.end(),'/');
        const auto db=std::count(b->path.begin(),b->path.end(),'/');
        return a->remove?da>db:da<db;
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
        if(sequential_vat) {
            const auto existing=index.find(upper(key));
            if(existing!=index.end()) {
                auto* n=existing->second;
                // Descendant removals were expanded before cloning.
                // An existing directory can only be unlinked once its
                // children have been removed from this private tree.
                need(n->parent==parent &&
                     (c->remove?(n->directory==c->directory && n->children.empty()):
                                (!n->directory && !c->directory)),
                     "VAT mirrored target has invalid type or surviving children");
                protect_boot(*n,image.boot_lbas);
                if(image.hybrid_system_area)protect_hybrid_target(*n,image);
                each_extent(*n,[&](auto start,auto length) {
                    for(const auto& [at,bytes]:image.boot_regions)
                        need(!intersect(start,length,at,bytes),
                             "VAT mirrored file overlaps protected boot data");
                });
                if(c->remove) {
                    need(plan.vat_tree_deletions.count(key)!=0,
                         "VAT mirrored removal differs from expanded contract");
                    if(n->directory) {
                        // No path tables have been assigned yet. Remove
                        // all non-owning references before freeing Node.
                        if(image.joliet) {
                            const auto at=std::uint64_t(le(n->joliet_record,2))*block;
                            const auto bytes=le(n->joliet_record,10);
                            for(const auto* ranges:{&image.boot_regions,
                                                    &image.hybrid_protected_regions})
                                for(const auto& [start,length]:*ranges)
                                    need(!intersect(at,bytes,start,length),
                                         "VAT mirrored Joliet directory overlaps boot/partition data");
                        }
                        tree.dots.erase(n);
                        tree.dirs.erase(std::remove(tree.dirs.begin(),tree.dirs.end(),n),
                                        tree.dirs.end());
                    }
                    ++tree.removed;
                    auto child=std::find_if(parent->children.begin(),parent->children.end(),
                        [&](const auto& p){return p.get()==n;});
                    need(child!=parent->children.end(),"missing mirrored deletion target");
                    index.erase(existing);
                    parent->children.erase(child);
                } else {
                    need(std::filesystem::is_regular_file(
                             std::filesystem::symlink_status(c->source)),
                         "VAT shared replacement source must be a regular file");
                    n->size=std::filesystem::file_size(c->source);
                    need(n->size>0 && n->size<=0xffffffffull,
                         "VAT mirrored replacement exceeds single ISO extent");
                    plan.shared_replacements.insert(key);
                    tree.replaced.emplace(key,n);
                }
                continue;
            }
            need(!c->remove,"VAT mirrored removal target does not exist");
            const auto top=upper(key.substr(0,key.find('/')));
            need(image.boot_descriptor_lbas.empty() || top!="BOOT.IMAGES",
                 "VAT mirrored addition conflicts with virtual boot namespace");
        }
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
            need(!sequential_vat && image.joliet,
                 "shared name cannot be represented in ISO9660 without an alias");
            for(unsigned salt=0;salt<128;++salt) {
                n->identifier=identifier(iso_alias(component,n->directory,salt),
                                         false,n->directory);
                if(!collision(n->identifier,0))break;
            }
        }
        need(!collision(n->identifier,0),"shared primary ISO name collision");
        n->name=decode(n->identifier,false,n->directory);
        need(!sequential_vat || upper(n->name)==upper(component),
             "VAT mirrored primary identifier must match the UDF component");
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
        SharedTreePlan& tree,const EditProgress& progress,
        const MbrGrowthPlan* mbr_growth=nullptr,
        const GptGrowthPlan* gpt_growth=nullptr,
        bool sequential_vat=false) {
    if(sequential_vat) {
        // VAT already relocated and independently checked GPT.
        // Never use ROM end-anchor growth for sequential media.
        need(!mbr_growth && image.gpt_hybrid==(gpt_growth!=nullptr),
             "invalid sequential VAT remaster envelope");
        if(gpt_growth)validate_vat_gpt_bridge(staged,image,*gpt_growth);
        else validate_vat_bridge_envelope(staged,image,image.hybrid_system_area);
    } else protect_iso_udf_stage(path,staged,image,progress,mbr_growth,gpt_growth);
    const auto size=std::filesystem::file_size(staged);
    need(size>image.size && size%block==0 && size/block<0xffffffffu,
         "shared addition requires append mastering");
    const auto volume_blocks=gpt_growth?
        (sequential_vat?size/block-gpt_growth->trailer_blocks:gpt_growth->logical_blocks):
        size/block;
    const unsigned views=image.joliet?2u:1u;
    std::uint64_t first=0,main=0;
    if(sequential_vat) {
        // The VAT planner places an unmapped, zero-filled ISO metadata
        // reservation immediately before the optional external VAT table
        // and final VAT ICB. Derive its location from that final ICB.
        Image mapped(staged,true);
        const auto fe=mapped.read((volume_blocks-1)*block,block);
        need(le(fe,0,2)==261 && (fe[27]==0 || fe[27]==248),
             "missing final sequential VAT ICB");
        const auto mode=le(fe,34,2);
        const auto count=std::uint64_t(le(fe,64)) |
                         (std::uint64_t(le(fe,68))<<32);
        need((mode==3 && !count) ||
             (mode==0 && le(fe,172)==8 && count && count<=8192),
             "unsupported final sequential VAT storage");
        need(count<volume_blocks && plan.iso_metadata_blocks &&
             plan.iso_metadata_blocks<=volume_blocks-1-count,
             "invalid sequential ISO metadata reservation");
        main=volume_blocks-1-count;
        first=main-plan.iso_metadata_blocks;
        need(first>=image.size/block,"sequential ISO reservation overlaps source");
        if(mode==0) {
            const auto anchor=mapped.read(256ull*block,block);
            const auto vds=le(anchor,20);
            const auto vds_blocks=le(anchor,16)/block;
            bool match=false;
            for(std::uint64_t j=0;j<vds_blocks;++j) {
                const auto d=mapped.read((std::uint64_t(vds)+j)*block,block);
                if(le(d,0,2)==8)break;
                if(le(d,0,2)==5) {
                    need(!match && std::uint64_t(le(d,188))+le(fe,180)==main,
                         "VAT table is not directly after ISO reservation");
                    match=true;
                }
            }
            need(match,"missing sequential physical partition");
        }
    } else {
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
        auto changed=tree.added;
        changed.insert(tree.replaced.begin(),tree.replaced.end());
        for(const auto& [key,n]:changed) {
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
            if(tree.replaced.count(key)) {
                both(n->record,2,n->lba);
                both(n->record,10,static_cast<std::uint32_t>(n->size));
                if(image.joliet) {
                    both(n->joliet_record,2,n->lba);
                    both(n->joliet_record,10,static_cast<std::uint32_t>(n->size));
                }
            }
        }
        if(sequential_vat)for(const auto& item:udf.items())
            for(const auto& e:item.extents)
                need(e.sparse || !intersect(e.offset,e.length,first*block,
                                            (main-first)*block),
                     "sequential ISO reservation overlaps live UDF extent");
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
        both(vd,80,static_cast<std::uint32_t>(volume_blocks));
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
    if(sequential_vat) {
        if(gpt_growth)validate_vat_gpt_bridge(staged,image,*gpt_growth,true);
        else need(checked.hybrid_system_area==image.hybrid_system_area &&
                  !checked.gpt_hybrid &&
                  checked.hybrid_protected_regions==image.hybrid_protected_regions,
                  "VAT mirrored remaster changed independent partitions");
    }
    const auto after=index_iso_tree(*checked.root);
    const auto wanted=index_iso_tree(*tree.root);
    need(tree.removed<=original.size() &&
         after.size()==original.size()-tree.removed+tree.added.size() &&
         after.size()==wanted.size(),
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
    need(udf.format()=="UDF" &&
         (!sequential_vat || udf.udf_has_virtual_partition()),
         "mirrored remaster lost UDF namespace");
    if(sequential_vat) {
        need(udf.items().size()==wanted.size()+tree.independent_udf_paths.size(),
             "VAT coordinated namespace size differs");
        std::set<std::string> seen;
        for(const auto& item:udf.items()) {
            const auto key=upper(item.path);
            const auto found=wanted.find(key);
            need(seen.insert(key).second,"duplicate staged VAT path");
            const auto independent=tree.independent_udf_paths.find(item.path);
            if(independent!=tree.independent_udf_paths.end()) {
                need(found==wanted.end() && independent->second==item.directory,
                     "independent VAT entry became a shared ISO entry");
                // The VAT backend subsequently verifies its complete
                // original layout and checks the protected old prefix.
                continue;
            }
            need(found!=wanted.end() && found->second->directory==item.directory,
                 "VAT mirrored namespace differs from ISO plan");
            if(item.directory)continue;
            const auto* n=found->second;
            need(item.size==n->size && item.inline_data.empty() &&
                 !item.extents.empty(),"VAT mirrored file size differs");
            auto at=std::uint64_t(n->lba)*block;
            const auto end=at+n->size;
            for(const auto& e:item.extents) {
                need(!e.sparse && e.offset==at && e.length<=end-at,
                     "VAT mirrored file is not the planned ISO extent");
                at+=e.length;
            }
            need(at==end,"VAT mirrored file extent is incomplete");
        }
    }
    auto changed=tree.added;
    changed.insert(tree.replaced.begin(),tree.replaced.end());
    for(const auto& [key,n]:changed) {
        const auto found=std::find_if(udf.items().begin(),udf.items().end(),
            [&](const Item& item){return item.path==key;});
        need(found!=udf.items().end() && found->directory==n->directory &&
             (n->directory || (found->size==n->size && !found->extents.empty() &&
                found->extents.front().offset==std::uint64_t(n->lba)*block)),
             "final shared ISO/Joliet/UDF mapping differs");
    }
}
// The synchronous UDF transaction may call back into the ISO coordinator.
// Own its complete plan independently of the planning stack frame. Keep
// partition preflight, protected-range enumeration and staged verification
// in separate functions. MSVC x64 ASan reported use-after-poison at the
// edit_udf call in the previously monolithic function. Do not suppress
// instrumentation or weaken any original readback/protection checks.
struct BridgeMetadataEdit {
    IsoNodeIndex iso_files;
    CoordinatedIsoUpdate plan;
    SharedIsoDeleteSectors deletion_sectors;
    SharedTreePlan add_tree;
    IsoNodeIndex shared;
    std::optional<MbrGrowthPlan> mbr_append;
    std::optional<GptGrowthPlan> gpt_append;
    bool adding_shared=false;
};

std::unique_ptr<BridgeMetadataEdit> prepare_bridge_metadata_edit(
        const std::filesystem::path& path,Image& image,
        const std::vector<ArchiveChange>& changes,bool sequential_vat=false) {
    auto edit=std::make_unique<BridgeMetadataEdit>();
    need(image.udf_bridge && image.boot_regions_bounded,
         "ISO/UDF metadata edit requires bounded El Torito boot images");
    // Shared regular files can be remastered or unlinked from both
    // namespaces; ISO-only and ambiguous aliases remain protected.
    auto& iso_files=edit->iso_files;
    auto gather=[&](auto&& self,const Node& node,const std::string& prefix)->void {
        for(const auto& child:node.children) {
            const auto key=prefix.empty()?child->logical:prefix+"/"+child->logical;
            need(iso_files.emplace(upper(key),child.get()).second,
                 "ambiguous ISO/UDF path");
            if(child->directory)self(self,*child,key);
        }
    };
    gather(gather,*image.root,"");
    if(image.hybrid_system_area)
        need(!image.iso_system_use,
             "partitioned UDF metadata edits require plain ISO/Joliet");
    auto& plan=edit->plan;
    auto& deletion_sectors=edit->deletion_sectors;
    auto& add_tree=edit->add_tree;
    auto& shared=edit->shared;
    edit->adding_shared=plan_shared_additions(path,image,iso_files,changes,
                                              plan,add_tree,sequential_vat);
    const bool adding_shared=edit->adding_shared;
    if(sequential_vat && !adding_shared)return nullptr;
    const bool existing_shared=!adding_shared && std::any_of(changes.begin(),changes.end(),
        [&](const ArchiveChange& c){
            return iso_files.count(upper(edit_path(c.path)))!=0;
        });
    if(image.hybrid_system_area && !adding_shared && !existing_shared) {
        // Never downgrade a common namespace to a UDF-only addition
        // when the mirrored planner rejects an unsupported layout.
        const Disc udf(path);
        need(udf.format()=="UDF" &&
             std::any_of(udf.items().begin(),udf.items().end(),[&](const Item& item) {
                 return !item.directory && !iso_files.count(upper(item.path));
             }),
             "partitioned ISO/UDF requires coordinated or demonstrably split namespace");
    }
    if(existing_shared) {
        need(!image.iso_system_use,
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
                 node->sections.size()==1 &&
                 node->sections.front().length==node->size &&
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
            if(image.hybrid_system_area)protect_hybrid_target(*node,image);
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
                                   &image.iso_auxiliary_regions,
                                   &image.hybrid_protected_regions})
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
    // ROM mastering may grow a validated MBR or protective-only GPT.
    // Do not alter the fixed-size allocator route on rewritable media.
    auto& mbr_append=edit->mbr_append;
    auto& gpt_append=edit->gpt_append;
    if(image.hybrid_system_area && !sequential_vat) {
        const auto info=inspect_udf_edit(path,true,true);
        if(info.parser_compatible && info.partition_access_type==1 &&
           !info.allocation_bitmap && !info.allocation_table) {
            HybridTailPlan snapshot;
            for(const auto& [key,node]:iso_files) {
                (void)key;
                each_extent(*node,[&](auto at,auto length) {
                    if(length)snapshot.original_extents.emplace_back(at,length);
                });
            }
            each_extent(*image.root,[&](auto at,auto length) {
                if(length)snapshot.original_extents.emplace_back(at,length);
            });
            for(const auto* ranges:{&image.boot_regions,&image.joliet_regions,
                                   &image.udf_metadata_regions,
                                   &image.iso_auxiliary_regions})
                snapshot.original_extents.insert(snapshot.original_extents.end(),
                                                 ranges->begin(),ranges->end());
            if(image.gpt_hybrid) {
                // The GPT metadata itself is intentionally relocated;
                // independent partition extents are not.
                const auto backup=image.read(image.size-512,512);
                const auto old_table=le64(backup,72)*512;
                for(const auto& range:image.hybrid_protected_regions)
                    if(range.first>=16ull*block && range.first<old_table)
                        snapshot.original_extents.push_back(range);
                gpt_append=plan_gpt_growth(image,snapshot,1,true);
                need(gpt_append->trailer_blocks<256,
                     "GPT backup is too large for separate UDF end anchor");
            } else {
                snapshot.original_extents.insert(snapshot.original_extents.end(),
                    image.hybrid_protected_regions.begin(),
                    image.hybrid_protected_regions.end());
                mbr_append=plan_mbr_growth(image,snapshot,1,true);
            }
        }
    }
    return edit;
}

void validate_bridge_metadata_stage(
        const std::filesystem::path& path,const std::filesystem::path& staged,
        const Image& image,BridgeMetadataEdit& edit,const EditProgress& progress) {
    const auto& gpt_append=edit.gpt_append;
    const auto& mbr_append=edit.mbr_append;
    const auto& plan=edit.plan;
    const auto& shared=edit.shared;
    const auto& iso_files=edit.iso_files;
    auto& add_tree=edit.add_tree;
    const auto& deletion_sectors=edit.deletion_sectors;
    const bool adding_shared=edit.adding_shared;
    std::optional<GptGrowthPlan> gpt_growth;
    std::optional<MbrGrowthPlan> mbr_growth;
    if(gpt_append && std::filesystem::file_size(staged)>image.size) {
        gpt_growth=*gpt_append;
        auto& growth=*gpt_growth;
        const auto bytes=std::filesystem::file_size(staged);
        need(bytes%block==0,"GPT/UDF mastering produced unaligned volume");
        set_gpt_growth_size(growth,bytes/block);
        {
            std::fstream out(staged,std::ios::in|std::ios::out|std::ios::binary);
            need(bool(out),"cannot update staged GPT/UDF metadata");
            write_gpt_growth(out,growth,image.size,bytes/block,progress);
            out.flush();
            need(bool(out),"staged GPT/UDF flush failed");
        }
    } else if(mbr_append && std::filesystem::file_size(staged)>image.size) {
        // UDF already verified the staged filesystem. Update only
        // the MBR container CHS/length before the independent ISO
        // validator authorizes the single atomic commit.
        mbr_growth=*mbr_append;
        auto& growth=*mbr_growth;
        growth.physical_size=std::filesystem::file_size(staged);
        const auto sectors=growth.physical_size/512u;
        need(growth.physical_size%block==0 &&
             sectors<=0xffffffffull &&
             (growth.physical_size/block)%growth.alignment_blocks==0,
             "UDF mastering did not honor MBR geometry");
        const auto cylinder=std::min<std::uint64_t>(
             sectors/growth.geometry_sectors-1,1023);
        auto& header=growth.mbr;
        header[growth.end_chs_at+1]=static_cast<unsigned char>(
            (header[growth.end_chs_at+1]&0x3fu)|((cylinder>>2)&0xc0u));
        header[growth.end_chs_at+2]=static_cast<unsigned char>(cylinder&0xffu);
        put(header,growth.length_at,static_cast<std::uint32_t>(sectors));
        edit_tick(progress);
        {
            std::fstream out(staged,std::ios::in|std::ios::out|std::ios::binary);
            need(bool(out),"cannot update staged MBR/UDF header");
            write(out,header);
            out.flush();
            need(bool(out),"staged MBR/UDF header flush failed");
        }
    }
    const auto* mbr=mbr_growth?&*mbr_growth:nullptr;
    const auto* gpt=gpt_growth?&*gpt_growth:nullptr;
    if(adding_shared)
        coordinate_shared_add_stage(path,staged,image,iso_files,plan,
                                    add_tree,progress,mbr,gpt);
    else if(shared.empty())
        protect_iso_udf_stage(path,staged,image,progress,mbr,gpt);
    else coordinate_shared_bridge_stage(path,staged,image,shared,plan,
                                        deletion_sectors,progress,mbr,gpt);
}

EditProtectedRanges bridge_metadata_protected_ranges(const Image& image) {
    // UDF may advertise free blocks that ISO9660 owns exclusively, e.g.
    // boot.catalog, BIOS/EFI image extents or a shared old ISO filename.
    // Exclude them *before* UDF's allocation scan so a new file uses a
    // genuinely unshared bitmap run rather than failing after a full copy.
    // All spans still go through the mandatory byte-exact staged guard.
    EditProtectedRanges immutable;
    immutable.emplace_back(0,256ull*block);
    immutable.insert(immutable.end(),image.hybrid_protected_regions.begin(),
                     image.hybrid_protected_regions.end());
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
        // ISO9660 multi-extent sections may live in unrelated blocks.
        // Protect the actual physical intervals, not the fictitious
        // contiguous range [first LBA, first LBA + logical file size).
        each_extent(*node,[&](auto start,auto length) {
            if(length)immutable.emplace_back(start,length);
        });
        for(const auto& child:node->children)
            nodes.push_back(child.get());
        need(nodes.size()<=200000,"ISO/UDF immutable namespace exceeds limit");
    }
    return immutable;
}

void execute_bridge_metadata_edit(const std::filesystem::path& path,
        Image& image,const std::vector<ArchiveChange>& changes,
        const EditProgress& progress,BridgeMetadataEdit& edit) {
    const auto immutable=bridge_metadata_protected_ranges(image);
    const CoordinatedIsoUpdate* coordinated=nullptr;
    if(edit.adding_shared || !edit.shared.empty())coordinated=&edit.plan;
    std::uint64_t mbr_alignment=0,gpt_trailer=0;
    if(edit.mbr_append)mbr_alignment=edit.mbr_append->alignment_blocks;
    if(edit.gpt_append)gpt_trailer=edit.gpt_append->trailer_blocks;
    const bool partitioned=image.hybrid_system_area;
    // The callback is invoked synchronously by edit_udf before commit.
    // The owning edit plan outlives this entire call, including rollback.
    const EditStageValidator validate=
        [&path,&image,&edit,&progress](const std::filesystem::path& staged) {
            validate_bridge_metadata_stage(path,staged,image,edit,progress);
        };
    image.file.close();
    edit_udf(path,changes,progress,true,validate,immutable,coordinated,
             partitioned,mbr_alignment,gpt_trailer);
}

// Initial shared-file VAT profile. The established ISO coordinator
// rewrites only explicitly planned ISO record fields and volume sizes.
// The VAT backend additionally requires all these fields to precede
// sector 256, outside the UDF descriptors and backing partition.
void edit_iso_vat_shared_bridge(const std::filesystem::path& path,Image& image,
        const std::vector<ArchiveChange>& changes,const EditProgress& progress) {
    auto edit=prepare_bridge_metadata_edit(path,image,changes);
    need(!edit->adding_shared && !edit->shared.empty() &&
         edit->shared.size()==changes.size() &&
         !edit->mbr_append && !edit->gpt_append,
         "VAT shared bridge supports existing regular-file replacements and removals only");
    // Require the actual extent to be independently visible in both
    // ISO views. A same-name but different-extent entry, a primary alias
    // of another name, or any extra payload alias is not this profile.
    for(bool primary:{true,false}) {
        const auto view=inspect_iso_namespace(path,primary);
        need(view.format=="ISO 9660" || view.format=="Joliet",
             "VAT shared bridge requires ordinary ISO/Joliet namespaces");
        for(const auto& [key,node]:edit->shared) {
            edit_tick(progress);
            const auto wanted=upper(key);
            std::size_t matches=0;
            const auto at=std::uint64_t(node->lba)*block;
            for(const auto& item:view.items) {
                if(upper(item.path)==wanted) {
                    need(!item.directory && item.size==node->size &&
                         item.extents.size()==1 && !item.extents[0].sparse &&
                         item.extents[0].offset==at &&
                         item.extents[0].length==node->size,
                         "VAT shared ISO view disagrees with UDF extent");
                    ++matches;
                } else {
                    for(const auto& e:item.extents)
                        need(e.sparse || !intersect(at,node->size,e.offset,e.length),
                             "VAT shared target aliases another ISO entry");
                }
            }
            need(matches==1,"VAT shared target missing or ambiguous in ISO view");
        }
    }
    auto immutable=bridge_metadata_protected_ranges(image);
    std::optional<GptGrowthPlan> gpt;
    if(image.gpt_hybrid) {
        // The primary and Joliet views may contain different independent
        // ISO-only resources. Include both before retiring the old GPT.
        for(bool primary:{true,false}) {
            const auto view=inspect_iso_namespace(path,primary);
            for(const auto& item:view.items)
                for(const auto& e:item.extents)
                    if(e.length && !e.sparse)immutable.emplace_back(e.offset,e.length);
        }
        gpt=plan_vat_gpt_bridge(image,immutable);
    }
    auto* owner=edit.get();
    const auto* gpt_plan=gpt?&*gpt:nullptr;
    const EditStageValidator validate=
        [&path,&image,&progress,owner,gpt_plan](const std::filesystem::path& staged) {
            coordinate_shared_bridge_stage(path,staged,image,owner->shared,
                owner->plan,owner->deletion_sectors,progress,nullptr,gpt_plan,true);
        };
    image.file.close();
    edit_udf(path,changes,progress,true,validate,immutable,&edit->plan,
             gpt_plan!=nullptr,0,gpt_plan?gpt_plan->trailer_blocks:0);
}

// Fully mirrored VAT trees use a fresh ISO directory/path-table
// reservation. This supports repeat edits even after the previous
// directory tree was appended inside the recorded partition.
// The first protection span covers the whole system area. Exclude
// only this synthetic blanket range from the geometry preflight;
// retain every real ISO/UDF/EFI/boot extent, even in the system area.
std::uint64_t vat_mbr_alignment(Image& image,const EditProtectedRanges& immutable) {
    if(!image.hybrid_system_area)return 0;
    need(!image.gpt_hybrid && !immutable.empty() &&
         immutable.front().first==0 && immutable.front().second==256ull*block,
         "missing VAT MBR system-area protection");
    HybridTailPlan tail;
    tail.original_extents.assign(std::next(immutable.begin()),immutable.end());
    return plan_mbr_growth(image,tail,1,true).alignment_blocks;
}
bool try_edit_iso_vat_mirrored_tree(const std::filesystem::path& path,Image& image,
        const std::vector<ArchiveChange>& changes,const EditProgress& progress) {
    auto edit=prepare_bridge_metadata_edit(path,image,changes,true);
    if(!edit)return false;
    const auto immutable=bridge_metadata_protected_ranges(image);
    std::optional<GptGrowthPlan> gpt;
    if(image.gpt_hybrid)gpt=plan_vat_gpt_bridge(image,immutable);
    const auto alignment=gpt?0u:vat_mbr_alignment(image,immutable);
    auto* owner=edit.get();
    const auto* gpt_plan=gpt?&*gpt:nullptr;
    const EditStageValidator validate=
        [&path,&image,&progress,owner,gpt_plan](const std::filesystem::path& staged) {
            coordinate_shared_add_stage(path,staged,image,owner->iso_files,
                owner->plan,owner->add_tree,progress,nullptr,gpt_plan,true);
        };
    image.file.close();
    edit_udf(path,changes,progress,true,validate,immutable,&edit->plan,
             alignment!=0 || gpt_plan!=nullptr,alignment,
             gpt_plan?gpt_plan->trailer_blocks:0);
    return true;
}

// Sequential VAT media may grow only by appending a new transaction.
// This initial bridge profile intentionally supports disjoint top-level
// trees, not a mirrored namespace. All original ISO/El Torito bytes stay
// unchanged, including the original ISO VolumeSpaceSize. The UDF anchor
// at sector 256 discovers the VAT ICB at actual EOF.
GptGrowthPlan plan_vat_gpt_bridge(Image& image,const EditProtectedRanges& immutable) {
    need(image.gpt_hybrid && image.hybrid_system_area &&
         immutable.size()>=1+image.hybrid_protected_regions.size() &&
         image.hybrid_protected_regions.size()>=5,
         "missing VAT GPT protected geometry");
    const auto header=image.read(512,512);
    need(le64(header,72)==2 && le(header,80)==128 && le(header,84)==128,
         "VAT GPT requires a standard 128-entry partition array");
    // The ISO parser lists GPT metadata before partition data. Exclude
    // only its structural metadata entries from the relocation check.
    // Independent ISO/UDF/boot extents at those addresses remain in
    // the snapshot and therefore cannot be mistaken for GPT metadata.
    const auto& regions=image.hybrid_protected_regions;
    need(regions[0]==std::pair<std::uint64_t,std::uint64_t>{512,512} &&
         regions[1]==std::pair<std::uint64_t,std::uint64_t>{image.size-512,512} &&
         regions[2]==std::pair<std::uint64_t,std::uint64_t>{1024,16384} &&
         regions[3]==std::pair<std::uint64_t,std::uint64_t>{image.size-33*512,16384},
         "VAT GPT requires protective-only standard GPT geometry");
    HybridTailPlan tail;
    tail.original_extents.assign(immutable.begin()+1+regions.size(),immutable.end());
    tail.original_extents.insert(tail.original_extents.end(),regions.begin()+4,regions.end());
    auto plan=plan_gpt_growth(image,tail,1,true);
    need(plan.trailer_blocks==9 &&
         std::uint64_t(image.declared_volume_blocks)<=image.size/block-9,
         "VAT GPT backup must follow the recorded optical volume");
    return plan;
}
void validate_vat_gpt_bridge(const std::filesystem::path& staged,
                             const Image& before,GptGrowthPlan plan,bool remastered) {
    const auto size=std::filesystem::file_size(staged);
    need(size>before.size && size%block==0 && size/block>plan.trailer_blocks,
         "invalid staged VAT GPT size");
    set_gpt_growth_size(plan,size/block-plan.trailer_blocks);
    Image after(staged,true);
    auto expected=before.hybrid_protected_regions;
    expected[1].first=plan.new_header;
    expected[3].first=plan.new_table;
    need(after.udf_bridge && after.gpt_hybrid && after.hybrid_system_area &&
         after.hybrid_protected_regions==expected &&
         after.gpt_first_usable==before.gpt_first_usable &&
         after.gpt_last_usable==plan.new_table/512-1 &&
         after.boot_regions_bounded &&
         (remastered?after.declared_volume_blocks==plan.logical_blocks:
                     after.descriptors==before.descriptors) &&
         after.boot_regions==before.boot_regions &&
         after.boot_entries==before.boot_entries &&
         after.boot_lbas==before.boot_lbas &&
         after.boot_descriptor_lbas==before.boot_descriptor_lbas &&
         after.read(0,512)==plan.mbr && after.read(512,512)==plan.primary &&
         after.read(plan.new_header,512)==plan.backup &&
         after.read(plan.new_table,plan.entries.size())==plan.entries,
         "staged VAT GPT geometry differs from independent growth plan");
}

void edit_iso_vat_bridge(const std::filesystem::path& path,Image& image,
        const Disc& udf,const std::vector<ArchiveChange>& changes,
        const EditProgress& progress) {
    need(!image.iso_system_use &&
         image.boot_regions_bounded && udf.udf_has_virtual_partition(),
         "VAT bridge requires bounded plain ISO/Joliet");
    if(try_edit_iso_vat_mirrored_tree(path,image,changes,progress))return;
    {
        const auto index=index_iso_tree(*image.root);
        const bool shared=std::any_of(changes.begin(),changes.end(),
            [&](const ArchiveChange& c){return index.count(upper(edit_path(c.path)))!=0;});
        if(shared) {
            need(!image.hybrid_system_area || image.gpt_hybrid,
                 "MBR VAT shared edits require a supported mirrored namespace");
            edit_iso_vat_shared_bridge(path,image,changes,progress);
            return;
        }
    }
    std::set<std::string> iso_roots;
    auto top=[](const std::string& key) {
        return upper(key.substr(0,key.find('/')));
    };
    // Inspect both views separately, including aliases that the merged
    // Image tree may otherwise present under only one visible name.
    auto immutable=bridge_metadata_protected_ranges(image);
    for(bool primary:{true,false}) {
        const auto view=inspect_iso_namespace(path,primary);
        need(view.format=="ISO 9660" || view.format=="Joliet",
             "VAT bridge requires ordinary ISO/Joliet namespaces");
        for(const auto& item:view.items) {
            iso_roots.insert(top(item.path));
            for(const auto& e:item.extents)
                if(e.length && !e.sparse)immutable.emplace_back(e.offset,e.length);
        }
    }
    std::optional<GptGrowthPlan> gpt;
    if(image.gpt_hybrid)gpt=plan_vat_gpt_bridge(image,immutable);
    const auto mbr_alignment=gpt?0u:vat_mbr_alignment(image,immutable);
    // Coalesce immutable ranges for efficient overlap checks. Even an
    // ISO alias with a different name must not become a silently split
    // shared payload. Historical source bytes are always protected.
    std::sort(immutable.begin(),immutable.end());
    EditProtectedRanges merged;
    for(const auto& [at,length]:immutable) {
        need(at<=image.size && length<=image.size-at,
             "VAT bridge protected extent outside recorded image");
        if(!length)continue;
        if(!merged.empty() && at<=merged.back().first+merged.back().second)
            merged.back().second=std::max(merged.back().first+merged.back().second,
                                         at+length)-merged.back().first;
        else merged.emplace_back(at,length);
    }
    for(const auto& item:udf.items()) {
        edit_tick(progress);
        need(!iso_roots.count(top(item.path)),
             "VAT bridge shared ISO/UDF namespace requires coordinated mastering");
        for(const auto& e:item.extents) {
            if(e.sparse || !e.length)continue;
            need(e.offset<=image.size && e.length<=image.size-e.offset,
                 "VAT bridge UDF payload outside original image");
            const auto at=std::lower_bound(merged.begin(),merged.end(),e.offset,
                [](const auto& range,std::uint64_t offset) {
                    return range.first+range.second<=offset;
                });
            need(at==merged.end() || at->first>=e.offset+e.length,
                 "VAT bridge UDF payload overlaps protected ISO/boot data");
        }
    }
    for(const auto& change:changes)
        need(!iso_roots.count(top(edit_path(change.path))),
             "VAT bridge edit would change a protected ISO namespace");
    // Prefix verification in the VAT backend covers every original byte,
    // not just these ranges. Reparse the staged ISO/boot structures as an
    // additional independent contract; do NOT require ROM end anchors,
    // since a sequential VAT ICB, rather than AVDP, occupies the last LBA.
    const bool mbr=mbr_alignment!=0;
    const auto* gpt_plan=gpt?&*gpt:nullptr;
    const EditStageValidator validate=[&image,mbr,gpt_plan](const std::filesystem::path& staged) {
        if(gpt_plan)validate_vat_gpt_bridge(staged,image,*gpt_plan);
        else validate_vat_bridge_envelope(staged,image,mbr);
    };
    image.file.close();
    edit_udf(path,changes,progress,true,validate,merged,nullptr,
             mbr || gpt_plan!=nullptr,mbr_alignment,gpt_plan?gpt_plan->trailer_blocks:0);
}

void edit_iso_udf_bridge_metadata(const std::filesystem::path& path,
                                  Image& image,
                                  const std::vector<ArchiveChange>& changes,
                                  const EditProgress& progress) {
    auto edit=prepare_bridge_metadata_edit(path,image,changes);
    execute_bridge_metadata_edit(path,image,changes,progress,*edit);
}
}
// Keep the write and independent readback phases in separate stack frames.
// In particular, do not carry a capturing capacity closure through the
// full ISO authoring function. MSVC x64 ASan reported use-after-poison
// at its invocation in ordinary multi-file F5 batches. No sanitizer
// instrumentation or validation is disabled.
static constexpr std::uint64_t iso_max_section=0xfffff800ull;
static void check_iso_capacity(std::uint64_t next,std::uint64_t blocks,
                               const std::optional<HybridTailPlan>& hybrid_tail) {
    need(next<0xffffffffu && blocks<0xffffffffull-next,"ISO size limit");
    if(hybrid_tail)
        need(next<=hybrid_tail->limit && blocks<=hybrid_tail->limit-next,
             "hybrid ISO has insufficient verified padding; partition resize required");
}
static std::uint64_t write_iso_rebuild(
        Image& image,std::fstream& out,const std::vector<Node*>& all,
        const std::vector<Node*>& dirs,
        const std::optional<HybridTailPlan>& hybrid_tail,
        const std::optional<GptGrowthPlan>& gpt_growth,
        const std::optional<MbrGrowthPlan>& mbr_growth,
        const EditProgress& progress) {
    std::uint64_t next=hybrid_tail?hybrid_tail->first:image.size/block;
    if(hybrid_tail) {
        out.seekp(static_cast<std::streamoff>(next*block));
        need(bool(out),"cannot seek verified hybrid ISO tail");
    }
    for(auto* n:all)if(!n->source.empty()) {
        check_iso_capacity(next,(n->size+block-1)/block,hybrid_tail);
        n->lba=static_cast<std::uint32_t>(next);
        std::ifstream source(n->source,std::ios::binary);
        need(bool(source),"cannot open ISO replacement source");
        // ECMA-119 Level 3 permits multiple records per file. Every
        // nonfinal section is aligned and fits the 32-bit data length.
        n->sections.clear(); // discard preflight-only section addresses
        auto left=n->size;
        do {
            const auto bytes=static_cast<std::uint32_t>(
                std::min(left,iso_max_section));
            n->sections.push_back({static_cast<std::uint32_t>(next),bytes});
            copy_bytes(source,out,bytes,progress);
            write(out,Bytes((block-bytes%block)%block));
            next+=(std::uint64_t(bytes)+block-1)/block;
            left-=bytes;
        } while(left);
        need(source.peek()==std::char_traits<char>::eof(),
             "ISO replacement source grew during write");
    }
    if(hybrid_tail && image.joliet)
        for(auto* d:dirs) {
            const auto bytes=std::max(directory_data(*d,0).size(),
                                      directory_data(*d,1).size());
            need(bytes<=0xffffffffu,"hybrid ISO directory too large");
            d->new_size[0]=d->new_size[1]=static_cast<std::uint32_t>(bytes);
        }
    for(unsigned view=0;view<(image.joliet?2u:1u);++view){
        for(auto* d:dirs){
            if(!hybrid_tail || !image.joliet)
                d->new_size[view]=static_cast<std::uint32_t>(directory_data(*d,view).size());
            check_iso_capacity(next,d->new_size[view]/block,hybrid_tail);
            d->new_lba[view]=static_cast<std::uint32_t>(next);
            next+=d->new_size[view]/block;
        }
        for(auto* d:dirs){
            edit_tick(progress);
            auto bytes=directory_data(*d,view);
            need(bytes.size()<=d->new_size[view],"ISO directory allocation changed");
            bytes.resize(d->new_size[view],0);
            write(out,bytes);
        }
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
            table_size=static_cast<std::uint32_t>(table.size());table_lba[big]=static_cast<std::uint32_t>(next);table.resize((table.size()+block-1)/block*block);check_iso_capacity(next,table.size()/block,hybrid_tail);next+=table.size()/block;write(out,table);}
        for(auto& [lba,vd]:image.descriptors)if((vd[0]==2)==(view==1)){both(vd,132,table_size);put(vd,140,table_lba[0]);put(vd,144,0);put(vd,148,table_lba[1],4,true);put(vd,152,0);auto r=record(*image.root,Bytes{0},view);std::copy(r.begin(),r.end(),vd.begin()+156);(void)lba;}
    }
    for(auto& [lba,vd]:image.descriptors){both(vd,80,static_cast<std::uint32_t>(next));out.seekp(std::uint64_t(lba)*block);write(out,vd);}
    if(gpt_growth)write_gpt_growth(out,*gpt_growth,image.size,next,progress);
    if(mbr_growth)write_mbr_growth(out,*mbr_growth,next,progress);
    return next;
}
static void verify_iso_rebuild(
        const std::filesystem::path& staged,const Image& image,
        const std::vector<ArchiveChange>& changes,std::uint64_t next,
        const std::optional<HybridTailPlan>& hybrid_tail,
        const std::optional<GptGrowthPlan>& gpt_growth,
        const std::optional<MbrGrowthPlan>& mbr_growth,
        const EditProgress& progress) {
    // Reparse the staged namespaces and reject changed El Torito references
    // even if the ISO directory metadata itself remains parseable.
    {
        Image verified(staged,false,image.hybrid_rr_strict);
        need(verified.size==(gpt_growth?gpt_growth->physical_size:
                                 mbr_growth?mbr_growth->physical_size:
                                 hybrid_tail?image.size:next*block) &&
             verified.declared_volume_blocks==next,
             "staged ISO physical or logical size mismatch");
        if(hybrid_tail) {
            auto expected_regions=image.hybrid_protected_regions;
            if(gpt_growth) {
                const auto& g=*gpt_growth;
                for(auto& [at,length]:expected_regions) {
                    if(at==image.size-512 && length==512)at=g.new_header;
                    else if(at==g.old_backup && length==g.entries.size())at=g.new_table;
                }
                need(verified.gpt_first_usable==image.gpt_first_usable &&
                     verified.gpt_last_usable==g.new_table/512-1 &&
                     verified.read(0,512)==g.mbr &&
                     verified.read(512,512)==g.primary &&
                     verified.read(g.new_table,g.entries.size())==g.entries &&
                     verified.read(g.new_header,512)==g.backup,
                     "relocated GPT metadata differs from growth plan");
                const auto old=verified.read(g.old_backup,
                    static_cast<std::size_t>(image.size-g.old_backup));
                need(std::all_of(old.begin(),old.end(),[](auto c){return c==0;}),
                     "obsolete GPT backup was not cleared");
            }
            if(mbr_growth) {
                need(verified.read(0,512)==mbr_growth->mbr,
                     "grown MBR header differs from plan");
                const auto bytes=verified.read(next*block,
                    static_cast<std::size_t>(verified.size-next*block));
                need(std::all_of(bytes.begin(),bytes.end(),[](auto c){return c==0;}),
                     "grown MBR alignment padding is not zero");
            }
            need(verified.hybrid_system_area && verified.gpt_hybrid==image.gpt_hybrid &&
                 verified.hybrid_protected_regions==expected_regions &&
                 verified.descriptors==image.descriptors,
                 "hybrid partition geometry or ISO descriptor readback changed");
            const auto wanted=index_iso_tree(*image.root);
            const auto actual=index_iso_tree(*verified.root);
            need(wanted.size()==actual.size(),"hybrid ISO namespace entry count differs");
            for(const auto& [key,node]:wanted) {
                const auto found=actual.find(key);
                need(found!=actual.end() && found->second->directory==node->directory &&
                     found->second->identifier==node->identifier &&
                     found->second->joliet_identifier==node->joliet_identifier &&
                     (node->directory?
                        (found->second->lba==node->new_lba[0] &&
                         found->second->size==node->new_size[0]):
                        (found->second->size==node->size &&
                         found->second->sections==node->sections)),
                     "hybrid ISO namespace differs from rebuild plan");
                if(image.hybrid_rr_strict)
                    need(found->second->record==
                         record(*node,node->identifier,0,nullptr,nullptr,false,
                                &node->rr_record),
                         "hybrid Rock Ridge record differs from remaster plan");
            }
            if(image.hybrid_rr_strict) {
                auto verify_dots=[](const Node& want,const Node& got) {
                    if(!want.directory)return;
                    need(got.rr_dot==record(want,Bytes{0},0,&want,nullptr,
                                           false,&want.rr_dot,true) &&
                         got.rr_parent==record(want,Bytes{1},0,
                              want.parent?want.parent:&want,nullptr,false,
                              &want.rr_parent,true),
                         "hybrid Rock Ridge dot metadata differs from remaster plan");
                };
                verify_dots(*image.root,*verified.root);
                for(const auto& [key,node]:wanted)
                    verify_dots(*node,*actual.at(key));
            }
        }
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
            std::ifstream output(staged,std::ios::binary);
            need(bool(input) && bool(output),
                 "cannot open ISO file for independent F5 verification");
            std::vector<char> source_chunk(64u*1024u),stage_chunk(64u*1024u);
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
    std::optional<HybridTailPlan> hybrid_tail;
    if(image.hybrid_system_area && !image.udf_bridge) {
        bool same_size=false;
        if(image.iso_system_use) {
            // The ordinary parsed tree has ISO aliases, not RRIP NM
            // names. Keep the byte-only path for equal-length edits.
            const Disc disc(path);
            same_size=disc.format()=="Rock Ridge" && !changes.empty() &&
                std::all_of(changes.begin(),changes.end(),[&](const ArchiveChange& c) {
                    if(c.remove || c.directory || !std::filesystem::is_regular_file(
                           std::filesystem::symlink_status(c.source)))return false;
                    const auto key=edit_path(c.path);
                    const auto item=std::find_if(disc.items().begin(),disc.items().end(),
                        [&](const Item& entry){return entry.path==key && !entry.directory;});
                    return item!=disc.items().end() &&
                           item->size==std::filesystem::file_size(c.source);
                });
        } else {
            const auto index=index_iso_tree(*image.root);
            same_size=!changes.empty() &&
                std::all_of(changes.begin(),changes.end(),[&](const ArchiveChange& c) {
                    if(c.remove || c.directory || !std::filesystem::is_regular_file(
                           std::filesystem::symlink_status(c.source)))return false;
                    const auto found=index.find(upper(edit_path(c.path)));
                    return found!=index.end() && !found->second->directory &&
                           found->second->sections.size()==1 &&
                           found->second->size==std::filesystem::file_size(c.source);
                });
        }
        if(same_size) {
            edit_iso_hybrid_equal_length(path,image,changes,progress);
            return;
        }
        if(image.iso_system_use) {
            // Unknown SUSP, unmatched views and unsupported geometry
            // fail before staging. Never downgrade to plain ISO rebuild.
            image.file.close();
            image=Image(path,true,true);
        }
        const auto index=index_iso_tree(*image.root);
        hybrid_tail=plan_hybrid_tail(image,progress);
        // Check the original targets before changing the in-memory tree.
        for(const auto& c:changes) {
            const auto found=index.find(upper(edit_path(c.path)));
            if(found!=index.end())protect_hybrid_target(*found->second,image);
        }
    }
    if(image.udf_bridge) {
        // Prefer the non-metadata equal-length replacement for a pre-existing
        // single-extent file (including UDF-only paths).
        const Disc udf(path);
        need(udf.format()=="UDF","ISO/UDF filesystem is unreadable");
        // VAT must never take the legacy in-place equal-length shortcut,
        // even when the current file has one ordinary physical extent.
        // The parsed map family, not a guessed revision string or the
        // Type-1 write preflight, determines this routing.
        if(udf.udf_has_virtual_partition()) {
            edit_iso_vat_bridge(path,image,udf,changes,progress);
            return;
        }
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
        else
            edit_iso_udf_bridge_metadata(path,image,changes,progress);
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
    std::vector<Node*> all{image.root.get()},dirs{image.root.get()};
    for(std::size_t i=0;i<all.size();++i){for(auto& c:all[i]->children){all.push_back(c.get());if(c->directory)dirs.push_back(c.get());}need(all.size()<=200000&&dirs.size()<=65535,"ISO edit tree too large");}
    std::optional<GptGrowthPlan> gpt_growth;
    std::optional<MbrGrowthPlan> mbr_growth;
    if(hybrid_tail) {
        std::uint64_t required=0;
        for(auto* n:all)if(!n->source.empty()) {
            // Directory preflight must serialize exactly as many records
            // as the eventual Level-3 payload. Dummy LBAs are used only
            // for sizing; the original extent snapshot is already sealed.
            auto left=n->size;
            n->sections.clear();
            do {
                const auto bytes=static_cast<std::uint32_t>(
                    std::min(left,iso_max_section));
                n->sections.push_back({0,bytes});
                left-=bytes;
            } while(left);
            required+=(n->size+block-1)/block;
        }
        const unsigned views=image.joliet?2u:1u;
        std::array<std::uint64_t,2> table_bytes{};
        for(auto* d:dirs) {
            std::size_t max_directory=0;
            for(unsigned view=0;view<views;++view) {
                max_directory=std::max(max_directory,directory_data(*d,view).size());
                const auto id=d==image.root.get()?1u:view_id(*d,view).size();
                table_bytes[view]+=8+id+(id%2);
            }
            required+=views*(max_directory/block);
        }
        for(unsigned view=0;view<views;++view)
            required+=2*((table_bytes[view]+block-1)/block);
        if(required>hybrid_tail->limit-hybrid_tail->first) {
            if(image.gpt_hybrid) {
                gpt_growth=plan_gpt_growth(image,*hybrid_tail,required);
                hybrid_tail->limit=gpt_growth->logical_blocks;
            } else {
                mbr_growth=plan_mbr_growth(image,*hybrid_tail,required);
                hybrid_tail->limit=mbr_growth->logical_blocks;
            }
            hybrid_tail->first=image.size/block;
        }
    }
    EditTransaction tx(path);std::fstream out(tx.output,std::ios::in|std::ios::out|std::ios::binary|std::ios::trunc);need(bool(out),"cannot create staged ISO");image.file.clear();image.file.seekg(0);copy_bytes(image.file,out,image.size,progress);
    const auto next=write_iso_rebuild(image,out,all,dirs,hybrid_tail,
                                      gpt_growth,mbr_growth,progress);
    out.flush();need(bool(out),"ISO flush failed");out.close();image.file.close();
    verify_iso_rebuild(tx.output,image,changes,next,hybrid_tail,
                       gpt_growth,mbr_growth,progress);
    // The ordinary ISO writer appends data and regenerates PVD/Joliet trees.
    // It must never alter *any* original system-area, boot descriptor,
    // boot catalog or full boot-image byte, including emulated floppy/HDD
    // payloads. Only protected ranges in the original prefix are compared.
    OpticalEditGuard guard(image.size);
    if(!gpt_growth && !mbr_growth)guard.protect(0,16ull*block);
    for(const auto lba:image.boot_descriptor_lbas)
        guard.protect(std::uint64_t(lba)*block,block);
    for(const auto& [offset,length]:image.boot_regions)
        guard.protect(offset,length);
    for(const auto& [offset,length]:image.iso_auxiliary_regions)
        guard.protect(offset,length);
    if(hybrid_tail) {
        // Byte-exact check of EVERYTHING except the specifically planned
        // PVD/SVD fields and used zero tail. Old metadata, payloads,
        // MBR, GPT (including its backup), ESP and unused pad are immutable.
        auto writes=hybrid_tail->descriptor_fields;
        if(gpt_growth) {
            writes.emplace_back(gpt_growth->mbr_length_at,4);
            writes.emplace_back(512,512);
            writes.emplace_back(gpt_growth->old_backup,image.size-gpt_growth->old_backup);
        } else if(mbr_growth) {
            writes.emplace_back(mbr_growth->end_chs_at,3);
            writes.emplace_back(mbr_growth->length_at,4);
        } else writes.emplace_back(hybrid_tail->first*block,
                                   (next-hybrid_tail->first)*block);
        std::sort(writes.begin(),writes.end());
        std::uint64_t cursor=0;
        for(const auto& [at,bytes]:writes) {
            need(at>=cursor && at<=image.size && bytes<=image.size-at,
                 "overlapping hybrid write ranges");
            guard.protect(cursor,at-cursor);
            cursor=at+bytes;
        }
        guard.protect(cursor,image.size-cursor);
        if(gpt_growth || mbr_growth) {
            // All original bytes outside the explicitly authorized
            // metadata fields are protected, including every partition.
            guard.verify_appended(path,tx.output,progress);
        } else {
            for(const auto& [at,bytes]:image.hybrid_protected_regions)
                guard.protect(at,bytes);
            guard.verify(path,tx.output,progress);
        }
    } else guard.verify_appended(path,tx.output,progress);
    tx.commit(progress);
}
}
