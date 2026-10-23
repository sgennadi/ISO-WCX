#include "iso_wcx/virtual_disk.hpp"

#include <algorithm>
#include <array>
#include <cwctype>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#if defined(ISO_WCX_WITH_LIBVHDI)
#include <libvhdi.h>
#endif

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

#if defined(ISO_WCX_WITH_LIBVHDI)
// A local guard ensures that libvhdi handles cannot escape if any
// validation step fails inside the constructor. No write API is called.
void vhdi_close(libvhdi_file_t*& handle) noexcept {
    if(!handle)return;
    libvhdi_error_t* error=nullptr;
    (void)libvhdi_file_close(handle,&error);
    if(error)libvhdi_error_free(&error);
    (void)libvhdi_file_free(&handle,&error);
    if(error)libvhdi_error_free(&error);
}
struct VhdiGuard {
    libvhdi_file_t* handle=nullptr;
    ~VhdiGuard() noexcept {vhdi_close(handle);}
};
#endif

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

    const auto suffix=lowercase(image.extension().wstring());
    if(suffix==L".vhdx") {
#if defined(ISO_WCX_WITH_LIBVHDI)
        std::array<char,8> signature{};
        input_.seekg(0);
        require(bool(input_.read(signature.data(),signature.size())) &&
                std::string_view(signature.data(),signature.size())=="vhdxfile",
                "invalid VHDX file identifier");
        open_optional_backend(image,VirtualDiskFormat::vhdx);
        return;
#else
        throw std::runtime_error(
            "VHDX requires experimental ISO_WCX_WITH_LIBVHDI=ON (not shipped in WCX)");
#endif
    }
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
#if defined(ISO_WCX_WITH_LIBVHDI)
            open_optional_backend(image,VirtualDiskFormat::dynamic_vhd);
            require(capacity_==info.logical_size,
                    "libvhdi VHD capacity disagrees with validated footer");
#else
            throw std::runtime_error(
                "dynamic VHD requires experimental ISO_WCX_WITH_LIBVHDI=ON");
#endif
        }else {
            throw std::runtime_error(
                "differencing VHD parent chains are deliberately unsupported");
        }
    }
    require(capacity_>=512,"virtual disk has no sector data");
}

VirtualDiskReader::~VirtualDiskReader() noexcept {
#if defined(ISO_WCX_WITH_LIBVHDI)
    auto* f=static_cast<libvhdi_file_t*>(library_backend_);
    vhdi_close(f);
    library_backend_=nullptr;
#else
    (void)library_backend_;
#endif
}

void VirtualDiskReader::open_optional_backend(const std::filesystem::path& image,
                                              VirtualDiskFormat expected) {
#if defined(ISO_WCX_WITH_LIBVHDI)
    VhdiGuard state;
    libvhdi_error_t* error=nullptr;
    const auto check_result=[&](int result,const char* message) {
        if(result!=1) {
            if(error)libvhdi_error_free(&error);
            throw std::runtime_error(message);
        }
        if(error)libvhdi_error_free(&error);
    };
    check_result(libvhdi_file_initialize(&state.handle,&error),
                 "libvhdi initialization failed");
    const std::string filename=image.string();
    check_result(libvhdi_file_open(state.handle,filename.c_str(),
                                  LIBVHDI_OPEN_READ,&error),
                 "libvhdi cannot open virtual disk read-only");
    int file_type=0;
    check_result(libvhdi_file_get_file_type(state.handle,&file_type,&error),
                 "libvhdi cannot identify virtual disk format");
    if(expected==VirtualDiskFormat::dynamic_vhd)
        require(file_type==LIBVHDI_FILE_TYPE_VHD,"unexpected libvhdi file type for VHD");
    else if(expected==VirtualDiskFormat::vhdx)
        require(file_type==LIBVHDI_FILE_TYPE_VHDX,"unexpected libvhdi file type for VHDX");
    else
        throw std::runtime_error("unexpected libvhdi backend request");

    std::uint32_t disk_type=0;
    check_result(libvhdi_file_get_disk_type(state.handle,&disk_type,&error),
                 "libvhdi cannot determine disk allocation type");
    require(disk_type==LIBVHDI_DISK_TYPE_FIXED ||
            disk_type==LIBVHDI_DISK_TYPE_DYNAMIC,
            "differencing or unsupported libvhdi parent disk type");
    if(expected==VirtualDiskFormat::dynamic_vhd)
        require(disk_type==LIBVHDI_DISK_TYPE_DYNAMIC,
                "expected dynamic VHD, got a different layout");

    size64_t size=0;
    check_result(libvhdi_file_get_media_size(state.handle,&size,&error),
                 "libvhdi cannot determine logical disk size");
    require(size>=512 && size%512==0 &&
            size<=static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()),
            "libvhdi logical media size is invalid");
    capacity_=static_cast<std::uint64_t>(size);
    format_=expected;
    library_backend_=state.handle;
    state.handle=nullptr;
    input_.close();
#else
    (void)image;
    (void)expected;
    throw std::runtime_error("libvhdi backend was not enabled during compilation");
#endif
}

std::vector<std::uint8_t> VirtualDiskReader::read(
        std::uint64_t offset,std::size_t length) {
    require(length<=4u*1024u*1024u,"virtual disk metadata read exceeds 4 MiB");
    require(offset<=capacity_ && length<=capacity_-offset,
            "read exceeds virtual disk capacity");
    require(offset<=static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()),
            "virtual disk offset exceeds supported seek range");
    std::vector<std::uint8_t> output(length);
    if(length==0)return output;
#if defined(ISO_WCX_WITH_LIBVHDI)
    if(library_backend_) {
        libvhdi_error_t* error=nullptr;
        const auto n=libvhdi_file_read_buffer_at_offset(
            static_cast<libvhdi_file_t*>(library_backend_), output.data(),
            output.size(),static_cast<off64_t>(offset),&error);
        if(error)libvhdi_error_free(&error);
        require(n>=0 && static_cast<std::uint64_t>(n)==length,
                "libvhdi returned an incomplete logical disk read");
        return output;
    }
#endif
    input_.clear();
    input_.seekg(static_cast<std::streamoff>(offset),std::ios::beg);
    require(bool(input_),"cannot seek in virtual disk");
    input_.read(reinterpret_cast<char*>(output.data()),
                static_cast<std::streamsize>(length));
    require(bool(input_),"truncated virtual disk data");
    return output;
}

} // namespace iso_wcx
