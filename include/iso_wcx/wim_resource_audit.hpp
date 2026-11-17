#pragma once
// WIM resource SHA-1 and metadata reference audit: READ ONLY, FAIL CLOSED.
// It verifies bounded uncompressed resources and metadata -> lookup links.
// It DOES NOT validate compressed payload bytes, WIM XML, integrity tables,
// security descriptor semantics or license fast metadata writes.
#include "iso_wcx/wim_metadata_tree_probe.hpp"
#include "iso_wcx/wim_sha1.hpp"
#include "iso_wcx/wim_chunked_resource.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace iso_wcx::wim_detail {
struct WimResourceAudit {
    bool metadata_verified=false;
    bool references_resolved=false;
    bool uncompressed_resources_verified=false;
    bool limited_audit_complete=false;
    bool requires_decompression=false;
    std::uint64_t images=0;
    std::uint64_t metadata_resources_verified=0;
    std::uint64_t referenced_hashes=0;
    std::uint64_t uncompressed_payloads_verified=0;
    std::uint64_t compressed_metadata_verified=0;
    std::uint64_t compressed_payloads_verified=0;
    std::uint64_t chunk_tables_verified=0;
    std::uint64_t unverified_payloads=0;
    std::string reason;
};

namespace resource_audit_detail {
struct Descriptor {
    std::uint64_t stored=0;
    std::uint64_t original=0;
    std::uint64_t offset=0;
    std::uint8_t flags=0;
    WimSha1Digest digest{};
};

inline Descriptor decode(std::span<const std::uint8_t,50> b) {
    Descriptor d;
    d.stored=fast_probe_detail::resource_size(b.data());
    d.offset=fast_probe_detail::le64(b.data()+8);
    d.original=fast_probe_detail::le64(b.data()+16);
    d.flags=b[7];
    for(unsigned j=0;j<d.digest.size();++j)
        d.digest[j]=b[30+j];
    return d;
}

inline std::vector<std::uint8_t> load(
        std::ifstream& in,std::uint64_t offset,std::uint64_t size) {
    if(size>128ull*1024ull*1024ull)
        throw std::runtime_error("WIM resource exceeds bounded reader budget");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if(!fast_probe_detail::read_at(in,offset,bytes.data(),bytes.size()))
        throw std::runtime_error("truncated WIM resource");
    return bytes;
}
} // namespace resource_audit_detail

// All bytes are read through std::ifstream. No WIMGAPI Apply, writes or
// temporary mounts. Even a successful result is NOT fast-edit permission.
[[nodiscard]] inline WimResourceAudit audit_wim_resource_links(
        const std::filesystem::path& image) {
    WimResourceAudit result;
    try {
        const auto probe=probe_fast_edit_metadata(image);
        result.images=probe.images;
        if(!probe.header_and_lookup_valid)
            throw std::runtime_error(probe.reason);
        // The narrow legacy fast-write preflight intentionally rejects
        // compressed metadata. Read-only audit is allowed to examine it,
        // but this NEVER changes that preflight's write-disabled verdict.
        const bool compressed_metadata_candidate=
            !probe.metadata_layout_candidate &&
            probe.reason=="WIM image metadata must be decompressed before fast editing";
        if(!probe.metadata_layout_candidate &&
           !compressed_metadata_candidate) {
            result.requires_decompression=probe.compressed_metadata_resources>0;
            result.reason="incomplete: "+probe.reason;
            return result;
        }
        std::ifstream in(image,std::ios::binary);
        if(!in)throw std::runtime_error("cannot open WIM for resource audit");
        std::array<std::uint8_t,208> hdr{};
        if(!fast_probe_detail::read_at(in,0,hdr.data(),hdr.size()))
            throw std::runtime_error("cannot read WIM header for resource audit");
        const auto* lookup=hdr.data()+48;
        const auto lookup_at=fast_probe_detail::le64(lookup+8);
        const auto lookup_bytes=fast_probe_detail::resource_size(lookup);
        const auto header_flags=fast_probe_detail::le32(hdr.data()+16);
        const auto chunk_size=fast_probe_detail::le32(hdr.data()+20);
        // Explicit, exclusive algorithm selection. Mislabelled formats
        // cannot be sent to a Windows decoder of the wrong codec.
        const auto compression_bits=header_flags &
            (0x00020000u|0x00040000u|0x00080000u|0x00100000u|0x00200000u);
        const bool xpress=compression_bits==0x00020000u &&
                          (header_flags&0x00000002u)!=0;
        const auto algorithm=xpress ? WimChunkAlgorithm::xpress_huff :
                             compression_bits==0x00040000u ?
                             WimChunkAlgorithm::lzx :
                             WimChunkAlgorithm::unsupported;
        std::map<WimSha1Digest,resource_audit_detail::Descriptor> resources;
        std::vector<resource_audit_detail::Descriptor> metadata_resources;
        std::array<std::uint8_t,50> raw{};
        if(probe.lookup_entries>1000000u)
            throw std::runtime_error("WIM resource table exceeds audit count limit");
        for(std::uint64_t i=0;i<probe.lookup_entries;++i) {
            if(!fast_probe_detail::read_at(in,lookup_at+i*raw.size(),
                                          raw.data(),raw.size()))
                throw std::runtime_error("truncated WIM resource table");
            const auto d=resource_audit_detail::decode(raw);
            if(d.flags & 0x01u) continue; // free resource
            if(d.flags & ~0x06u)
                throw std::runtime_error("unsupported WIM lookup resource flags");
            if(wim_sha1_is_zero(d.digest)) {
                // A data resource without a content hash cannot be linked.
                if((d.flags&2u)!=0)
                    throw std::runtime_error("WIM metadata resource lacks SHA-1");
                continue;
            }
            if(!d.stored || !d.original ||
               !fast_probe_detail::inside(d.offset,d.stored,
                                         probe.archive_size) ||
               d.offset<208 ||
               fast_probe_detail::intersects(
                   d.offset,d.stored,lookup_at,lookup_bytes))
                throw std::runtime_error("WIM resource descriptor outside image");
            if(!(d.flags&4u) && d.stored!=d.original)
                throw std::runtime_error("uncompressed WIM resource size mismatch");
            if(!resources.emplace(d.digest,d).second)
                throw std::runtime_error("duplicate WIM content hash descriptor");
            if(d.flags&2u)metadata_resources.push_back(d);
        }
        if(metadata_resources.size()!=probe.images)
            throw std::runtime_error("WIM metadata resource count mismatch");

        std::vector<WimSha1Digest> required;
        std::uint64_t metadata_budget=0;
        for(const auto& m:metadata_resources) {
            if(m.original>128ull*1024ull*1024ull ||
               metadata_budget>256ull*1024ull*1024ull-m.original)
                throw std::runtime_error("WIM metadata audit memory budget exceeded");
            metadata_budget+=m.original;
            auto bytes=resource_audit_detail::load(in,m.offset,m.stored);
            if(m.flags&4u) {
                const auto chunks=inspect_wim_chunks(
                    bytes,m.original,chunk_size);
                if(!chunks.valid)
                    throw std::runtime_error(
                        "invalid WIM metadata chunk table: "+chunks.reason);
                ++result.chunk_tables_verified;
                if(algorithm!=WimChunkAlgorithm::xpress_huff) {
                    result.requires_decompression=true;
                    result.reason="incomplete: LZX/LZMS/unknown WIM metadata codec";
                    return result;
                }
#ifndef _WIN32
                if(chunks.compressed_chunks!=0) {
                    result.requires_decompression=true;
                    result.reason="incomplete: XPRESS metadata decoder needs Windows";
                    return result;
                }
#endif
                bytes=read_wim_chunked_metadata(
                    bytes,m.original,chunk_size,algorithm);
                ++result.compressed_metadata_verified;
            }
            if(wim_sha1(bytes)!=m.digest)
                throw std::runtime_error("WIM metadata resource SHA-1 mismatch");
            auto tree=scan_uncompressed_metadata(bytes,true);
            if(!tree.valid)
                throw std::runtime_error(
                    "WIM metadata directory tree invalid: "+tree.reason);
            ++result.metadata_resources_verified;
            for(const auto& hash:tree.referenced_resource_hashes) {
                if(required.size()>=1000000u)
                    throw std::runtime_error(
                        "WIM stream reference audit count exceeded");
                required.push_back(hash);
            }
        }
        result.metadata_verified=true;
        result.referenced_hashes=required.size();
        std::map<WimSha1Digest,bool> unique_references;
        for(const auto& hash:required) {
            const auto it=resources.find(hash);
            if(it==resources.end() || (it->second.flags&2u)!=0)
                throw std::runtime_error(
                    "WIM file/ADS SHA-1 reference missing from resource table");
            unique_references.emplace(hash,false);
        }
        result.references_resolved=true;

        // Avoid reading arbitrary multi-GiB payloads simply to open an
        // image. Unsupported compressed payloads and large unhashed streams
        // are always reported as incomplete, never as verified.
        constexpr std::uint64_t per_resource_limit=32ull*1024ull*1024ull;
        constexpr std::uint64_t total_limit=128ull*1024ull*1024ull;
        std::uint64_t used=0;
        for(const auto& [hash,unused]:unique_references) {
            (void)unused;
            const auto& entry=resources.at(hash);
            if(entry.stored>per_resource_limit ||
               entry.original>per_resource_limit ||
               used>total_limit-entry.original) {
                ++result.unverified_payloads;
                if(entry.flags&4u)result.requires_decompression=true;
                continue;
            }
            auto bytes=resource_audit_detail::load(
                in,entry.offset,entry.stored);
            if(entry.flags&4u) {
                const auto chunks=inspect_wim_chunks(
                    bytes,entry.original,chunk_size);
                if(!chunks.valid)
                    throw std::runtime_error(
                        "invalid WIM payload chunk table: "+chunks.reason);
                ++result.chunk_tables_verified;
                if(algorithm!=WimChunkAlgorithm::xpress_huff) {
                    ++result.unverified_payloads;
                    result.requires_decompression=true;
                    continue;
                }
#ifndef _WIN32
                if(chunks.compressed_chunks!=0) {
                    ++result.unverified_payloads;
                    result.requires_decompression=true;
                    continue;
                }
#endif
                bytes=read_wim_chunked_metadata(
                    bytes,entry.original,chunk_size,algorithm);
                ++result.compressed_payloads_verified;
            }else{
                ++result.uncompressed_payloads_verified;
            }
            used+=entry.original;
            if(wim_sha1(bytes)!=hash)
                throw std::runtime_error("WIM payload resource SHA-1 mismatch");
        }
        result.uncompressed_resources_verified=result.unverified_payloads==0;
        result.limited_audit_complete=result.metadata_verified &&
                                      result.references_resolved &&
                                      result.uncompressed_resources_verified;
        result.reason=result.limited_audit_complete ?
            "bounded SHA-1/resource-link audit complete; fast writer DISABLED" :
            "incomplete: unsupported codec or oversized WIM payloads remain unverified";
    }catch(const std::exception& error) {
        // Preserve already verified stages in the diagnostic result. A
        // missing ADS link must not erase a successfully verified metadata
        // SHA-1, and a bad payload must not erase resolved lookup references.
        // Never report the combined audit as complete after any failure.
        result.uncompressed_resources_verified=false;
        result.limited_audit_complete=false;
        result.reason=error.what();
    }
    return result;
}
} // namespace iso_wcx::wim_detail
