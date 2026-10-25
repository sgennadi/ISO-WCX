// Preview native dynamic VHD FAT12/16/32 transactional writes.
//
// Never mutate a mounted or live VHD: edits are performed against an
// independently validated logical raw clone, then only changed 512-byte
// sectors are reflected into a private physical VHD staging image.
// Allocations append [bitmap][full block][matching final footer], with
// big-endian BAT slot updates. The original stays byte-identical until the
// staged logical view matches the edited raw view and atomically replaces it.
//
// No native disk mount, helper EXE, libvhdi, Rust/Go/.NET or external DLL.
#include "iso_wcx/archive_edit.hpp"
#include "iso_wcx/dynamic_vhd_reader.hpp"
#include "iso_wcx/fat_mapped_image.hpp"
#include "iso_wcx/fat_volume.hpp"
#include "iso_wcx/partition_map.hpp"
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
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace iso_wcx {
namespace {

constexpr std::uint64_t sector = 512;
constexpr std::size_t buffer_limit = 1024u*1024u;
constexpr std::uint64_t x64_logical_limit = 1024ull*1024ull*1024ull;
constexpr std::uint64_t x86_logical_limit = 512ull*1024ull*1024ull;

void require(bool condition,const char* message) {
    if(!condition)throw EditUnsupported(message);
}

void progress_check(const EditProgress& progress) {
    if(progress && !progress())throw EditCancelled();
}

void write_at(std::fstream& stream,std::uint64_t at,
              const std::uint8_t* bytes,std::size_t count) {
    require(at<=std::uint64_t(std::numeric_limits<std::streamoff>::max()) &&
            count<=std::uint64_t(std::numeric_limits<std::streamoff>::max())-at,
            "dynamic VHD physical write address overflow");
    stream.clear();
    stream.seekp(static_cast<std::streamoff>(at),std::ios::beg);
    require(bool(stream),"cannot seek into dynamic VHD staging file");
    stream.write(reinterpret_cast<const char*>(bytes),
                 static_cast<std::streamsize>(count));
    require(bool(stream),"cannot write dynamic VHD staging file");
}

void read_at(std::fstream& stream,std::uint64_t at,
             std::uint8_t* bytes,std::size_t count) {
    require(at<=std::uint64_t(std::numeric_limits<std::streamoff>::max()) &&
            count<=std::uint64_t(std::numeric_limits<std::streamoff>::max())-at,
            "dynamic VHD physical read address overflow");
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(at),std::ios::beg);
    require(bool(stream),"cannot seek in dynamic VHD staging file");
    stream.read(reinterpret_cast<char*>(bytes),
                static_cast<std::streamsize>(count));
    require(bool(stream),"cannot read dynamic VHD staging metadata");
}

void write_be32(std::fstream& out,std::uint64_t at,std::uint32_t value) {
    const std::array<std::uint8_t,4> bytes{
        std::uint8_t(value>>24),std::uint8_t(value>>16),
        std::uint8_t(value>>8),std::uint8_t(value)};
    write_at(out,at,bytes.data(),bytes.size());
}

class TemporaryLogicalRaw {
public:
    explicit TemporaryLogicalRaw(const std::filesystem::path& image) {
        std::random_device random;
        for(unsigned tries=0;tries<24;++tries) {
            const auto entropy=(std::uint64_t(random())<<32)|random();
            auto candidate=image;
            candidate+=".iso-wcx-dynamic-"+std::to_string(entropy)+".img";
#ifdef _WIN32
            HANDLE fd=CreateFileW(candidate.c_str(),GENERIC_READ|GENERIC_WRITE,
                                  0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY,
                                  nullptr);
            if(fd==INVALID_HANDLE_VALUE) {
                const auto status=GetLastError();
                require(status==ERROR_FILE_EXISTS||status==ERROR_ALREADY_EXISTS,
                        "cannot safely reserve dynamic VHD raw staging filename");
                continue;
            }
            CloseHandle(fd);
#else
            const int fd=::open(candidate.c_str(),
                               O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
            if(fd<0) {
                require(errno==EEXIST,
                        "cannot safely reserve dynamic VHD raw staging filename");
                continue;
            }
            (void)::close(fd);
#endif
            name_=std::move(candidate);
            return;
        }
        throw EditUnsupported("cannot reserve temporary logical VHD image");
    }
    ~TemporaryLogicalRaw() noexcept {
        if(!name_.empty()) {
            std::error_code ignored;
            std::filesystem::remove(name_,ignored);
        }
    }
    TemporaryLogicalRaw(const TemporaryLogicalRaw&)=delete;
    TemporaryLogicalRaw& operator=(const TemporaryLogicalRaw&)=delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return name_;
    }
private:
    std::filesystem::path name_;
};

std::array<std::uint8_t,512> read_terminal_footer(
        const std::filesystem::path& source) {
    std::ifstream file(source,std::ios::binary|std::ios::ate);
    require(bool(file),"cannot open original dynamic VHD footer");
    const auto end=file.tellg();
    require(end>=static_cast<std::streamoff>(512),
            "dynamic VHD file is too small for final footer");
    file.seekg(end-static_cast<std::streamoff>(512),std::ios::beg);
    std::array<std::uint8_t,512> footer{};
    file.read(reinterpret_cast<char*>(footer.data()),512);
    require(bool(file),"cannot read original dynamic VHD footer");
    return footer;
}

std::uint64_t next_logical_limit() noexcept {
    return sizeof(void*)==4 ? x86_logical_limit : x64_logical_limit;
}

std::uint32_t allocate_physical_block(
        std::fstream& staged,
        std::uint64_t& stage_size,
        std::uint32_t bitmap_size,
        std::uint32_t block_size,
        const std::array<std::uint8_t,512>& footer,
        std::uint64_t bat_offset,
        std::size_t block_index) {
    require(stage_size>=512 && stage_size%512==0,
            "invalid dynamic VHD staging physical length");
    const auto insert_at=stage_size-512;
    // 0xffffffff is reserved as the BAT "not present" sentinel.
    require(insert_at/512 < UINT32_MAX,
            "dynamic VHD physical address exceeds 32-bit BAT range");
    std::array<std::uint8_t,buffer_limit> zeros{};
    const auto extent=std::uint64_t(bitmap_size)+block_size;
    require(extent>0 &&
            stage_size<=std::uint64_t(std::numeric_limits<std::streamoff>::max())-extent,
            "dynamic VHD stage exceeds supported physical file limit");
    std::uint64_t done=0;
    while(done<extent) {
        const auto n=static_cast<std::size_t>(
            std::min<std::uint64_t>(zeros.size(),extent-done));
        write_at(staged,insert_at+done,zeros.data(),n);
        done+=n;
    }
    write_at(staged,insert_at+extent,footer.data(),footer.size());
    stage_size+=extent;
    const auto pointer=static_cast<std::uint32_t>(insert_at/512);
    write_be32(staged,bat_offset+std::uint64_t(block_index)*4,pointer);
    return pointer;
}

void assign_logical_sector(
        std::fstream& stage,
        std::uint64_t& stage_size,
        std::vector<std::uint32_t>& bat,
        const DynamicVhdReader& layout,
        const std::array<std::uint8_t,512>& footer,
        std::uint64_t offset,
        const std::uint8_t* bytes) {
    require(offset%512==0,"unaligned dynamic VHD logical write");
    const auto block=static_cast<std::size_t>(offset/layout.block_bytes());
    require(block<bat.size(),"VHD logical write indexes beyond BAT");
    if(bat[block]==UINT32_MAX) {
        bat[block]=allocate_physical_block(stage,stage_size,
                   layout.bitmap_bytes(),layout.block_bytes(),
                   footer,layout.bat_offset(),block);
    }
    const auto block_start=std::uint64_t(bat[block])*512;
    const auto inside=offset%layout.block_bytes();
    const auto sector_index=static_cast<std::size_t>(inside/512);
    const auto bitmap_byte=block_start+sector_index/8;
    std::uint8_t existing=0;
    read_at(stage,bitmap_byte,&existing,1);
    const auto changed_bitmap=std::uint8_t(
        existing | (0x80u>>(sector_index%8)));
    if(changed_bitmap!=existing)
        write_at(stage,bitmap_byte,&changed_bitmap,1);
    write_at(stage,block_start+layout.bitmap_bytes()+inside,bytes,512);
}

void copy_logical_view(VirtualDiskReader& disk,
                       const std::filesystem::path& dest,
                       const EditProgress& progress) {
    std::fstream raw(dest,std::ios::binary|std::ios::in|std::ios::out);
    require(bool(raw),"cannot open private logical FAT disk");
    std::uint64_t offset=0;
    while(offset<disk.size()) {
        progress_check(progress);
        const auto amount=static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer_limit,disk.size()-offset));
        const auto data=disk.read(offset,amount);
        write_at(raw,offset,data.data(),data.size());
        offset+=amount;
    }
    raw.flush();
    require(bool(raw),"cannot flush extracted dynamic VHD logical image");
}

void stage_changed_sectors(
        VirtualDiskReader& original,const DynamicVhdReader& layout,
        const std::filesystem::path& flat,
        const std::filesystem::path& physical_stage,
        const std::array<std::uint8_t,512>& footer,
        const EditProgress& progress) {
    std::ifstream raw(flat,std::ios::binary);
    require(bool(raw),"cannot read edited logical FAT disk");
    std::fstream stage(physical_stage,
                      std::ios::binary|std::ios::in|std::ios::out);
    require(bool(stage),"cannot open cloned dynamic VHD for sector patching");
    auto bat=layout.bat_entries();
    std::uint64_t stage_size=std::filesystem::file_size(physical_stage);
    std::vector<std::uint8_t> buffer(buffer_limit);
    std::uint64_t changes=0;
    std::uint64_t logical=0;
    while(logical<original.size()) {
        progress_check(progress);
        const auto length=static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer_limit,original.size()-logical));
        raw.read(reinterpret_cast<char*>(buffer.data()),
                 static_cast<std::streamsize>(length));
        require(bool(raw),"edited logical VHD disk is unexpectedly truncated");
        const auto before=original.read(logical,length);
        for(std::size_t at=0;at<length;at+=512) {
            require(at+512<=length,"VHD logical disk has partial sector");
            if(!std::equal(buffer.data()+at,buffer.data()+at+512,
                           before.data()+at)) {
                assign_logical_sector(stage,stage_size,bat,layout,footer,
                                      logical+at,buffer.data()+at);
                ++changes;
            }
        }
        logical+=length;
    }
    require(changes>0,"dynamic FAT edit produced no changed sectors");
    stage.flush();
    require(bool(stage),"cannot flush changed dynamic VHD physical sectors");
}

void check_staged_logical_match(
        const std::filesystem::path& staged_path,
        const std::filesystem::path& flat,
        std::uint64_t expected_size,
        const EditProgress& progress) {
    VirtualDiskReader modified(staged_path);
    require(modified.format()==VirtualDiskFormat::dynamic_vhd &&
            modified.size()==expected_size,
            "staged dynamic VHD is not a readable base image");
    std::ifstream raw(flat,std::ios::binary);
    require(bool(raw),"cannot independently verify edited raw FAT image");
    std::vector<std::uint8_t> buffer(buffer_limit);
    for(std::uint64_t logical=0;logical<expected_size;) {
        progress_check(progress);
        const auto n=static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer_limit,expected_size-logical));
        raw.read(reinterpret_cast<char*>(buffer.data()),
                 static_cast<std::streamsize>(n));
        require(bool(raw),"edited logical image failed length check");
        const auto actual=modified.read(logical,n);
        require(std::equal(actual.begin(),actual.end(),buffer.begin()),
                "dynamic VHD BAT/bitmap staged logical readback mismatch");
        logical+=n;
    }
    const auto partitions=inspect_partition_table(modified,512);
    require(!partitions.empty(),
            "staged dynamic VHD lost its original partition mapping");
}

} // namespace

void edit_dynamic_vhd(const std::filesystem::path& image,
                      const std::vector<ArchiveChange>& changes,
                      const EditProgress& progress) {
    if(changes.empty())return;
    TemporaryLogicalRaw flat(image);
    std::unique_ptr<FatMappedImage> staged;
    {
        VirtualDiskReader original(image);
        require(original.format()==VirtualDiskFormat::dynamic_vhd,
                "dynamic VHD edit requires validated base-image BAT");
        const auto* layout=original.dynamic_vhd_layout();
        require(layout && original.size()<=next_logical_limit(),
                "dynamic VHD write Preview exceeds capped logical capacity");
        // Explicit partitionN targeting and FAT support are validated again
        // by the ordinary RAW/IMG transactional editor below.
        const auto partitions=inspect_partition_table(original,512);
        require(!partitions.empty(),
                "dynamic VHD write requires a valid MBR/GPT FAT partition");
        progress_check(progress);
        // Ensure a bounded scratch-space budget before any disk mutation.
        const auto parent=image.parent_path().empty() ?
                          std::filesystem::path(".") : image.parent_path();
        std::error_code ec;
        const auto available=std::filesystem::space(parent,ec);
        if(!ec && available.available!=UINTMAX_MAX) {
            require(available.available >=
                    std::uint64_t(original.size())*3+
                    std::filesystem::file_size(image),
                    "insufficient free disk for atomic dynamic VHD FAT edit");
        }
        copy_logical_view(original,flat.path(),progress);
        // This is the same independently verified FAT12/16/32 writer used
        // for normal RAW, IMG and fixed VHD. It edits its own staged clone.
        edit_fat(flat.path(),changes,progress);
        progress_check(progress);

        staged=std::make_unique<FatMappedImage>(image,true);
        staged->flush_and_close();
        const auto footer=read_terminal_footer(image);
        stage_changed_sectors(original,*layout,flat.path(),
                              staged->stage_path(),footer,progress);
        check_staged_logical_match(staged->stage_path(),flat.path(),
                                  original.size(),progress);
    } // Original image and validation readers are released before replacement.
    progress_check(progress);
    staged->replace_source(); // Only here may the original pathname change.
}
} // namespace iso_wcx
