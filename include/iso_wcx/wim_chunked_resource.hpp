#pragma once
// Read-only Windows Imaging Format chunk-table inspection and bounded XPRESS
// decompression. No WIMGAPI mount, mutable image handles or writer APIs.
//
// WIM stores compressed resources as an (N-1)-entry chunk offset table,
// followed by N independent chunks. Table entries are relative to the end
// of the table. The default chunk size is 32768 bytes. A chunk that did
// not compress smaller than its original size is stored verbatim.
// The original-size threshold for 8-byte offsets is 4 GiB. We deliberately
// forbid such resources in this 128-MiB metadata-only research decoder.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <compressapi.h>
#endif

namespace iso_wcx::wim_detail {
enum class WimChunkAlgorithm {
    xpress_huff,
    lzx,
    unsupported
};

struct WimChunk {
    std::uint64_t data_offset=0;  // relative to complete stored resource
    std::uint32_t stored_bytes=0;
    std::uint32_t expected_bytes=0;
    bool compressed=false;
};

struct WimChunkLayout {
    bool valid=false;
    std::string reason;
    std::uint32_t chunk_size=0;
    std::uint64_t original_bytes=0;
    std::uint64_t stored_bytes=0;
    std::uint64_t table_bytes=0;
    std::uint32_t compressed_chunks=0;
    std::vector<WimChunk> chunks;
};

namespace wim_chunk_detail {
inline void require(bool condition,const char* message) {
    if(!condition)throw std::runtime_error(message);
}
inline std::uint64_t read_le(
        std::span<const std::uint8_t> bytes,std::size_t pos,
        unsigned count) {
    require(pos<=bytes.size() && count<=bytes.size()-pos,
            "truncated WIM chunk table");
    std::uint64_t n=0;
    for(unsigned i=0;i<count;++i)
        n|=std::uint64_t(bytes[pos+i])<<(i*8u);
    return n;
}
} // namespace wim_chunk_detail

// Inspect all chunk bounds before ever attempting decompression.
// No tolerance for overlapping/zero-sized/oversized chunks.
[[nodiscard]] inline WimChunkLayout inspect_wim_chunks(
        std::span<const std::uint8_t> stored,
        std::uint64_t original_size,
        std::uint32_t chunk_size) {
    WimChunkLayout info;
    info.chunk_size=chunk_size;
    info.original_bytes=original_size;
    info.stored_bytes=stored.size();
    try {
        using wim_chunk_detail::require;
        constexpr std::uint64_t limit=128ull*1024ull*1024ull;
        require(original_size!=0 && original_size<=limit &&
                !stored.empty() && stored.size()<=limit,
                "compressed WIM resource exceeds metadata budget");
        require(chunk_size>=4096u && chunk_size<=65536u &&
                (chunk_size&(chunk_size-1u))==0u,
                "invalid WIM compression chunk size");
        const auto count=1u+(original_size-1u)/chunk_size;
        require(count<=32768u,"too many WIM compression chunks");
        const auto offset_width=original_size>=0x100000000ull?8u:4u;
        const auto table=(count-1u)*offset_width;
        require(table<stored.size(),"truncated WIM chunk offsets");
        info.table_bytes=table;
        const auto payload_size=stored.size()-table;
        std::uint64_t previous=0;
        for(std::uint64_t i=0;i<count;++i) {
            const auto next=i+1u==count ?
                payload_size :
                wim_chunk_detail::read_le(
                    stored,static_cast<std::size_t>(i*offset_width),
                    offset_width);
            require(next>previous && next<=payload_size,
                    "WIM chunk offsets not strictly increasing");
            const auto expected=std::min<std::uint64_t>(
                chunk_size,original_size-i*chunk_size);
            const auto encoded=next-previous;
            require(encoded!=0 && encoded<=expected,
                    "WIM chunk size exceeds uncompressed block");
            WimChunk chunk{};
            chunk.data_offset=table+previous;
            chunk.stored_bytes=static_cast<std::uint32_t>(encoded);
            chunk.expected_bytes=static_cast<std::uint32_t>(expected);
            chunk.compressed=encoded<expected;
            if(chunk.compressed)++info.compressed_chunks;
            info.chunks.push_back(chunk);
            previous=next;
        }
        require(previous==payload_size,
                "WIM chunk table does not cover full resource");
        info.valid=true;
        info.reason="WIM chunk offsets and bounds validated read-only";
    }catch(const std::exception& error) {
        info.valid=false;
        info.reason=error.what();
        info.chunks.clear();
    }
    return info;
}

#ifdef _WIN32
// Only OS-provided Cabinet.dll: dynamically resolved so TC never requires
// a second bundled DLL. XPRESS as used in ordinary WIM archives is the
// Huffman variant; raw mode accepts each independent WIM chunk as a block.
// WIM LZX is deliberately NOT mapped to XPRESS or LZMS.
class WimNativeXpress {
public:
    WimNativeXpress() {
        dll_=LoadLibraryExW(L"Cabinet.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!dll_)throw std::runtime_error("Windows Cabinet compression API missing");
        create_=reinterpret_cast<Create>(
            GetProcAddress(dll_,"CreateDecompressor"));
        decode_=reinterpret_cast<Decode>(
            GetProcAddress(dll_,"Decompress"));
        close_=reinterpret_cast<Close>(
            GetProcAddress(dll_,"CloseDecompressor"));
        if(!create_ || !decode_ || !close_) {
            FreeLibrary(dll_);
            dll_=nullptr;
            throw std::runtime_error("Windows Cabinet decompressor unavailable");
        }
        if(!create_(COMPRESS_ALGORITHM_XPRESS_HUFF|COMPRESS_RAW,
                    nullptr,&handle_)) {
            const auto code=GetLastError();
            FreeLibrary(dll_);
            dll_=nullptr;
            throw std::runtime_error(
                "Windows raw XPRESS decompressor unavailable (Win32 "+
                std::to_string(code)+")");
        }
    }
    WimNativeXpress(const WimNativeXpress&)=delete;
    WimNativeXpress& operator=(const WimNativeXpress&)=delete;
    ~WimNativeXpress() {
        if(handle_)close_(handle_);
        if(dll_)FreeLibrary(dll_);
    }
    void operator()(std::span<const std::uint8_t> compressed,
                    std::span<std::uint8_t> output) const {
        SIZE_T got=0;
        if(!decode_(handle_,compressed.data(),compressed.size(),
                    output.data(),output.size(),&got) ||
           got!=output.size())
            throw std::runtime_error("WIM XPRESS chunk decompression failed");
    }
private:
    using Create=BOOL(WINAPI*)(DWORD,PCOMPRESS_ALLOCATION_ROUTINES,
                                PDECOMPRESSOR_HANDLE);
    using Decode=BOOL(WINAPI*)(DECOMPRESSOR_HANDLE,LPCVOID,SIZE_T,
                                PVOID,SIZE_T,PSIZE_T);
    using Close=BOOL(WINAPI*)(DECOMPRESSOR_HANDLE);
    HMODULE dll_=nullptr;
    Create create_=nullptr;
    Decode decode_=nullptr;
    Close close_=nullptr;
    DECOMPRESSOR_HANDLE handle_=nullptr;
};
#endif

// Caller-supplied callback allows testing the entire WIM container layout
// without a particular native decompression library. The only writer is a
// bounded *memory* buffer; no source WIM file is ever modified.
template<class Decode>
[[nodiscard]] inline std::vector<std::uint8_t> expand_wim_chunks(
        std::span<const std::uint8_t> stored,
        const WimChunkLayout& layout,Decode&& decompress) {
    if(!layout.valid || layout.stored_bytes!=stored.size() ||
       layout.original_bytes>128ull*1024ull*1024ull)
        throw std::runtime_error("WIM chunk layout not validated");
    // Layout is a public value type: valid=true is not a trust boundary.
    // Rebind every descriptor to the current table before copying or calling
    // a decoder. In particular, a forged raw descriptor must never copy more
    // bytes than its destination span holds.
    const auto verified=inspect_wim_chunks(
        stored,layout.original_bytes,layout.chunk_size);
    if(!verified.valid || layout.table_bytes!=verified.table_bytes ||
       layout.compressed_chunks!=verified.compressed_chunks ||
       layout.chunks.size()!=verified.chunks.size())
        throw std::runtime_error("WIM chunk layout changed after validation");
    for(std::size_t i=0;i<verified.chunks.size();++i) {
        const auto& actual=verified.chunks[i];
        const auto& supplied=layout.chunks[i];
        if(supplied.data_offset!=actual.data_offset ||
           supplied.stored_bytes!=actual.stored_bytes ||
           supplied.expected_bytes!=actual.expected_bytes ||
           supplied.compressed!=actual.compressed)
            throw std::runtime_error("WIM chunk descriptor changed after validation");
    }
    std::vector<std::uint8_t> output(
        static_cast<std::size_t>(layout.original_bytes));
    std::size_t next=0;
    for(const auto& chunk:layout.chunks) {
        if(chunk.data_offset>stored.size() ||
           chunk.stored_bytes>stored.size()-chunk.data_offset ||
           chunk.expected_bytes>output.size()-next)
            throw std::runtime_error("invalid validated WIM chunk bounds");
        const auto encoded=stored.subspan(
            static_cast<std::size_t>(chunk.data_offset),
            chunk.stored_bytes);
        auto decoded=std::span<std::uint8_t>(
            output.data()+next,chunk.expected_bytes);
        if(chunk.compressed) {
            decompress(encoded,decoded);
        }else{
            std::copy(encoded.begin(),encoded.end(),decoded.begin());
        }
        next+=decoded.size();
    }
    if(next!=output.size())
        throw std::runtime_error("WIM chunk output length mismatch");
    return output;
}

// Uncompressed chunks are independently readable on all platforms. Actual
// XPRESS decoding is supported by the Windows system compression API only;
// the caller MUST SHA-1 verify the fully expanded resource afterwards.
[[nodiscard]] inline std::vector<std::uint8_t> read_wim_chunked_metadata(
        std::span<const std::uint8_t> stored,
        std::uint64_t original_size,
        std::uint32_t chunk_size,
        WimChunkAlgorithm algorithm) {
    const auto layout=inspect_wim_chunks(stored,original_size,chunk_size);
    if(!layout.valid)throw std::runtime_error(layout.reason);
    if(layout.compressed_chunks==0)
        return expand_wim_chunks(
            stored,layout,[](auto,auto) {
                throw std::runtime_error("unexpected compressed WIM chunk");
            });
    if(algorithm!=WimChunkAlgorithm::xpress_huff)
        throw std::runtime_error(
            "WIM LZX/LZMS decoding not implemented; resource is unverified");
#ifdef _WIN32
    WimNativeXpress decoder;
    return expand_wim_chunks(stored,layout,decoder);
#else
    throw std::runtime_error(
        "WIM XPRESS decoding unavailable on this platform");
#endif
}
} // namespace iso_wcx::wim_detail
