#include "iso_wcx/wim_chunked_resource.hpp"
#include "iso_wcx/wim_sha1.hpp"
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace iso_wcx::wim_detail;

static void check(bool condition,const char* message) {
    if(!condition)throw std::runtime_error(message);
}
static void put32(std::vector<std::uint8_t>& b,std::size_t pos,
                  std::uint32_t value) {
    for(unsigned i=0;i<4;++i)
        b[pos+i]=static_cast<std::uint8_t>(value>>(i*8u));
}
static void expect_rejection(
        std::span<const std::uint8_t> bytes,std::uint64_t size,
        std::uint32_t chunk_size,const char* error) {
    const auto layout=inspect_wim_chunks(bytes,size,chunk_size);
    if(layout.valid)throw std::runtime_error(error);
}

static void test_layout_and_rejections() {
    constexpr std::uint32_t chunk=32768u;
    // The first chunk is stored raw, the second is a stand-in for
    // compressed data, and the last 13 bytes are again stored raw.
    std::vector<std::uint8_t> encoded(8u+chunk+11u+13u);
    put32(encoded,0u,chunk);
    put32(encoded,4u,chunk+11u);
    std::fill(encoded.begin()+8,encoded.begin()+8+chunk,0x41u);
    std::fill(encoded.begin()+8+chunk,
              encoded.begin()+8+chunk+11u,0x5cu);
    std::fill(encoded.end()-13,encoded.end(),0x43u);
    const auto layout=inspect_wim_chunks(encoded,2ull*chunk+13u,chunk);
    check(layout.valid && layout.chunks.size()==3u &&
          layout.compressed_chunks==1u && layout.table_bytes==8u &&
          layout.chunks[1].compressed &&
          layout.chunks[1].data_offset==8u+chunk &&
          layout.chunks[2].expected_bytes==13u,
          "multi-chunk offset layout was interpreted incorrectly");
    unsigned calls=0;
    const auto decoded=expand_wim_chunks(
        encoded,layout,[&](auto input,auto output) {
            ++calls;
            check(input.size()==11u && output.size()==chunk,
                  "compressed block size or output length mismatched");
            std::fill(output.begin(),output.end(),0x42u);
        });
    check(calls==1 && decoded.size()==2ull*chunk+13u &&
          std::all_of(decoded.begin(),decoded.begin()+chunk,
                      [](auto x){return x==0x41u;}) &&
          std::all_of(decoded.begin()+chunk,decoded.begin()+2u*chunk,
                      [](auto x){return x==0x42u;}) &&
          std::all_of(decoded.end()-13,decoded.end(),
                      [](auto x){return x==0x43u;}),
          "bounded multi-chunk reconstruction is incorrect");

    // A fully raw resource can still be encoded with the WIM chunk table.
    auto raw=encoded;
    raw.erase(raw.begin()+static_cast<std::ptrdiff_t>(8u+chunk),
              raw.begin()+static_cast<std::ptrdiff_t>(8u+chunk+11u));
    raw.insert(raw.begin()+static_cast<std::ptrdiff_t>(8u+chunk),
               chunk,0x42u);
    put32(raw,4u,2u*chunk);
    const auto restored=read_wim_chunked_metadata(
        raw,2ull*chunk+13u,chunk,WimChunkAlgorithm::xpress_huff);
    check(restored==decoded,
          "copy of raw WIM chunks changed the original resource bytes");

    auto bad=encoded;
    put32(bad,0u,0u);
    expect_rejection(bad,2ull*chunk+13u,chunk,"zero WIM chunk offset accepted");
    bad=encoded;
    put32(bad,4u,chunk);
    expect_rejection(bad,2ull*chunk+13u,chunk,"duplicate WIM offset accepted");
    bad=encoded;
    put32(bad,0u,chunk+12u);
    put32(bad,4u,chunk+11u);
    expect_rejection(bad,2ull*chunk+13u,chunk,"descending offsets accepted");
    bad=encoded;
    put32(bad,0u,0xffffffffu);
    expect_rejection(bad,2ull*chunk+13u,chunk,"out-of-bounds offset accepted");
    bad=encoded;
    bad.resize(4u);
    expect_rejection(bad,2ull*chunk+13u,chunk,"truncated offset table accepted");
    expect_rejection(encoded,2ull*chunk+13u,0u,"zero chunk size accepted");
    expect_rejection(encoded,2ull*chunk+13u,33333u,"non-power-of-two chunk accepted");
    expect_rejection(encoded,2ull*chunk+13u,128u*1024u,"oversized chunk accepted");
    expect_rejection(encoded,129ull*1024ull*1024ull,chunk,
                     "resource exceeding bounded memory budget accepted");
    bool rejected=false;
    try {
        (void)read_wim_chunked_metadata(
            encoded,2ull*chunk+13u,chunk,WimChunkAlgorithm::lzx);
    }catch(const std::exception&) {rejected=true;}
    check(rejected,"LZX resource was incorrectly passed through XPRESS");

    // Single-chunk resources legitimately have no offset table.
    std::vector<std::uint8_t> one(47u,0x55u);
    const auto one_layout=inspect_wim_chunks(one,47u,chunk);
    check(one_layout.valid && one_layout.table_bytes==0u &&
          one_layout.compressed_chunks==0u,
          "one-chunk raw WIM resource rejected");
}

static void test_modified_layout_refusal() {
    constexpr std::uint32_t chunk=4096u;
    std::vector<std::uint8_t> stored(4u+chunk+17u,0x41u);
    put32(stored,0u,chunk);
    const auto layout=inspect_wim_chunks(stored,chunk+17u,chunk);
    check(layout.valid,"raw layout fixture invalid");
    const auto reject=[&](const auto& bytes,const auto& candidate) {
        bool rejected=false;
        unsigned calls=0;
        try {
            (void)expand_wim_chunks(bytes,candidate,
                [&](auto,auto){++calls;});
        }catch(const std::runtime_error&) {rejected=true;}
        check(rejected && calls==0,
              "modified layout reached copy or decompressor");
    };
    auto bad=layout;
    // Previously this copied 4096 bytes into an eight-byte output span.
    bad.original_bytes=8u;
    bad.chunks.resize(1u);
    bad.chunks[0].expected_bytes=8u;
    reject(stored,bad);
    bad=layout; bad.chunks[0].expected_bytes=1u; reject(stored,bad);
    bad=layout; bad.chunks[0].stored_bytes+=1u; reject(stored,bad);
    bad=layout; bad.chunks[0].data_offset=0u; reject(stored,bad);
    bad=layout; bad.chunks[0].compressed=true; reject(stored,bad);
    bad=layout; bad.chunks.pop_back(); reject(stored,bad);
    bad=layout; bad.chunks.push_back(bad.chunks.back()); reject(stored,bad);
    bad=layout; bad.table_bytes=0u; reject(stored,bad);
    bad=layout; bad.compressed_chunks=1u; reject(stored,bad);
    bad=layout; bad.chunk_size=0u; reject(stored,bad);
    bad=layout; bad.original_bytes=0u; reject(stored,bad);
    // Same-sized storage with a changed table must not reuse old descriptors.
    auto changed=stored;
    put32(changed,0u,chunk-1u);
    reject(changed,layout);
    // Payload changes do not change layout; SHA-1 remains the caller's job.
    stored.back()=0x55u;
    const auto output=expand_wim_chunks(stored,layout,[](auto,auto){});
    check(output.size()==chunk+17u && output.back()==0x55u,
          "valid raw resource rejected after payload-only change");
}

#ifdef _WIN32
static void test_native_xpress() {
    // Generate genuine raw XPRESS+Huffman input with the documented
    // Windows system compressor. No canned platform-specific fixtures.
    HMODULE dll=LoadLibraryExW(
        L"Cabinet.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    check(dll!=nullptr,"Windows Cabinet.dll was not found");
    using Create=BOOL(WINAPI*)(DWORD,PCOMPRESS_ALLOCATION_ROUTINES,
                                PCOMPRESSOR_HANDLE);
    using Encode=BOOL(WINAPI*)(COMPRESSOR_HANDLE,LPCVOID,SIZE_T,
                               PVOID,SIZE_T,PSIZE_T);
    using Close=BOOL(WINAPI*)(COMPRESSOR_HANDLE);
    const auto create=reinterpret_cast<Create>(
        GetProcAddress(dll,"CreateCompressor"));
    const auto encode=reinterpret_cast<Encode>(
        GetProcAddress(dll,"Compress"));
    const auto close=reinterpret_cast<Close>(
        GetProcAddress(dll,"CloseCompressor"));
    check(create && encode && close,"Windows raw XPRESS compressor unavailable");
    COMPRESSOR_HANDLE handle=nullptr;
    check(create(COMPRESS_ALGORITHM_XPRESS_HUFF|COMPRESS_RAW,
                 nullptr,&handle)!=FALSE,
          "could not create a raw XPRESS-HUFF compressor");
    try {
        std::vector<std::uint8_t> original(32768u,0x51u);
        for(std::size_t i=0;i<original.size();i+=64u)
            original[i]=static_cast<std::uint8_t>((i/64u)%251u);
        std::vector<std::uint8_t> compressed(2u*original.size());
        SIZE_T length=0;
        std::cerr<<"WIM chunk test: Cabinet Compress begin\n"<<std::flush;
        check(encode(handle,original.data(),original.size(),
                     compressed.data(),compressed.size(),&length)!=FALSE &&
              length>0u && length<original.size(),
              "XPRESS-HUFF did not produce a smaller raw chunk");
        std::cerr<<"WIM chunk test: Cabinet Compress passed\n"<<std::flush;
        compressed.resize(length);
        std::cerr<<"WIM chunk test: XPRESS decode begin\n"<<std::flush;
        const auto result=read_wim_chunked_metadata(
            compressed,original.size(),32768u,
            WimChunkAlgorithm::xpress_huff);
        check(result==original && wim_sha1(result)==wim_sha1(original),
              "Windows raw XPRESS-HUFF roundtrip differs from original SHA-1");

        std::cerr<<"WIM chunk test: XPRESS decode passed\n"<<std::flush;
        // Compressed first chunk plus a raw, short last chunk.
        std::vector<std::uint8_t> mixed(4u+compressed.size()+17u,0x7bu);
        put32(mixed,0u,static_cast<std::uint32_t>(compressed.size()));
        std::copy(compressed.begin(),compressed.end(),mixed.begin()+4);
        std::cerr<<"WIM chunk test: mixed decode begin\n"<<std::flush;
        const auto expanded=read_wim_chunked_metadata(
            mixed,32768u+17u,32768u,WimChunkAlgorithm::xpress_huff);
        check(expanded.size()==32768u+17u &&
              std::equal(expanded.begin(),expanded.begin()+32768u,
                         original.begin()) &&
              std::all_of(expanded.end()-17,expanded.end(),
                          [](auto v){return v==0x7bu;}),
              "mixed XPRESS and raw WIM chunk roundtrip failed");
    }catch(...){
        close(handle);
        FreeLibrary(dll);
        throw;
    }
    close(handle);
    FreeLibrary(dll);
}
#endif

int main() {
    try {
        std::cerr<<"WIM chunk test: layout begin\n"<<std::flush;
        test_layout_and_rejections();
        std::cerr<<"WIM chunk test: layout passed; modified layout begin\n"<<std::flush;
        test_modified_layout_refusal();
        std::cerr<<"WIM chunk test: modified layout passed\n"<<std::flush;
#ifdef _WIN32
        std::cerr<<"WIM chunk test: native XPRESS begin\n"<<std::flush;
        test_native_xpress();
        std::cerr<<"WIM chunk test: native XPRESS passed\n"<<std::flush;
#endif
        std::cout<<"PASS: read-only bounded WIM chunks, corruption refusal, "
                    "raw/XPRESS roundtrip, LZX refusal\n";
        return 0;
    }catch(const std::exception& error) {
        std::cerr<<"FAIL WIM chunk decoder: "<<error.what()<<'\n';
        return 1;
    }
}
