#include "iso_wcx/wim_resource_audit.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <thread>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <RestartManager.h>
#ifdef _MSC_VER
#pragma comment(lib, "Rstrtmgr.lib")
#endif
#endif

using namespace iso_wcx::wim_detail;
namespace fs=std::filesystem;

static void require(bool cond,const char* error) {
    if(!cond)throw std::runtime_error(error);
}
static void put16(std::vector<std::uint8_t>& a,std::size_t p,std::uint16_t n) {
    for(unsigned i=0;i<2;++i)a[p+i]=static_cast<std::uint8_t>(n>>(i*8u));
}
static void put32(std::vector<std::uint8_t>& a,std::size_t p,std::uint32_t n) {
    for(unsigned i=0;i<4;++i)a[p+i]=static_cast<std::uint8_t>(n>>(i*8u));
}
static void put64(std::vector<std::uint8_t>& a,std::size_t p,std::uint64_t n) {
    for(unsigned i=0;i<8;++i)a[p+i]=static_cast<std::uint8_t>(n>>(i*8u));
}
static void store_digest(std::vector<std::uint8_t>& a,std::size_t p,
                         const WimSha1Digest& digest) {
    std::copy(digest.begin(),digest.end(),a.begin()+static_cast<std::ptrdiff_t>(p));
}
static std::vector<std::uint8_t> bytes(std::string_view v) {
    return {v.begin(),v.end()};
}
static std::string hex(const WimSha1Digest& hash) {
    constexpr char alphabet[]="0123456789abcdef";
    std::string s;
    for(auto byte:hash) {
        s.push_back(alphabet[byte>>4]);
        s.push_back(alphabet[byte&15u]);
    }
    return s;
}
static void descriptor(std::vector<std::uint8_t>& a,
                       std::size_t p,std::uint64_t stored,
                       std::uint8_t flags,std::uint64_t off,
                       const WimSha1Digest& digest) {
    for(unsigned i=0;i<7;++i)
        a[p+i]=static_cast<std::uint8_t>(stored>>(i*8u));
    a[p+7]=flags;
    put64(a,p+8,off);
    put64(a,p+16,stored);
    put16(a,p+24,1u);
    put32(a,p+26,1u);
    store_digest(a,p+30,digest);
}
static std::vector<std::uint8_t> metadata(
        const WimSha1Digest& file_hash,const WimSha1Digest& ads_hash) {
    // Security table, root, one file and one alternate stream. All sizes
    // and offsets follow the uncompressed Windows Imaging Format layout.
    std::vector<std::uint8_t> b(320);
    put32(b,0,8);
    put32(b,4,0);
    put64(b,8,104);
    put32(b,16,0x10u);
    put32(b,20,0xffffffffu);
    put64(b,24,120); // root's children at 120
    // Root sibling terminator at 112
    put64(b,120,112);
    put32(b,128,0x80u); // regular file
    put32(b,132,0xffffffffu);
    store_digest(b,184,file_hash); // child+64
    put16(b,216,1u); // child+96 has one ADS
    put16(b,220,6u);
    put16(b,222,'N');put16(b,224,'E');put16(b,226,'W');
    // child ends at 232, first stream is at 232
    put64(b,232,48u);
    store_digest(b,248,ads_hash); // stream+16
    put16(b,268,6u); // stream+36; name length 6 bytes
    put16(b,270,'A');put16(b,272,'D');put16(b,274,'S');
    // Terminate directory list at offset 280
    return b;
}
static std::vector<std::uint8_t> image(
        std::uint8_t payload_flags=0,std::uint8_t metadata_flags=2,
        bool omit_ads_resource=false) {
    const auto data=bytes("HELLO-WIM-RESOURCE");
    const auto ads=bytes("ALTERNATE-STREAM");
    const auto fh=wim_sha1(data);
    const auto sh=wim_sha1(ads);
    const auto meta=metadata(fh,sh);
    constexpr std::size_t header=208;
    const std::size_t file_at=header+meta.size();
    const std::size_t ads_at=file_at+data.size();
    const std::size_t lookup_at=ads_at+ads.size();
    const std::size_t descriptors=omit_ads_resource?2u:3u;
    std::vector<std::uint8_t> out(lookup_at+50u*descriptors);
    const std::array<std::uint8_t,8> magic{'M','S','W','I','M',0,0,0};
    std::copy(magic.begin(),magic.end(),out.begin());
    put32(out,8,208);
    put32(out,12,0x00010d00u);
    put16(out,40,1u);put16(out,42,1u);put32(out,44,1u);
    for(std::size_t i=0;i<7;++i)
        out[48+i]=static_cast<std::uint8_t>(
            (50u*descriptors)>>(8u*i));
    put64(out,56,lookup_at);
    put64(out,64,50u*descriptors);
    std::copy(meta.begin(),meta.end(),
              out.begin()+static_cast<std::ptrdiff_t>(header));
    std::copy(data.begin(),data.end(),
              out.begin()+static_cast<std::ptrdiff_t>(file_at));
    std::copy(ads.begin(),ads.end(),
              out.begin()+static_cast<std::ptrdiff_t>(ads_at));
    descriptor(out,lookup_at,meta.size(),metadata_flags,header,
               wim_sha1(meta));
    descriptor(out,lookup_at+50,data.size(),payload_flags,file_at,fh);
    if(!omit_ads_resource)
        descriptor(out,lookup_at+100,ads.size(),0,ads_at,sh);
    return out;
}
// Repack a throwaway synthetic WIM with a chunked metadata resource.
// The new metadata remains SHA-1-addressed by its *expanded* 320-byte form.
// The header, lookup and file/ADS offsets must all move consistently.
static std::vector<std::uint8_t> chunked_metadata_image(
        const std::vector<std::uint8_t>& original,
        const std::vector<std::uint8_t>& stored,
        std::uint32_t header_flags=0x00020002u,
        std::size_t expanded_metadata_size=320u) {
    constexpr std::size_t header=208u,plain_metadata=320u;
    require(original.size()>=header+plain_metadata+150u &&
            expanded_metadata_size>=plain_metadata &&
            expanded_metadata_size<=128u*1024u &&
            !stored.empty() && stored.size()<=expanded_metadata_size,
            "invalid disposable WIM metadata fixture");
    const auto old_lookup=fast_probe_detail::le64(original.data()+56);
    require(old_lookup>=header+plain_metadata &&
            old_lookup+150u==original.size(),
            "fixture WIM lookup dimensions changed");
    const auto new_lookup=old_lookup-plain_metadata+stored.size();
    std::vector<std::uint8_t> result(
        original.size()-plain_metadata+stored.size());
    std::copy(original.begin(),original.begin()+header,result.begin());
    std::copy(stored.begin(),stored.end(),result.begin()+header);
    std::copy(original.begin()+header+plain_metadata,original.end(),
              result.begin()+header+stored.size());
    put32(result,16,header_flags);
    put32(result,20,32768u);
    put64(result,56,new_lookup);
    // The throwaway physical WIM may store a larger padded metadata
    // resource. The 320-byte directory tree remains intact while the
    // padded expanded form is SHA-1 authenticated as one resource.
    std::vector<std::uint8_t> expanded(expanded_metadata_size);
    std::copy(original.begin()+header,
              original.begin()+header+plain_metadata,expanded.begin());
    const auto digest=wim_sha1(expanded);
    descriptor(result,new_lookup,stored.size(),6u,header,digest);
    put64(result,new_lookup+16u,expanded_metadata_size);
    const auto delta=static_cast<std::int64_t>(stored.size())-
                     static_cast<std::int64_t>(plain_metadata);
    for(std::size_t i=1;i<3;++i) {
        const auto entry=new_lookup+50u*i;
        const auto old=fast_probe_detail::le64(
            original.data()+old_lookup+50u*i+8u);
        put64(result,entry+8u,
              static_cast<std::uint64_t>(
                  static_cast<std::int64_t>(old)+delta));
    }
    return result;
}

#ifdef _WIN32
// The actual compression bytes are independently produced by Windows,
// not hardcoded/canned XPRESS fixtures. Read-only WIM audit then expands
// them through its own decompressor and must verify the original SHA-1.
static std::vector<std::uint8_t> native_xpress(
        std::span<const std::uint8_t> input) {
    HMODULE dll=LoadLibraryExW(
        L"Cabinet.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    require(dll!=nullptr,"Windows Cabinet.dll unavailable in WIM test");
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
    require(create && encode && close,
            "Windows XPRESS-HUFF compression functions missing");
    COMPRESSOR_HANDLE handle=nullptr;
    const bool created=create(
        COMPRESS_ALGORITHM_XPRESS_HUFF|COMPRESS_RAW,
        nullptr,&handle)!=FALSE;
    require(created,"Windows raw XPRESS-HUFF compressor creation failed");
    std::vector<std::uint8_t> output(input.size()*2u+4096u);
    SIZE_T length=0;
    const bool ok=encode(handle,input.data(),input.size(),
                         output.data(),output.size(),&length)!=FALSE;
    close(handle);
    FreeLibrary(dll);
    require(ok && length>0u && length<input.size(),
            "Windows XPRESS-HUFF metadata compression did not reduce size");
    output.resize(length);
    return output;
}

// The 32-KiB synthetic file resource is XPRESS compressed; metadata
// points to the SHA-1 of its expanded form, while an ADS remains plain.
static std::vector<std::uint8_t> chunked_file_image(
        const std::vector<std::uint8_t>& original,
        std::span<const std::uint8_t> plain,
        std::span<const std::uint8_t> stored) {
    constexpr std::size_t header=208u,metadata_length=320u;
    const auto old_lookup=fast_probe_detail::le64(original.data()+56);
    const auto old_file_at=header+metadata_length;
    const auto old_file_bytes=bytes("HELLO-WIM-RESOURCE").size();
    const auto new_lookup=old_lookup-old_file_bytes+stored.size();
    auto ads_hash=wim_sha1(bytes("ALTERNATE-STREAM"));
    auto new_metadata=metadata(wim_sha1(plain),ads_hash);
    std::vector<std::uint8_t> result(
        original.size()-old_file_bytes+stored.size());
    std::copy(original.begin(),original.begin()+header,result.begin());
    std::copy(new_metadata.begin(),new_metadata.end(),
              result.begin()+header);
    std::copy(stored.begin(),stored.end(),
              result.begin()+old_file_at);
    std::copy(original.begin()+old_file_at+old_file_bytes,original.end(),
              result.begin()+old_file_at+stored.size());
    put32(result,16,0x00020002u);
    put32(result,20,32768u);
    put64(result,56,new_lookup);
    descriptor(result,new_lookup,metadata_length,2u,header,
               wim_sha1(new_metadata));
    descriptor(result,new_lookup+50u,stored.size(),4u,old_file_at,
               wim_sha1(plain));
    put64(result,new_lookup+50u+16u,plain.size());
    const auto old_ads=fast_probe_detail::le64(
        original.data()+old_lookup+100u+8u);
    put64(result,new_lookup+100u+8u,
          old_ads-old_file_bytes+stored.size());
    return result;
}
#endif

static void write(const fs::path& path,
                  const std::vector<std::uint8_t>& input) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    require(bool(out),"cannot write disposable WIM fixture");
    out.write(reinterpret_cast<const char*>(input.data()),
              static_cast<std::streamsize>(input.size()));
    require(bool(out),"cannot finish disposable WIM fixture");
}
static void remove_fixture(const fs::path& path) {
    std::error_code ec;
    for (unsigned attempt=0;attempt<20;++attempt) {
        ec.clear();
        if (fs::remove(path,ec) || !fs::exists(path)) return;
        // Windows ERROR_SHARING_VIOLATION (32) is not consistently mapped
        // to std::errc::device_or_resource_busy by std::filesystem.
        // Retry this transient case too; do not swallow other I/O failures.
        bool retryable=ec==std::errc::permission_denied ||
                       ec==std::errc::device_or_resource_busy;
#ifdef _WIN32
        retryable=retryable || (ec.category()==std::system_category() &&
                                ec.value()==32);
#endif
        if (!retryable) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
#ifdef _WIN32
    DWORD rm_handle=0;
    WCHAR rm_key[CCH_RM_SESSION_KEY+1]{};
    const DWORD rm_start=RmStartSession(&rm_handle,0,rm_key);
    if(rm_start==ERROR_SUCCESS) {
        LPCWSTR files[]={path.c_str()};
        const DWORD registered=RmRegisterResources(rm_handle,1,files,0,nullptr,0,nullptr);
        if(registered==ERROR_SUCCESS) {
            UINT needed=0,provided=0;
            DWORD reboot=0;
            DWORD result=RmGetList(rm_handle,&needed,&provided,nullptr,&reboot);
            std::vector<RM_PROCESS_INFO> owners(needed);
            if(result==ERROR_MORE_DATA && needed) {
                provided=needed;
                result=RmGetList(rm_handle,&needed,&provided,owners.data(),&reboot);
            }
            std::cerr<<"WIM_RESTART_MANAGER result="<<result
                     <<" owners="<<(result==ERROR_SUCCESS?provided:0)<<"\n";
            if(result==ERROR_SUCCESS)
                for(UINT i=0;i<provided;++i)
                    std::cerr<<"WIM_LOCK_OWNER pid="<<owners[i].Process.dwProcessId
                             <<" self="<<(owners[i].Process.dwProcessId==GetCurrentProcessId())<<"\n";
        } else std::cerr<<"WIM_RESTART_MANAGER register_error="<<registered<<"\n";
        RmEndSession(rm_handle);
    } else std::cerr<<"WIM_RESTART_MANAGER start_error="<<rm_start<<"\n";
    const auto shared=CreateFileW(path.c_str(),GENERIC_READ,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(shared==INVALID_HANDLE_VALUE)
        std::cerr<<"WIM_LOCK_PROBE shared_error="<<GetLastError()<<"\n";
    else {
        CloseHandle(shared);
        const auto exclusive=CreateFileW(path.c_str(),GENERIC_READ,
            0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(exclusive==INVALID_HANDLE_VALUE)
            std::cerr<<"WIM_LOCK_PROBE exclusive_error="<<GetLastError()<<"\n";
        else {
            std::cerr<<"WIM_LOCK_PROBE exclusive_open=OK\n";
            CloseHandle(exclusive);
        }
    }
#endif
    throw fs::filesystem_error("cannot remove WIM audit fixture",path,ec);
}
int main(){
    const auto path=fs::temp_directory_path()/
        ("iso-wcx-wim-resource-audit-readonly-fixture-"+
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".fixture");
    try {
        const auto empty=wim_sha1(std::span<const std::uint8_t>{});
        require(hex(empty)=="da39a3ee5e6b4b0d3255bfef95601890afd80709",
                "SHA-1 empty vector mismatch");
        require(hex(wim_sha1(bytes("abc")))==
                "a9993e364706816aba3e25717850c26c9cd0d89d",
                "SHA-1 abc vector mismatch");
        std::vector<std::uint8_t> million(1000000u,'a');
        require(hex(wim_sha1(million))==
                "34aa973cd4c4daa4f61eeb2bdbad27316534016f",
                "SHA-1 multi-block vector mismatch");
        auto original=image();
        write(path,original);
        auto good=audit_wim_resource_links(path);
        if(!good.limited_audit_complete)
            std::cerr<<"Unexpected incomplete audit: "<<good.reason<<"\n";
        require(good.metadata_verified && good.references_resolved &&
                good.limited_audit_complete && good.images==1u &&
                good.referenced_hashes==2u &&
                good.metadata_resources_verified==1u &&
                good.uncompressed_payloads_verified==2u,
                "valid metadata, normal file or ADS hashes not verified");
        {
            std::ifstream in(path,std::ios::binary);
            const std::vector<std::uint8_t> unchanged(
                (std::istreambuf_iterator<char>(in)),
                std::istreambuf_iterator<char>{});
            require(unchanged==original,"read-only audit modified WIM bytes");
        }
        auto broken=original;
        broken[208+184]^=0x80u; // metadata child file stream hash
        write(path,broken);
        require(!audit_wim_resource_links(path).metadata_verified,
                "metadata resource SHA-1 corruption accepted");
        broken=image(0,2,true);
        write(path,broken);
        const auto missing=audit_wim_resource_links(path);
        require(missing.metadata_verified && !missing.references_resolved &&
                !missing.limited_audit_complete,
                "missing ADS lookup resource accepted");
        broken=original;
        broken[208+320]^=0x01u; // corrupt plain file payload, not lookup
        write(path,broken);
        const auto payload=audit_wim_resource_links(path);
        require(payload.metadata_verified && payload.references_resolved &&
                !payload.limited_audit_complete &&
                payload.reason.find("SHA-1 mismatch")!=std::string::npos,
                "payload SHA-1 corruption accepted");
        broken=image(4); // content is marked compressed: refuse full hash claim
        write(path,broken);
        const auto compressed=audit_wim_resource_links(path);
        require(compressed.metadata_verified && compressed.references_resolved &&
                compressed.requires_decompression &&
                compressed.unverified_payloads==1u &&
                !compressed.limited_audit_complete,
                "compressed payload falsely reported verified");
        // Metadata marked chunked but stored entirely raw must be
        // recognized even without a Windows decompressor.
        auto chunked_raw=chunked_metadata_image(original,
            std::vector<std::uint8_t>(
                original.begin()+208,original.begin()+208+320));
        write(path,chunked_raw);
        const auto raw_result=audit_wim_resource_links(path);
        if(!raw_result.limited_audit_complete)
            std::cerr<<"Chunked raw WIM audit: "<<raw_result.reason<<"\n";
        require(raw_result.metadata_verified &&
                raw_result.compressed_metadata_verified==1u &&
                raw_result.references_resolved &&
                raw_result.limited_audit_complete &&
                raw_result.chunk_tables_verified==1u,
                "raw WIM metadata chunk audit failed");

        // The same bytes cannot be reported verified when the header
        // advertises LZX: no unchecked codec fallback is allowed.
        const auto lzx=chunked_metadata_image(original,
            std::vector<std::uint8_t>(
                original.begin()+208,original.begin()+208+320),
            0x00040002u);
        write(path,lzx);
        const auto lzx_result=audit_wim_resource_links(path);
        require(lzx_result.requires_decompression &&
                !lzx_result.metadata_verified &&
                !lzx_result.limited_audit_complete,
                "LZX was incorrectly sent to the XPRESS decoder");
#ifdef _WIN32
        // Huffman has fixed coding overhead: 320-byte metadata is too
        // small to compress efficiently. Pad the harmless test resource
        // to 8 KiB so it exercises a genuinely compressed XPRESS chunk.
        std::vector<std::uint8_t> expanded(8192u);
        std::copy(original.begin()+208,original.begin()+208+320,
                  expanded.begin());
        const auto compressed_bytes=native_xpress(expanded);
        const auto compressed_wim=chunked_metadata_image(
            original,compressed_bytes,0x00020002u,expanded.size());
        write(path,compressed_wim);
        const auto compressed_metadata=audit_wim_resource_links(path);
        if(!compressed_metadata.limited_audit_complete)
            std::cerr<<"Genuine XPRESS WIM metadata audit: "
                     <<compressed_metadata.reason<<"\n";
        require(compressed_metadata.limited_audit_complete &&
                compressed_metadata.metadata_verified &&
                compressed_metadata.compressed_metadata_verified==1u &&
                compressed_metadata.uncompressed_payloads_verified==2u,
                "XPRESS-compressed WIM metadata SHA-1 was not verified");
        {
            std::ifstream in(path,std::ios::binary);
            const std::vector<std::uint8_t> unchanged(
                (std::istreambuf_iterator<char>(in)),
                std::istreambuf_iterator<char>{});
            require(unchanged==compressed_wim,
                    "XPRESS WIM read-only audit changed original bytes");
        }
        auto corrupt=compressed_wim;
        corrupt[208u+compressed_bytes.size()/2u]^=0x40u;
        write(path,corrupt);
        const auto bad_xpress=audit_wim_resource_links(path);
        require(!bad_xpress.limited_audit_complete &&
                !bad_xpress.metadata_verified &&
                bad_xpress.compressed_metadata_verified==0u,
                "corrupt XPRESS metadata was counted as verified");

        std::vector<std::uint8_t> large_payload(32768u,0x52u);
        for(std::size_t i=0;i<large_payload.size();i+=64u)
            large_payload[i]=static_cast<std::uint8_t>(i/64u);
        const auto compressed_payload=native_xpress(large_payload);
        const auto payload_wim=chunked_file_image(
            original,large_payload,compressed_payload);
        write(path,payload_wim);
        const auto audited_payload=audit_wim_resource_links(path);
        if(!audited_payload.limited_audit_complete)
            std::cerr<<"XPRESS file/ADS audit: "
                     <<audited_payload.reason<<"\n";
        require(audited_payload.limited_audit_complete &&
                audited_payload.compressed_payloads_verified==1u &&
                audited_payload.uncompressed_payloads_verified==1u,
                "XPRESS file payload and its ADS were not both verified");
        // A corrupt compressed file may fail in the native decoder or only
        // at the SHA-1 stage. Both are failures; never increment "verified".
        auto corrupt_file=payload_wim;
        corrupt_file[208u+320u+compressed_payload.size()/2u]^=0x80u;
        write(path,corrupt_file);
        const auto failed_file=audit_wim_resource_links(path);
        require(!failed_file.limited_audit_complete &&
                failed_file.metadata_verified &&
                failed_file.references_resolved &&
                failed_file.compressed_payloads_verified==0u,
                "corrupt XPRESS file resource was counted as verified");
#endif
        broken=original;
        // Truncated lookup entry must be rejected before metadata traversal.
        broken.resize(broken.size()-1);
        write(path,broken);
        require(!audit_wim_resource_links(path).limited_audit_complete,
                "truncated lookup entry accepted");
        remove_fixture(path);
        std::cout<<"PASS: WIM SHA-1, normal streams, ADS, missing links, "
                    "truncation and compressed-resource refusal\n";
    }catch(const std::exception& e) {
        std::error_code ec;fs::remove(path,ec);
        std::cerr<<"FAIL WIM resource audit: "<<e.what()<<'\n';
        return 1;
    }
    return 0;
}
