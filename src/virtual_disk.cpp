#include "iso_wcx/virtual_disk.hpp"
#include "iso_wcx/vhdx_reader.hpp"
#include "iso_wcx/imz_reader.hpp"

#include <algorithm>
#include <array>
#include <cwctype>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>


namespace iso_wcx {
namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
std::uint32_t be32(const std::uint8_t* p) noexcept {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}
std::uint64_t be64(const std::uint8_t* p) noexcept {
    return (std::uint64_t(be32(p)) << 32) | be32(p+4);
}
std::wstring lowercase(std::wstring value) {
    for (auto& c : value) c = static_cast<wchar_t>(std::towlower(c));
    return value;
}
struct VhdFooter {
    std::uint32_t disk_type{};
    std::uint64_t logical_size{};
};

// VHD v1: footer is 512 bytes at EOF. Checksum is one's complement of the
// sum of every footer byte except its stored four-byte checksum field.
VhdFooter parse_vhd_footer(const std::array<std::uint8_t,512>& footer,
                           std::uint64_t physical_length) {
    const auto* bytes=footer.data();
    require(std::string_view(reinterpret_cast<const char*>(bytes),8)=="conectix",
            "invalid VHD footer signature");
    std::uint32_t sum=0;
    for(std::size_t i=0;i<footer.size();++i)
        if(i<64 || i>=68)sum+=footer[i];
    require(~sum==be32(bytes+64),"VHD footer checksum mismatch");
    const auto features=be32(bytes+8);
    require((features&2u)!=0 && (features&~3u)==0,
            "unsupported VHD footer features");
    require(be32(bytes+12)==0x00010000u,"unsupported VHD footer version");
    require(bytes[84]==0,"saved-state VHD is not supported");
    const auto original=be64(bytes+40);
    const auto current=be64(bytes+48);
    require(original>=512 && original%512==0 &&
            current>=512 && current%512==0 &&
            current<=static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()),
            "invalid VHD logical capacity");
    const auto disk_type=be32(bytes+60);
    const auto metadata=be64(bytes+16);
    if(disk_type==2u) {
        require(metadata==UINT64_MAX,"invalid fixed VHD data-offset marker");
        require(physical_length>=512 && current==physical_length-512,
                "fixed VHD capacity differs from the data section");
    }else if(disk_type==3u || disk_type==4u) {
        require(physical_length>=2048 && metadata>=512 &&
                metadata%512==0 && metadata<=physical_length-512-1024,
                "dynamic VHD metadata outside the image");
    }else {
        throw std::runtime_error("unsupported VHD disk type");
    }
    return {disk_type,current};
}

} // namespace

VirtualDiskReader::VirtualDiskReader(const std::filesystem::path& image) {
    require(std::filesystem::is_regular_file(image) &&
            !std::filesystem::is_symlink(image),
            "virtual disk must be a regular non-symlink file");
    input_.open(image,std::ios::binary | std::ios::ate);
    require(input_.is_open(),"cannot open virtual disk");
    const auto physical_position=input_.tellg();
    require(physical_position>=0,"cannot size virtual disk");
    const auto physical_length=static_cast<std::uint64_t>(physical_position);
    require(physical_length>=512,"virtual disk image is too small");
    capacity_=physical_length;
    // Do not mistake a VHDX renamed to .img for a raw disk.
    std::array<char,8> identifier{};
    input_.seekg(0);
    input_.read(identifier.data(),static_cast<std::streamsize>(identifier.size()));
    require(bool(input_),"cannot read virtual disk signature");
    const bool is_vhdx=std::string_view(identifier.data(),identifier.size())=="vhdxfile";
    const auto suffix=lowercase(image.extension().wstring());
    if(suffix==L".imz") {
        imz_bytes_=decode_imz(input_,physical_length);
        capacity_=imz_bytes_.size();
        format_=VirtualDiskFormat::compressed_floppy;
        input_.close();
        return;
    }
    if(suffix==L".vhdx") {
        require(is_vhdx,".vhdx extension has invalid VHDX file identifier");
        vhdx_=std::make_unique<VhdxReader>(image);
        capacity_=vhdx_->size();
        format_=VirtualDiskFormat::vhdx;
        input_.close();
        return;
    }
    require(!is_vhdx,"VHDX renamed to another extension is not a raw image");
    if(suffix==L".vhd") {
        require(physical_length>=1024,"VHD image has no full footer and disk data");
        input_.seekg(static_cast<std::streamoff>(physical_length-512));
        require(bool(input_),"cannot seek to VHD footer");
        std::array<std::uint8_t,512> footer{};
        input_.read(reinterpret_cast<char*>(footer.data()),
                    static_cast<std::streamsize>(footer.size()));
        require(bool(input_),"truncated VHD footer");
        const auto info=parse_vhd_footer(footer,physical_length);
        if(info.disk_type==2u) {
            capacity_=info.logical_size;
            format_=VirtualDiskFormat::fixed_vhd;
        }else if(info.disk_type==3u) {
            throw std::runtime_error("dynamic VHD requires a future native BAT reader");
        }else {
            throw std::runtime_error(
                "differencing VHD parent chains are deliberately unsupported");
        }
    }
    require(capacity_>=512,"virtual disk has no sector data");
}

VirtualDiskReader::~VirtualDiskReader() noexcept = default;

std::uint32_t VirtualDiskReader::logical_sector_size() const noexcept {
    return vhdx_ ? vhdx_->logical_sector_size() : 512u;
}

std::vector<std::uint8_t> VirtualDiskReader::read(
        std::uint64_t offset,std::size_t length) {
    require(length<=4u*1024u*1024u,"virtual disk metadata read exceeds 4 MiB");
    require(offset<=capacity_ && length<=capacity_-offset,
            "read exceeds virtual disk capacity");
    require(offset<=static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()),
            "virtual disk offset exceeds supported seek range");
    if(vhdx_)return vhdx_->read(offset,length);
    std::vector<std::uint8_t> output(length);
    if(length==0)return output;
    if(format_==VirtualDiskFormat::compressed_floppy) {
        std::copy_n(imz_bytes_.data()+static_cast<std::size_t>(offset),length,
                    output.data());
        return output;
    }
    input_.clear();
    input_.seekg(static_cast<std::streamoff>(offset),std::ios::beg);
    require(bool(input_),"cannot seek in virtual disk");
    input_.read(reinterpret_cast<char*>(output.data()),
                static_cast<std::streamsize>(length));
    require(bool(input_),"truncated virtual disk data");
    return output;
}

} // namespace iso_wcx
