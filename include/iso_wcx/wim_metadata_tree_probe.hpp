#pragma once
// Bounded, READ-ONLY WIM metadata directory decoder. Never enables writes.
// Stage 2 after wim_fast_edit_probe.hpp; fail closed on unknown layouts.
// On-disk structures follow Microsoft's WIM specification and go-winio's
// independent direntry/security-table reader. No code is copied from wimlib.
#include "iso_wcx/wim_fast_edit_probe.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace iso_wcx::wim_detail {

struct MetadataTreeProbe {
    bool valid = false;
    std::string reason;
    std::uint32_t images = 0;
    std::uint64_t entries = 0;
    std::uint64_t directories = 0;
    std::uint64_t streams = 0;
    std::uint64_t security_descriptors = 0;
    std::uint64_t uncompressed_bytes = 0;
};

namespace metadata_tree_detail {
using Bytes = std::span<const std::uint8_t>;
inline void ensure(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}
inline bool within(std::uint64_t off, std::uint64_t size, std::uint64_t end) {
    return off <= end && size <= end-off;
}
inline std::uint16_t u16(Bytes b, std::uint64_t off) {
    ensure(within(off,2,b.size()),"truncated metadata uint16");
    return fast_probe_detail::le16(b.data()+static_cast<std::size_t>(off));
}
inline std::uint32_t u32(Bytes b, std::uint64_t off) {
    ensure(within(off,4,b.size()),"truncated metadata uint32");
    return fast_probe_detail::le32(b.data()+static_cast<std::size_t>(off));
}
inline std::uint64_t u64(Bytes b, std::uint64_t off) {
    ensure(within(off,8,b.size()),"truncated metadata uint64");
    return fast_probe_detail::le64(b.data()+static_cast<std::size_t>(off));
}
inline void utf16(Bytes b, std::uint64_t pos, std::uint16_t bytes) {
    ensure(bytes % 2 == 0 && within(pos,bytes,b.size()),
           "invalid UTF-16 name length");
    bool pending_low=false;
    for(std::uint64_t i=0;i<bytes;i+=2) {
        const auto ch=u16(b,pos+i);
        ensure(ch!=0 && ch>=0x20 && ch!=L'/' && ch!=L'\\',
               "unsafe WIM metadata filename");
        if(pending_low) {
            ensure(ch>=0xdc00 && ch<=0xdfff,"unpaired UTF-16 high surrogate");
            pending_low=false;
        }else if(ch>=0xd800 && ch<=0xdbff) {
            pending_low=true;
        }else {
            ensure(ch<0xdc00 || ch>0xdfff,"unpaired UTF-16 low surrogate");
        }
    }
    ensure(!pending_low,"truncated UTF-16 surrogate pair");
}
class Decoder {
public:
    explicit Decoder(Bytes b) : data_(b) {}
    MetadataTreeProbe parse() {
        ensure(data_.size()>=16,"truncated WIM security table");
        const auto table_length=static_cast<std::uint64_t>(u32(data_,0));
        const auto count=static_cast<std::uint64_t>(u32(data_,4));
        ensure(table_length>=8 && count<=65536 &&
               count<=(table_length-8)/8 && table_length<=data_.size(),
               "invalid WIM security table");
        std::uint64_t offset=8+count*8;
        for(std::uint64_t i=0;i<count;++i) {
            const auto size=u64(data_,8+i*8);
            ensure(size <= table_length-offset,"invalid security descriptor size");
            if(size!=0) {
                ensure(size>=20 && data_[static_cast<std::size_t>(offset)]==1 &&
                       (u16(data_,offset+2)&0x8000u)!=0,
                       "invalid self-relative security descriptor");
            }
            offset+=size;
        }
        const auto root_offset=(table_length+7u)&~std::uint64_t{7};
        ensure(root_offset<data_.size(),"missing WIM root directory entry");
        occupy(0,root_offset);
        stats_.security_descriptors=count;
        struct Pending {std::uint64_t offset; unsigned depth; bool root;};
        std::vector<Pending> pending{{root_offset,0,true}};
        for(std::size_t pi=0;pi<pending.size();++pi) {
            const auto node=pending[pi];
            ensure(node.depth<=128 && node.offset>=root_offset &&
                   node.offset%8==0 && node.offset<data_.size(),
                   "invalid WIM directory-list pointer or depth");
            std::uint64_t cursor=node.offset;
            unsigned in_list=0;
            for(;;) {
                ensure(within(cursor,8,data_.size()),"unterminated WIM directory list");
                const auto encoded_length=u64(data_,cursor);
                // WIM writers may store an unaligned logical record length,
                // while the following record starts after 8-byte padding.
                // A zero or length-eight slot terminates a sibling list.
                if(encoded_length==0 || encoded_length==8) {
                    occupy(cursor,cursor+8);
                    if(node.root) ensure(in_list==1,"WIM root list is not singular");
                    break;
                }
                ensure(encoded_length>=102 &&
                       encoded_length<=data_.size(),
                       "malformed WIM directory entry length");
                const auto length=(encoded_length+7u)&~std::uint64_t{7};
                ensure(within(cursor,length,data_.size()),
                       "WIM directory record exceeds metadata");
                ++in_list;
                ensure(in_list<=1000000 && stats_.entries<1000000,
                       "too many WIM directory entries");
                const auto attr=u32(data_,cursor+8);
                const auto sid=u32(data_,cursor+12);
                const auto child=u64(data_,cursor+16);
                const auto stream_count=u16(data_,cursor+96);
                const auto short_length=u16(data_,cursor+98);
                const auto name_length=u16(data_,cursor+100);
                ensure(sid==0xffffffffu || sid<count,
                       "WIM security descriptor reference is outside table");
                ensure(stream_count<=64 && name_length<=510 &&
                       short_length<=24 &&
                       102u+name_length+2u+short_length<=encoded_length,
                       "unsupported WIM name or stream record length");
                ensure(u16(data_,cursor+102u+name_length)==0,
                       "WIM filename is missing NUL terminator");
                utf16(data_,cursor+102,name_length);
                utf16(data_,cursor+104u+name_length,short_length);
                if(node.root) {
                    ensure(in_list==1 && name_length==0 && (attr&0x10u)!=0,
                           "invalid WIM root directory");
                }else {
                    ensure(name_length!=0,"unnamed non-root WIM entry");
                }
                const bool directory=(attr&0x10u)!=0;
                const bool reparse=(attr&0x400u)!=0;
                ensure((directory && !reparse) ? child!=0 : child==0,
                       "inconsistent WIM directory child offset");
                if(directory) ++stats_.directories;
                ++stats_.entries;
                occupy(cursor,cursor+length);
                std::uint64_t next=cursor+length;
                for(unsigned j=0;j<stream_count;++j) {
                    ensure(within(next,38,data_.size()),
                           "truncated WIM alternate-stream entry");
                    const auto encoded_stream_length=u64(data_,next);
                    const auto stream_name_length=u16(data_,next+36);
                    ensure(encoded_stream_length>=38 &&
                           encoded_stream_length<=data_.size() &&
                           38u+stream_name_length<=encoded_stream_length &&
                           stream_name_length<=510,
                           "invalid WIM alternate-stream entry");
                    const auto stream_length=
                        (encoded_stream_length+7u)&~std::uint64_t{7};
                    ensure(within(next,stream_length,data_.size()),
                           "truncated padded WIM stream record");
                    utf16(data_,next+38,stream_name_length);
                    occupy(next,next+stream_length);
                    next+=stream_length;
                    ++stats_.streams;
                }
                if(directory && !reparse && child!=0) {
                    ensure(child>=root_offset && child<data_.size() &&
                           child%8==0,"invalid WIM subdirectory offset");
                    pending.push_back({child,node.depth+1,false});
                }
                cursor=next;
            }
        }
        stats_.valid=true;
        stats_.reason="WIM metadata directory tree decoded read-only; writer is disabled";
        return stats_;
    }
private:
    // Reject cycles, aliased directory lists, and overlapping serialized nodes.
    void occupy(std::uint64_t begin, std::uint64_t end) {
        ensure(begin<end && end<=data_.size(),"invalid metadata span");
        auto next=occupied_.lower_bound(begin);
        ensure(next==occupied_.end() || next->first>=end,
               "overlapping WIM metadata records");
        if(next!=occupied_.begin()) {
            const auto previous=std::prev(next);
            ensure(previous->second<=begin,"overlapping WIM metadata records");
        }
        ensure(occupied_.emplace(begin,end).second,
               "reused WIM directory-list offset");
    }
    Bytes data_;
    std::map<std::uint64_t,std::uint64_t> occupied_;
    MetadataTreeProbe stats_;
};
} // namespace metadata_tree_detail

// Direct memory-only entry point for fuzzing and isolated regression fixtures.
inline MetadataTreeProbe scan_uncompressed_metadata(
        std::span<const std::uint8_t> metadata) {
    try {
        return metadata_tree_detail::Decoder(metadata).parse();
    }catch(const std::exception& e) {
        MetadataTreeProbe failed;
        failed.reason=e.what();
        return failed;
    }
}

// Validate all uncompressed metadata resources in a WIM without invoking
// WIMGAPI, unpacking file payloads, writing, or retaining a mutable pointer.
// Compressed/ESD/SWM/delta WIMs remain unsupported by this scanner.
inline MetadataTreeProbe scan_wim_metadata_tree(
        const std::filesystem::path& path) {
    MetadataTreeProbe result;
    try {
        const auto base=probe_fast_edit_metadata(path);
        result.images=base.images;
        if(!base.metadata_layout_candidate) {
            result.reason=base.reason;
            return result;
        }
        std::ifstream in(path,std::ios::binary);
        if(!in) throw std::runtime_error("cannot reopen WIM read-only");
        std::array<std::uint8_t,208> hdr{};
        if(!fast_probe_detail::read_at(in,0,hdr.data(),hdr.size()))
            throw std::runtime_error("cannot reread WIM header");
        const auto lookup=hdr.data()+48;
        const auto lookup_offset=fast_probe_detail::le64(lookup+8);
        const auto lookup_size=fast_probe_detail::resource_size(lookup);
        std::array<std::uint8_t,50> entry{};
        unsigned count=0;
        for(std::uint64_t i=0;i<lookup_size/entry.size();++i) {
            if(!fast_probe_detail::read_at(in,lookup_offset+i*entry.size(),
                                            entry.data(),entry.size()))
                throw std::runtime_error("cannot reread WIM resource table");
            if((entry[7]&2u)==0) continue;
            if((entry[7]&4u)!=0)
                throw std::runtime_error("compressed WIM metadata not decoded yet");
            const auto size=fast_probe_detail::resource_size(entry.data());
            const auto start=fast_probe_detail::le64(entry.data()+8);
            if(size>128ull*1024ull*1024ull ||
               result.uncompressed_bytes>256ull*1024ull*1024ull-size)
                throw std::runtime_error("WIM metadata exceeds bounded decoder budget");
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
            if(!fast_probe_detail::read_at(in,start,bytes.data(),bytes.size()))
                throw std::runtime_error("cannot read WIM metadata resource");
            const auto tree=scan_uncompressed_metadata(bytes);
            if(!tree.valid)
                throw std::runtime_error("WIM image "+std::to_string(count+1)+": "+tree.reason);
            result.entries+=tree.entries;
            result.directories+=tree.directories;
            result.streams+=tree.streams;
            result.security_descriptors+=tree.security_descriptors;
            result.uncompressed_bytes+=size;
            ++count;
        }
        if(count!=base.images)
            throw std::runtime_error("WIM image metadata count mismatch");
        result.valid=true;
        result.reason="metadata trees decoded read-only; no fast WIM writer implemented";
    }catch(const std::exception& ex) {
        result.valid=false;
        result.reason=ex.what();
    }
    return result;
}
} // namespace iso_wcx::wim_detail
