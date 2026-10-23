#include "iso_wcx/mds_probe.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace iso_wcx {
namespace {
void require(bool ok, const char* msg) {
    if(!ok)throw std::runtime_error(msg);
}
std::uint32_t le32(const std::uint8_t* b) noexcept {
    return std::uint32_t(b[0]) | (std::uint32_t(b[1]) << 8) |
           (std::uint32_t(b[2]) << 16) | (std::uint32_t(b[3]) << 24);
}
std::uint64_t le64(const std::uint8_t* b) noexcept {
    return std::uint64_t(le32(b)) | (std::uint64_t(le32(b+4)) << 32);
}
std::string lower(std::string value) {
    std::transform(value.begin(),value.end(),value.begin(),
        [](unsigned char c){return static_cast<char>(std::tolower(c));});
    return value;
}
constexpr std::uint64_t encrypted_header_bytes = 0x200u;
constexpr std::uint64_t maximum_mds_descriptor_bytes = 16u * 1024u * 1024u;
} // namespace

MdsDescriptorProbe probe_mds_descriptor(const std::filesystem::path& image) {
    require(std::filesystem::is_regular_file(image) &&
            !std::filesystem::is_symlink(image),
            "MDS/MDX probe needs a regular non-symlink image file");
    const auto suffix=lower(image.extension().string());
    require(suffix==".mds" || suffix==".mdx",
            "MDS/MDX probe only accepts .mds or .mdx");
    std::ifstream source(image,std::ios::binary|std::ios::ate);
    require(bool(source),"cannot open MDS/MDX image");
    const auto count=source.tellg();
    require(count>=0,"cannot determine MDS/MDX file size");
    const auto total=static_cast<std::uint64_t>(count);
    require(total>=88,"truncated MDS/MDX descriptor");
    require(total<=static_cast<std::uint64_t>(
                  std::numeric_limits<std::streamoff>::max()),
            "MDS/MDX image exceeds supported seek range");
    if(suffix==".mds")
        require(total<=maximum_mds_descriptor_bytes,
                "MDS descriptor exceeds 16 MiB metadata safety limit");

    std::array<std::uint8_t,96> header{};
    source.seekg(0);
    const auto n=static_cast<std::streamsize>(
        std::min<std::uint64_t>(header.size(),total));
    source.read(reinterpret_cast<char*>(header.data()),n);
    require(bool(source),"cannot read MDS/MDX descriptor");
    require(std::string_view(
                reinterpret_cast<const char*>(header.data()),16) ==
            "MEDIA DESCRIPTOR","invalid MDS/MDX signature");

    MdsDescriptorProbe result{};
    result.major=header[16];
    result.minor=header[17];
    result.file_bytes=total;
    if(result.major==1) {
        require(suffix==".mds","MDS major version 1 is not an MDX image");
        result.kind=MdsDescriptorKind::alcohol_v1;
        return result;
    }
    require(result.major==2,
            "unsupported MDS major version (expected 1 or 2)");
    require(total>=0x230u,"MDSv2/MDX truncated encrypted metadata");

    // Daemon Tools MDSv2 uses a packed/encrypted metadata stream beginning
    // at offset 0x30 with its encrypted 512-byte header at the offset
    // indicated by 0x2c. Embedded MDX uses a 0xffffffff sentinel, then
    // 64-bit location/size. These offsets come from the format study of
    // Marisa-Chan/mdsx and do NOT imply that encrypted content is readable.
    const auto marker=le32(header.data()+0x2c);
    if(marker!=UINT32_MAX) {
        require(suffix==".mds",
                "MDX missing its embedded-data marker");
        const auto header_at=std::uint64_t(marker);
        require(header_at>=0x30 &&
                header_at<=total &&
                encrypted_header_bytes<=total-header_at,
                "MDSv2 encrypted header lies outside descriptor");
        result.kind=MdsDescriptorKind::daemon_mds_v2;
        result.packed_metadata_offset=0x30;
        result.packed_metadata_length=header_at-0x30;
        result.encrypted_header_offset=header_at;
        require(result.packed_metadata_length>0,
                "MDSv2 packed metadata is empty");
        return result;
    }
    require(suffix==".mdx","MDX-layout metadata must have .mdx extension");
    const auto data_at=le64(header.data()+0x30);
    const auto extent=le64(header.data()+0x38);
    require(data_at>=0x60 && data_at<=total &&
            extent>=0x40 && extent<=total-data_at,
            "MDX embedded data extent lies outside the image");
    const auto header_at=data_at+extent-0x40;
    require(header_at<=total &&
            encrypted_header_bytes<=total-header_at,
            "MDX encrypted header lies outside image");
    result.kind=MdsDescriptorKind::daemon_mdx;
    result.packed_metadata_offset=data_at;
    result.packed_metadata_length=extent-0x40;
    result.encrypted_header_offset=header_at;
    require(result.packed_metadata_length>0,
            "MDX packed metadata is empty");
    return result;
}

} // namespace iso_wcx
