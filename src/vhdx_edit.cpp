// Preview: native bounded FAT12/16/32 writer for *fixed fully-mapped* VHDX.
//
// The source file is NEVER modified in place. A private logical RAW is
// edited using the existing verified FAT writer, then changed sectors are
// reflected into a separate physical VHDX clone. Both VHDX header copies
// receive fresh FileWrite/DataWrite GUIDs, a greater sequence and CRC32C.
// The staged VHDX is independently reopened and compared byte-for-byte with
// the edited RAW before the physical file is atomically replaced.
//
// No mounts, VirtDisk API, separate helper executable or parser DLL.
#include "iso_wcx/archive_edit.hpp"
#include "iso_wcx/fat_mapped_image.hpp"
#include "iso_wcx/partition_map.hpp"
#include "iso_wcx/vhdx_reader.hpp"
#include "iso_wcx/virtual_disk.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace iso_wcx {
namespace {
constexpr std::size_t window_bytes=1024u*1024u;
constexpr std::uint64_t MiB=1024ull*1024ull;
constexpr std::uint64_t logical_limit_64=1024ull*MiB;
constexpr std::uint64_t logical_limit_32=512ull*MiB;
constexpr std::uint64_t physical_limit_64=2ull*1024ull*MiB;
constexpr std::uint64_t physical_limit_32=512ull*MiB;

void require(bool ok,const char* reason) {
    if(!ok)throw EditUnsupported(reason);
}
void check_progress(const EditProgress& progress) {
    if(progress && !progress())throw EditCancelled();
}
std::uint64_t logical_limit() noexcept {
    return sizeof(void*)==4 ? logical_limit_32 : logical_limit_64;
}
std::uint64_t physical_limit() noexcept {
    return sizeof(void*)==4 ? physical_limit_32 : physical_limit_64;
}
void write_at(std::fstream& file,std::uint64_t where,
              const std::uint8_t* src,std::size_t len) {
    require(where<=std::uint64_t(std::numeric_limits<std::streamoff>::max()) &&
            len<=std::uint64_t(std::numeric_limits<std::streamoff>::max())-where,
            "fixed VHDX physical stage offset overflow");
    file.clear();
    file.seekp(static_cast<std::streamoff>(where),std::ios::beg);
    require(bool(file),"fixed VHDX physical stage seek failed");
    file.write(reinterpret_cast<const char*>(src),
               static_cast<std::streamsize>(len));
    require(bool(file),"fixed VHDX physical stage write failed");
}
void read_at(std::fstream& file,std::uint64_t where,
             std::uint8_t* dst,std::size_t len) {
    require(where<=std::uint64_t(std::numeric_limits<std::streamoff>::max()) &&
            len<=std::uint64_t(std::numeric_limits<std::streamoff>::max())-where,
            "fixed VHDX physical stage read offset overflow");
    file.clear();
    file.seekg(static_cast<std::streamoff>(where),std::ios::beg);
    require(bool(file),"fixed VHDX physical stage read seek failed");
    file.read(reinterpret_cast<char*>(dst),
              static_cast<std::streamsize>(len));
    require(bool(file),"fixed VHDX physical stage read failed");
}
void le64(std::uint8_t* out,std::uint64_t value) {
    for(unsigned i=0;i<8;++i)
        out[i]=static_cast<std::uint8_t>(value>>(8*i));
}
void le32(std::uint8_t* out,std::uint32_t value) {
    for(unsigned i=0;i<4;++i)
        out[i]=static_cast<std::uint8_t>(value>>(8*i));
}
std::uint32_t crc32c(const std::uint8_t* data,std::size_t length) noexcept {
    std::uint32_t crc=0xffffffffu;
    for(std::size_t i=0;i<length;++i) {
        crc^=data[i];
        for(unsigned n=0;n<8;++n)
            crc=(crc>>1)^((crc&1u)?0x82f63b78u:0u);
    }
    return ~crc;
}
std::array<std::uint8_t,16> random_guid() {
    std::array<std::uint8_t,16> result{};
#ifdef _WIN32
    // System-preferred Windows CSPRNG; no external utility or extra DLL.
    const NTSTATUS status=BCryptGenRandom(
        nullptr,result.data(),static_cast<ULONG>(result.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    require(BCRYPT_SUCCESS(status),"cannot create a fresh VHDX write GUID");
#else
    // Kernel entropy; no extra runtime library or process.
    const int fd=::open("/dev/urandom",O_RDONLY|O_CLOEXEC);
    require(fd>=0,"cannot obtain secure VHDX GUID entropy");
    std::size_t pos=0;
    while(pos<result.size()) {
        const auto n=::read(fd,result.data()+pos,result.size()-pos);
        if(n<0 && errno==EINTR)continue;
        if(n<=0){(void)::close(fd);throw EditUnsupported("VHDX GUID entropy read failed");}
        pos+=static_cast<std::size_t>(n);
    }
    (void)::close(fd);
#endif
    // GUID memory follows Microsoft mixed-endian wire representation.
    result[7]=std::uint8_t((result[7]&0x0fu)|0x40u);
    result[8]=std::uint8_t((result[8]&0x3fu)|0x80u);
    return result;
}
void sync_stage(const std::filesystem::path& path) {
#ifdef _WIN32
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,
                            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(file!=INVALID_HANDLE_VALUE,"cannot reopen VHDX clone to sync");
    const bool ok=FlushFileBuffers(file)!=FALSE;
    CloseHandle(file);
    require(ok,"cannot sync staged VHDX to physical storage");
#else
    const int fd=::open(path.c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    require(fd>=0,"cannot open staged VHDX for sync");
    const bool ok=::fsync(fd)==0;
    (void)::close(fd);
    require(ok,"cannot sync staged VHDX to physical storage");
#endif
}
class TemporaryLogicalRaw {
public:
    explicit TemporaryLogicalRaw(const std::filesystem::path& image) {
        std::random_device entropy;
        for(unsigned attempt=0;attempt<24;++attempt) {
            const auto nonce=(std::uint64_t(entropy())<<32)|entropy();
            auto candidate=image;
            candidate+=".iso-wcx-vhdx-"+std::to_string(nonce)+".img";
#ifdef _WIN32
            HANDLE file=CreateFileW(candidate.c_str(),GENERIC_READ|GENERIC_WRITE,
                                     0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(file==INVALID_HANDLE_VALUE) {
                const auto error=GetLastError();
                require(error==ERROR_FILE_EXISTS || error==ERROR_ALREADY_EXISTS,
                        "cannot reserve private fixed VHDX logical RAW");
                continue;
            }
            CloseHandle(file);
#else
            const int file=::open(candidate.c_str(),O_RDWR|O_CREAT|O_EXCL|
                                 O_CLOEXEC|O_NOFOLLOW,0600);
            if(file<0) {
                require(errno==EEXIST,"cannot reserve private fixed VHDX logical RAW");
                continue;
            }
            (void)::close(file);
#endif
            path_=std::move(candidate);
            return;
        }
        throw EditUnsupported("cannot securely reserve logical VHDX scratch path");
    }
    ~TemporaryLogicalRaw() noexcept {
        if(!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove(path_,ignored);
        }
    }
    TemporaryLogicalRaw(const TemporaryLogicalRaw&)=delete;
    TemporaryLogicalRaw& operator=(const TemporaryLogicalRaw&)=delete;
    const std::filesystem::path& path() const noexcept {return path_;}
private:
    std::filesystem::path path_;
};
void copy_logical(VirtualDiskReader& source,
                  const std::filesystem::path& flat,
                  const EditProgress& progress) {
    std::fstream dst(flat,std::ios::binary|std::ios::in|std::ios::out);
    require(bool(dst),"cannot open fixed VHDX logical RAW scratch");
    for(std::uint64_t at=0;at<source.size();) {
        check_progress(progress);
        const auto n=static_cast<std::size_t>(
            std::min<std::uint64_t>(window_bytes,source.size()-at));
        const auto chunk=source.read(at,n);
        write_at(dst,at,chunk.data(),chunk.size());
        at+=n;
    }
    dst.flush();
    require(bool(dst),"cannot flush fixed VHDX logical RAW");
}
void transfer_modified_sectors(VirtualDiskReader& original,
                               const VhdxFixedEditLayout& layout,
                               const std::filesystem::path& flat,
                               const std::filesystem::path& physical,
                               const EditProgress& progress) {
    std::ifstream raw(flat,std::ios::binary);
    std::fstream stage(physical,std::ios::binary|std::ios::in|std::ios::out);
    require(bool(raw) && bool(stage),"cannot open VHDX modified-sector stage");
    std::vector<std::uint8_t> buffer(window_bytes);
    std::uint64_t changed=0;
    for(std::uint64_t at=0;at<original.size();) {
        check_progress(progress);
        const auto n=static_cast<std::size_t>(
            std::min<std::uint64_t>(window_bytes,original.size()-at));
        require(n%512==0,"partial fixed VHDX logical sector");
        raw.read(reinterpret_cast<char*>(buffer.data()),
                 static_cast<std::streamsize>(n));
        require(bool(raw),"edited fixed VHDX RAW was truncated");
        const auto before=original.read(at,n);
        for(std::size_t i=0;i<n;i+=512) {
            if(!std::equal(buffer.data()+i,buffer.data()+i+512,
                           before.data()+i)) {
                const auto logical=at+i;
                const auto block=static_cast<std::size_t>(
                    logical/layout.block_bytes);
                require(block<layout.physical_blocks.size(),
                        "fixed VHDX logical write outside BAT");
                const auto physical_at=layout.physical_blocks[block]+
                                       logical%layout.block_bytes;
                write_at(stage,physical_at,buffer.data()+i,512);
                ++changed;
            }
        }
        at+=n;
    }
    require(changed>0,"fixed VHDX FAT edit produced no changed sectors");

    // Microsoft VHDX change tracking: advance both valid header copies,
    // replace the file and payload write GUIDs, recompute CRC32C.
    // Do not touch region tables, BAT, metadata, log or optional vendor data.
    std::array<std::uint8_t,4096> header{};
    read_at(stage,0x10000,header.data(),header.size());
    require(std::equal(header.begin(),header.begin()+4,"head"),
            "fixed VHDX staged header is invalid");
    le64(header.data()+8,layout.active_sequence+1);
    const auto file_guid=random_guid();
    const auto data_guid=random_guid();
    std::copy(file_guid.begin(),file_guid.end(),header.begin()+16);
    std::copy(data_guid.begin(),data_guid.end(),header.begin()+32);
    std::fill(header.begin()+4,header.begin()+8,0);
    le32(header.data()+4,crc32c(header.data(),header.size()));
    write_at(stage,0x10000,header.data(),header.size());
    write_at(stage,0x20000,header.data(),header.size());
    stage.flush();
    require(bool(stage),"cannot flush staged fixed VHDX changes");
    stage.close();
    require(!stage.fail(),"cannot close staged VHDX physical file");
    sync_stage(physical);
}
void verify_independent_readback(const std::filesystem::path& stage,
                                 const std::filesystem::path& flat,
                                 std::uint64_t expected_capacity,
                                 const EditProgress& progress) {
    VirtualDiskReader verified(stage);
    require(verified.format()==VirtualDiskFormat::vhdx &&
            verified.logical_sector_size()==512 &&
            verified.size()==expected_capacity,
            "edited fixed VHDX failed independent native reopen");
    auto* fixed=verified.vhdx_layout();
    require(fixed!=nullptr,"modified VHDX has no validated layout");
    (void)fixed->fully_mapped_fat_edit_layout(
        logical_limit(),physical_limit());
    const auto partitions=inspect_partition_table(verified,512);
    require(!partitions.empty(),"modified VHDX lost MBR/GPT partitions");
    std::ifstream raw(flat,std::ios::binary);
    require(bool(raw),"cannot open edited logical image for VHDX readback");
    std::vector<std::uint8_t> buffer(window_bytes);
    for(std::uint64_t at=0;at<expected_capacity;) {
        check_progress(progress);
        const auto n=static_cast<std::size_t>(
            std::min<std::uint64_t>(window_bytes,expected_capacity-at));
        raw.read(reinterpret_cast<char*>(buffer.data()),
                 static_cast<std::streamsize>(n));
        require(bool(raw),"edited VHDX RAW truncated before readback");
        const auto actual=verified.read(at,n);
        require(std::equal(actual.begin(),actual.end(),buffer.begin()),
                "fixed VHDX staged logical readback mismatch");
        at+=n;
    }
}
} // namespace

void edit_fixed_vhdx(const std::filesystem::path& image,
                     const std::vector<ArchiveChange>& changes,
                     const EditProgress& progress) {
    if(changes.empty())return;
    TemporaryLogicalRaw flat(image);
    std::unique_ptr<FatMappedImage> physical_stage;
    {
        VirtualDiskReader original(image);
        require(original.format()==VirtualDiskFormat::vhdx,
                "VHDX FAT edits require a validated fixed VHDX container");
        auto* reader=original.vhdx_layout();
        require(reader!=nullptr,"VHDX metadata reader unavailable");
        const auto layout=reader->fully_mapped_fat_edit_layout(
            logical_limit(),physical_limit());
        require(layout.capacity==original.size(),
                "VHDX edit layout and logical capacity disagree");
        const auto partitions=inspect_partition_table(original,512);
        require(!partitions.empty(),
                "fixed VHDX writes require an MBR/GPT FAT partition");
        // Reserve space for the logical RAW, editor clone, and physical
        // VHDX clone; source is never updated before full validation.
        const auto dir=image.parent_path().empty() ?
                       std::filesystem::path("."):image.parent_path();
        std::error_code error;
        const auto space=std::filesystem::space(dir,error);
        if(!error && space.available!=std::numeric_limits<std::uintmax_t>::max()) {
            require(space.available>=3*layout.capacity+
                    std::filesystem::file_size(image),
                    "insufficient disk space to stage fixed VHDX FAT edit");
        }
        check_progress(progress);
        copy_logical(original,flat.path(),progress);
        try {
            edit_fat(flat.path(),changes,progress);
        } catch(const EditUnsupported& e) {
            throw EditUnsupported(std::string("fixed VHDX logical FAT edit: ")+e.what());
        }
        check_progress(progress);
        physical_stage=std::make_unique<FatMappedImage>(image,true);
        physical_stage->flush_and_close();
        transfer_modified_sectors(original,layout,flat.path(),
                                  physical_stage->stage_path(),progress);
        verify_independent_readback(physical_stage->stage_path(),
                                    flat.path(),original.size(),progress);
    } // Close original and staged validation handles before Windows rename.
    check_progress(progress);
    try {
        physical_stage->replace_source();
    } catch(const EditUnsupported& e) {
        throw EditUnsupported(std::string("fixed VHDX commit: ")+e.what());
    }
}
} // namespace iso_wcx
