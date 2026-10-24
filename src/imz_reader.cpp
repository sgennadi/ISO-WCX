// Strict read-only WinImage IMZ decoding (ZIP store/deflate).
// The linked zlib is static on Windows: no external processes or DLLs.
#include "iso_wcx/imz_reader.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <zlib.h>

namespace iso_wcx {
namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
std::uint16_t le16(const std::vector<std::uint8_t>& b, std::size_t at) {
    require(at <= b.size() && b.size()-at >= 2, "truncated IMZ ZIP metadata");
    return std::uint16_t(b[at]) | (std::uint16_t(b[at+1]) << 8);
}
std::uint32_t le32(const std::vector<std::uint8_t>& b, std::size_t at) {
    return std::uint32_t(le16(b,at)) | (std::uint32_t(le16(b,at+2)) << 16);
}
std::vector<std::uint8_t> inflate_raw(const std::uint8_t* src,
                                       std::size_t in_size,
                                       std::size_t out_size) {
    require(in_size <= std::numeric_limits<uInt>::max() &&
            out_size <= std::numeric_limits<uInt>::max(),
            "IMZ inflate exceeds zlib bounds");
    std::vector<std::uint8_t> out(out_size);
    z_stream stream{};
    stream.next_in = const_cast<Bytef*>(src);
    stream.avail_in = static_cast<uInt>(in_size);
    stream.next_out = out.data();
    stream.avail_out = static_cast<uInt>(out.size());
    require(inflateInit2(&stream,-MAX_WBITS) == Z_OK,
            "cannot initialize IMZ raw DEFLATE");
    const int result=inflate(&stream,Z_FINISH);
    const bool valid=result==Z_STREAM_END &&
                     stream.total_in==in_size && stream.total_out==out_size;
    inflateEnd(&stream);
    require(valid,"IMZ compressed payload is truncated or malformed");
    return out;
}
}
std::vector<std::uint8_t> decode_imz(std::ifstream& input, std::uint64_t length) {
    constexpr std::size_t max_zip=64u*1024u*1024u;
    constexpr std::size_t max_disk=32u*1024u*1024u;
    require(length >= 22 && length <= max_zip,"IMZ ZIP wrapper size is unsupported");
    const auto zip_size=static_cast<std::size_t>(length);
    std::vector<std::uint8_t> zip(zip_size);
    input.clear();
    input.seekg(0,std::ios::beg);
    require(bool(input.read(reinterpret_cast<char*>(zip.data()),
                            static_cast<std::streamsize>(zip.size()))),
            "cannot read IMZ ZIP wrapper");
    std::size_t eocd=zip.size();
    const auto minimum=zip.size() > (22u+65535u) ? zip.size()-(22u+65535u) : 0u;
    for (std::size_t at=zip.size()-22; ; --at) {
        if (le32(zip,at)==0x06054b50 && at+22+le16(zip,at+20)==zip.size()) {
            eocd=at; break;
        }
        if (at==minimum) break;
    }
    require(eocd!=zip.size(),"IMZ ZIP end-of-central-directory not found");
    require(le16(zip,eocd+4)==0 && le16(zip,eocd+6)==0 &&
            le16(zip,eocd+8)==1 && le16(zip,eocd+10)==1,
            "IMZ requires one member in a single-disk ZIP archive");
    const auto central_size=static_cast<std::size_t>(le32(zip,eocd+12));
    const auto central_at=static_cast<std::size_t>(le32(zip,eocd+16));
    require(central_at<=eocd && central_size<=eocd-central_at &&
            central_size>=46,"invalid IMZ central directory range");
    const auto cd=central_at;
    require(le32(zip,cd)==0x02014b50,"invalid IMZ central file header");
    const auto flags=le16(zip,cd+8);
    const auto compression=le16(zip,cd+10);
    const auto expected_crc=le32(zip,cd+16);
    const auto compressed=static_cast<std::size_t>(le32(zip,cd+20));
    const auto unpacked=static_cast<std::size_t>(le32(zip,cd+24));
    const auto filename=static_cast<std::size_t>(le16(zip,cd+28));
    const auto extra=static_cast<std::size_t>(le16(zip,cd+30));
    const auto comment=static_cast<std::size_t>(le16(zip,cd+32));
    const auto local_at=static_cast<std::size_t>(le32(zip,cd+42));
    require(compression==0 || compression==8,"unsupported IMZ ZIP compression");
    require((flags & static_cast<std::uint16_t>(~0x080eu))==0 &&
            (flags & 0x0001u)==0,
            "encrypted or unsupported IMZ ZIP flags");
    require(le16(zip,cd+34)==0,"IMZ ZIP multi-disk member is unsupported");
    require(filename>0 && filename<=1024 && extra<=65535 && comment<=65535 &&
            46+filename+extra+comment==central_size,
            "IMZ ZIP must contain exactly one valid file entry");
    require(unpacked>=512 && unpacked<=max_disk &&
            unpacked%512==0 && compressed<=max_zip,
            "IMZ decompressed image has invalid floppy capacity");
    const auto name_at=cd+46;
    require(name_at<=eocd && filename<=eocd-name_at,
            "truncated IMZ ZIP filename");
    for (std::size_t i=0;i<filename;++i)
        require(zip[name_at+i]!=0 && zip[name_at+i]!='/' &&
                zip[name_at+i]!='\\',
                "IMZ ZIP member must be a single regular image file");
    require(zip[name_at+filename-1]!='/' &&
            (le32(zip,cd+38) & 0x10u)==0,
            "IMZ ZIP member is a directory");
    require(local_at<cd && cd-local_at>=30 &&
            le32(zip,local_at)==0x04034b50,
            "invalid IMZ local ZIP entry");
    const auto local_flags=le16(zip,local_at+6);
    require(local_flags==flags && le16(zip,local_at+8)==compression,
            "IMZ ZIP headers disagree on flags or method");
    const auto local_name=static_cast<std::size_t>(le16(zip,local_at+26));
    const auto local_extra=static_cast<std::size_t>(le16(zip,local_at+28));
    const auto payload=local_at+30+local_name+local_extra;
    require(local_name==filename && payload<=cd && compressed<=cd-payload &&
            local_at+30+local_name<=cd,
            "IMZ ZIP member points outside compressed data");
    require(std::equal(zip.begin()+static_cast<std::ptrdiff_t>(name_at),
                       zip.begin()+static_cast<std::ptrdiff_t>(name_at+filename),
                       zip.begin()+static_cast<std::ptrdiff_t>(local_at+30)),
            "IMZ ZIP local and central names disagree");
    if ((flags & 0x0008u)==0) {
        require(le32(zip,local_at+14)==expected_crc &&
                le32(zip,local_at+18)==compressed &&
                le32(zip,local_at+22)==unpacked,
                "IMZ ZIP local sizes and checksum disagree");
    }
    std::vector<std::uint8_t> disk;
    if (compression==0) {
        require(compressed==unpacked,"invalid uncompressed IMZ length");
        disk.assign(zip.begin()+static_cast<std::ptrdiff_t>(payload),
                    zip.begin()+static_cast<std::ptrdiff_t>(payload+compressed));
    } else {
        disk=inflate_raw(zip.data()+payload,compressed,unpacked);
    }
    const auto actual=crc32(0,disk.data(),static_cast<uInt>(disk.size()));
    require(actual==expected_crc,"IMZ ZIP CRC32 mismatch");
    return disk;
}
} // namespace iso_wcx
