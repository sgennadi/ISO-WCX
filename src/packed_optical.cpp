#include "iso_wcx/packed_optical.hpp"

#include <zlib.h>
#include <bzlib.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace iso_wcx {
namespace {
void check(bool condition, const char* why) {
    if (!condition) throw std::runtime_error(why);
}
std::uint16_t le16(const std::uint8_t* p) {
    return std::uint16_t(p[0]) | std::uint16_t(std::uint16_t(p[1]) << 8);
}
std::uint32_t le24(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16);
}
std::uint32_t le32(const std::uint8_t* p) {
    return le16(p) | (std::uint32_t(le16(p + 2)) << 16);
}
std::uint64_t le64(const std::uint8_t* p) {
    return le32(p) | (std::uint64_t(le32(p + 4)) << 32);
}
std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}
std::uint64_t be64(const std::uint8_t* p) {
    return (std::uint64_t(be32(p)) << 32) | be32(p + 4);
}
constexpr std::uint64_t max_logical = std::uint64_t(1) << 44;
constexpr std::size_t max_chunks = 4'000'000;
constexpr std::uint32_t max_block = 1024u * 1024u;

std::string lowercase_extension(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return extension;
}
void inflate_exact(const std::vector<std::uint8_t>& encoded,
                   std::vector<std::uint8_t>& decoded,
                   std::size_t expected, int format) {
    check(encoded.size() <= std::numeric_limits<uInt>::max() &&
          decoded.size() <= std::numeric_limits<uInt>::max(),
          "packed ISO zlib buffer exceeds bounds");
    z_stream state{};
    check(inflateInit2(&state, format) == Z_OK,
          "cannot initialize packed ISO DEFLATE");
    state.next_in = const_cast<Bytef*>(
        reinterpret_cast<const Bytef*>(encoded.data()));
    state.avail_in = static_cast<uInt>(encoded.size());
    state.next_out = reinterpret_cast<Bytef*>(decoded.data());
    state.avail_out = static_cast<uInt>(decoded.size());
    const int status = inflate(&state, Z_FINISH);
    const auto output_bytes = state.total_out;
    const auto remaining_bytes = state.avail_in;
    inflateEnd(&state);
    check(status == Z_STREAM_END &&
          (output_bytes == expected || output_bytes == decoded.size()) &&
          remaining_bytes == 0, "invalid packed ISO compressed block");
}
} // namespace

bool is_packed_optical_path(const std::filesystem::path& path) {
    const auto extension = lowercase_extension(path);
    return extension == ".isz" || extension == ".daa" || extension == ".chd";
}

PackedOptical::PackedOptical(const std::filesystem::path& path)
    : file_(path, std::ios::binary | std::ios::ate) {
    check(std::filesystem::is_regular_file(path) &&
          !std::filesystem::is_symlink(path),
          "packed ISO must be a regular non-symlink file");
    check(bool(file_), "cannot open packed optical image");
    const auto file_end = file_.tellg();
    check(file_end >= 0, "cannot stat packed optical image");
    physical_size_ = static_cast<std::uint64_t>(file_end);
    const auto extension = lowercase_extension(path);
    if (extension == ".isz") {
        type_ = Type::isz;
        parse_isz();
    } else if (extension == ".daa") {
        type_ = Type::daa_v1;
        parse_daa();
    } else if (extension == ".chd") {
        type_ = Type::chd_v5;
        parse_chd();
    } else {
        throw std::runtime_error("unsupported packed optical extension");
    }
    check(logical_size_ != 0 && logical_size_ <= max_logical &&
          block_bytes_ >= 2048 && block_bytes_ <= max_block &&
          block_bytes_ % 2048 == 0,
          "packed optical geometry is invalid");
    const auto expected = (logical_size_ - 1) / block_bytes_ + 1;
    check(expected == chunks_.size(),
          "packed optical chunk count disagrees with logical size");
}

std::string PackedOptical::label() const {
    switch (type_) {
    case Type::isz: return "ISZ v1 / native stored-zlib-bzip2";
    case Type::daa_v1: return "DAA v1 / native raw DEFLATE";
    case Type::chd_v5: return "CHD v5 / uncompressed hunks";
    }
    return "packed optical";
}

std::vector<std::uint8_t> PackedOptical::physical_read(
        std::uint64_t pos, std::size_t length) {
    check(length <= 16u * 1024u * 1024u &&
          pos <= physical_size_ && length <= physical_size_ - pos &&
          pos <= static_cast<std::uint64_t>(
              std::numeric_limits<std::streamoff>::max()),
          "packed optical read outside file");
    std::vector<std::uint8_t> result(length);
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(pos), std::ios::beg);
    check(bool(file_) &&
          (length == 0 || bool(file_.read(
              reinterpret_cast<char*>(result.data()),
              static_cast<std::streamsize>(length)))),
          "packed optical image truncated");
    return result;
}

void PackedOptical::parse_isz() {
    check(physical_size_ >= 64, "truncated ISZ header");
    const auto h = physical_read(0, 64);
    check(std::memcmp(h.data(), "IsZ!", 4) == 0 &&
          h[4] == 64 && h[5] == 1,
          "unsupported ISZ signature, header or version");
    check(h[16] == 0 && h[34] == 0,
          "encrypted or non-first-segment ISZ is unsupported");
    const auto sector = le16(h.data() + 10);
    const auto sectors = le32(h.data() + 12);
    check(sector == 2048 && sectors > 0, "ISZ is not a 2048-byte optical stream");
    logical_size_ = std::uint64_t(sector) * sectors;
    block_bytes_ = le32(h.data() + 29);
    check(block_bytes_ >= 2048 && block_bytes_ <= max_block &&
          block_bytes_ % 2048 == 0, "invalid ISZ block size");
    const auto count = le32(h.data() + 25);
    const auto ptr_offset = le32(h.data() + 35);
    const auto segment_table = le32(h.data() + 39);
    const auto data_offset = le32(h.data() + 43);
    check(segment_table == 0,
          "multi-segment ISZ is not supported by native decoder");
    check(count > 0 && count <= max_chunks &&
          count == (logical_size_ - 1)/block_bytes_ + 1,
          "invalid ISZ block count");
    check(data_offset >= 64 && data_offset <= physical_size_,
          "ISZ data offset outside image");
    chunks_.reserve(count);
    if (ptr_offset == 0) {
        check(count == 1 && logical_size_ <= block_bytes_ &&
              le32(h.data() + 52) == logical_size_,
              "unsupported ISZ implicit single-block layout");
        chunks_.push_back({data_offset, static_cast<std::uint32_t>(logical_size_),
                           Codec::stored});
        check(logical_size_ <= physical_size_ - data_offset,
              "truncated ISZ implicit block");
        return;
    }
    check(h[33] == 3 && ptr_offset >= 64 &&
          ptr_offset <= physical_size_ &&
          std::uint64_t(count) * 3 <= physical_size_ - ptr_offset &&
          (std::uint64_t(count) * 3 <= data_offset - std::min<std::uint64_t>(data_offset,ptr_offset)),
          "invalid ISZ obfuscated chunk table");
    std::vector<std::uint8_t> pointers;
    // Avoid unbounded allocations when parsing malformed chunk counts.
    for (std::uint64_t at = 0; at < std::uint64_t(count)*3; ) {
        const auto part = static_cast<std::size_t>(
            std::min<std::uint64_t>(std::uint64_t(count)*3-at, 1u<<20));
        auto bytes = physical_read(std::uint64_t(ptr_offset) + at, part);
        pointers.insert(pointers.end(), bytes.begin(), bytes.end());
        at += part;
    }
    constexpr std::array<std::uint8_t,4> key{0xb6,0x8c,0xa5,0xde};
    std::uint64_t physical = data_offset;
    for (std::size_t i = 0; i < count; ++i) {
        const auto at = i*3;
        for (std::size_t j = 0; j < 3; ++j)
            pointers[at+j] ^= key[(at+j)%4];
        const auto pointer = le24(pointers.data() + at);
        const auto kind = pointer >> 22;
        const auto length = pointer & 0x3fffffu;
        const auto expected = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(block_bytes_,logical_size_-i*block_bytes_));
        Codec codec{};
        switch (kind) {
        case 0:
            check(length == expected || length == block_bytes_,
                  "invalid zero ISZ chunk size");
            codec = Codec::zeros;
            break;
        case 1:
            check(length == expected, "uncompressed ISZ length mismatch");
            codec = Codec::stored;
            break;
        case 2:
            check(length > 0 && length <= block_bytes_ + 65536u,
                  "invalid ISZ zlib size");
            codec = Codec::zlib;
            break;
        case 3:
            check(length >= 4 && length <= block_bytes_ + 65536u,
                  "invalid ISZ bzip2 compressed size");
            codec = Codec::bzip2;
            break;
        default:
            throw std::runtime_error("invalid ISZ chunk codec");
        }
        const auto consumed = codec == Codec::zeros ? 0 : length;
        check(physical <= physical_size_ &&
              consumed <= physical_size_ - physical,
              "ISZ data chunk exceeds the file");
        chunks_.push_back({physical, consumed, codec});
        physical += consumed;
    }
}

void PackedOptical::parse_daa() {
    check(physical_size_ >= 76, "truncated DAA header");
    const auto h = physical_read(0, 76);
    check(std::memcmp(h.data(), "DAA", 3) == 0 &&
          le32(h.data() + 20) == 0x100 &&
          le32(h.data() + 28) == 1 &&
          le32(h.data() + 32) == 0,
          "unsupported DAA header or version (only unencrypted v1)");
    const auto checksum = crc32(
        0, reinterpret_cast<const Bytef*>(h.data()), 72);
    check(checksum == le32(h.data() + 72),
          "DAA header CRC32 mismatch");
    const auto ptr_offset = le32(h.data() + 16);
    const auto data_offset = le32(h.data() + 24);
    block_bytes_ = le32(h.data() + 36);
    logical_size_ = le64(h.data() + 40);
    check(logical_size_ >= 2048 && logical_size_ <= max_logical &&
          logical_size_ % 2048 == 0 &&
          block_bytes_ >= 2048 && block_bytes_ <= max_block &&
          block_bytes_ % 2048 == 0,
          "DAA output geometry invalid");
    const auto blocks = (logical_size_ - 1)/block_bytes_ + 1;
    check(blocks <= max_chunks && ptr_offset == 76 &&
          ptr_offset <= data_offset &&
          std::uint64_t(blocks)*3 <= data_offset - ptr_offset &&
          data_offset <= physical_size_,
          "DAA v1 index is malformed or contains unsupported extensions");
    std::uint64_t pos = data_offset;
    for (std::uint64_t base = 0; base < blocks; ) {
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(blocks-base, (1u<<20)/3));
        const auto index = physical_read(
            std::uint64_t(ptr_offset) + base*3, count*3);
        for (std::size_t i = 0; i < count; ++i) {
            const auto p = index.data() + i*3;
            const auto compressed = (std::uint32_t(p[0]) << 16) |
                                    (std::uint32_t(p[2]) << 8) | p[1];
            check(compressed > 0 && compressed <= block_bytes_ + 65536u &&
                  pos <= physical_size_ &&
                  compressed <= physical_size_ - pos,
                  "DAA v1 compressed chunk out of bounds");
            chunks_.push_back({pos,compressed,Codec::raw_deflate});
            pos += compressed;
        }
        base += count;
    }
}

void PackedOptical::parse_chd() {
    check(physical_size_ >= 124, "truncated CHD header");
    const auto h = physical_read(0, 124);
    check(std::memcmp(h.data(), "MComprHD", 8) == 0 &&
          be32(h.data() + 8) == 124 &&
          be32(h.data() + 12) == 5,
          "only CHD v5 headers are supported");
    for (std::size_t i = 0; i < 4; ++i)
        check(be32(h.data() + 16 + i*4) == 0,
              "compressed CHD v5 requires entropy map and codecs not yet implemented");
    check(std::all_of(h.begin()+104,h.begin()+124,
                      [](std::uint8_t c){return c==0;}),
          "CHD parent chains are unsupported");
    logical_size_ = be64(h.data()+32);
    const auto map_offset = be64(h.data()+40);
    block_bytes_ = be32(h.data()+56);
    const auto unit_bytes = be32(h.data()+60);
    check(logical_size_ != 0 && logical_size_ <= max_logical &&
          logical_size_ % 2048 == 0 &&
          block_bytes_ >= 2048 && block_bytes_ <= max_block &&
          block_bytes_ % 2048 == 0 && unit_bytes == 2048,
          "CHD v5 is not a supported 2048-byte logical optical stream");
    const auto blocks = (logical_size_-1)/block_bytes_ + 1;
    check(blocks <= max_chunks && map_offset >= 124 &&
          map_offset <= physical_size_ &&
          blocks*4 <= physical_size_ - map_offset,
          "CHD v5 uncompressed hunk map is malformed");
    const auto map_end = map_offset + blocks*4;
    for (std::uint64_t base = 0; base < blocks; ) {
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(blocks-base, 1u<<18));
        const auto map = physical_read(map_offset+base*4,count*4);
        for (std::size_t i=0;i<count;++i) {
            const auto index = be32(map.data()+i*4);
            if (!index) {
                chunks_.push_back({0,0,Codec::zeros});
                continue;
            }
            const auto at = std::uint64_t(index)*block_bytes_;
            check(at >= 124 && at <= physical_size_ &&
                  block_bytes_ <= physical_size_-at &&
                  (at >= map_end || (at <= map_offset &&
                                     block_bytes_ <= map_offset-at)),
                  "CHD hunk overlaps map, header or end of image");
            chunks_.push_back({at,block_bytes_,Codec::stored});
        }
        base += count;
    }
}

void PackedOptical::decode(std::size_t index) {
    check(index < chunks_.size(), "packed optical block outside map");
    const auto expected = static_cast<std::size_t>(
        std::min<std::uint64_t>(block_bytes_,logical_size_-std::uint64_t(index)*block_bytes_));
    const auto& entry = chunks_[index];
    cached_bytes_.assign(block_bytes_,0);
    switch(entry.codec) {
    case Codec::zeros:
        break;
    case Codec::stored: {
        check(entry.encoded_size >= expected, "truncated packed ISO stored block");
        const auto data = physical_read(entry.offset,expected);
        std::copy(data.begin(),data.end(),cached_bytes_.begin());
        break;
    }
    case Codec::bzip2: {
        const auto stored = physical_read(entry.offset,entry.encoded_size);
        check(stored.size() >= 4, "truncated ISZ bzip2 block");
        // ISZ substitutes the BZh signature with its private 3-byte prefix;
        // the remaining bzip2 stream and its CRCs are standard.
        auto data = stored;
        data[0] = 'B'; data[1] = 'Z'; data[2] = 'h';
        unsigned out_size = static_cast<unsigned>(cached_bytes_.size());
        const auto result = BZ2_bzBuffToBuffDecompress(
            reinterpret_cast<char*>(cached_bytes_.data()), &out_size,
            reinterpret_cast<char*>(data.data()),
            static_cast<unsigned>(data.size()), 0, 0);
        check(result == BZ_OK &&
              (out_size == expected || out_size == cached_bytes_.size()),
              "invalid ISZ bzip2 block or CRC");
        break;
    }
    case Codec::zlib:
    case Codec::raw_deflate: {
        const auto data = physical_read(entry.offset,entry.encoded_size);
        inflate_exact(data,cached_bytes_,expected,
                      entry.codec == Codec::zlib ? MAX_WBITS : -MAX_WBITS);
        break;
    }
    }
    cached_chunk_=index;
}

std::vector<std::uint8_t> PackedOptical::read(std::uint64_t offset,std::size_t length) {
    check(length <= 32u*1024u*1024u && offset <= logical_size_ &&
          length <= logical_size_-offset,
          "packed optical logical extent outside image");
    std::vector<std::uint8_t> output(length);
    std::size_t done=0;
    while(done<length) {
        const auto absolute = offset+done;
        const auto block = static_cast<std::size_t>(absolute/block_bytes_);
        if(block!=cached_chunk_)decode(block);
        const auto inside=static_cast<std::size_t>(absolute%block_bytes_);
        const auto amount=std::min<std::size_t>(length-done,block_bytes_-inside);
        std::copy_n(cached_bytes_.begin()+static_cast<std::ptrdiff_t>(inside),
                    amount,output.begin()+static_cast<std::ptrdiff_t>(done));
        done+=amount;
    }
    return output;
}
} // namespace iso_wcx
