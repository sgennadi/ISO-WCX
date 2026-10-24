#include "iso_wcx/packed_optical.hpp"
#include "iso_wcx/chd_stream.hpp"
#include "iso_wcx/mds_v2_tracks.hpp"

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
std::uint32_t le32(const std::uint8_t* p) {
    return le16(p) | (std::uint32_t(le16(p + 2)) << 16);
}
std::uint64_t le64(const std::uint8_t* p) {
    return le32(p) | (std::uint64_t(le32(p + 4)) << 32);
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
    check(encoded.size() <= (std::numeric_limits<uInt>::max)() &&
          decoded.size() <= (std::numeric_limits<uInt>::max)(),
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
    return extension == ".isz" || extension == ".daa" || extension == ".chd" ||
           extension == ".mds" || extension == ".mdx";
}

PackedOptical::PackedOptical(const std::filesystem::path& path)
    : image_path_(path), file_(path, std::ios::binary | std::ios::ate) {
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
        chd_ = std::make_unique<ChdStream>(path);
        logical_size_ = chd_->size();
        block_bytes_ = 2048;
    } else if (extension == ".mds" || extension == ".mdx") {
        type_ = Type::mds_v2;
        mdsv2_ = std::make_unique<MdsV2TrackStream>(path);
        logical_size_ = mdsv2_->size();
        block_bytes_ = 2048;
    } else {
        throw std::runtime_error("unsupported packed optical extension");
    }
    check(logical_size_ != 0 && logical_size_ <= max_logical &&
          block_bytes_ >= 2048 && block_bytes_ <= max_block &&
          block_bytes_ % 2048 == 0,
          "packed optical geometry is invalid");
    if (!chd_ && !mdsv2_) {
        const auto expected = (logical_size_ - 1) / block_bytes_ + 1;
        check(expected == chunks_.size(),
              "packed optical chunk count disagrees with logical size");
    }
}

PackedOptical::~PackedOptical() = default;

std::string PackedOptical::label() const {
    switch (type_) {
    case Type::isz: return isz_regions_.size() > 1
        ? "ISZ v1 / native stored-zlib-bzip2 / split"
        : "ISZ v1 / native stored-zlib-bzip2";
    case Type::daa_v1: return "DAA v1 / native raw DEFLATE";
    case Type::chd_v5: return chd_->label();
    case Type::mds_v2: return mdsv2_->label();
    }
    return "packed optical";
}

std::vector<std::uint8_t> PackedOptical::physical_read(
        std::uint64_t pos, std::size_t length) {
    check(length <= 16u * 1024u * 1024u &&
          pos <= physical_size_ && length <= physical_size_ - pos &&
          pos <= static_cast<std::uint64_t>(
              (std::numeric_limits<std::streamoff>::max)()),
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

void PackedOptical::append_isz_region(const std::filesystem::path& path,
                                       std::uint64_t begin,
                                       std::uint64_t declared_end) {
    check(std::filesystem::is_regular_file(path) &&
          !std::filesystem::is_symlink(path),
          "ISZ segment must be a regular non-symlink file");
    auto input=std::make_unique<std::ifstream>(path,std::ios::binary|std::ios::ate);
    check(bool(*input) && input->tellg()>=0,"cannot open ISZ segment");
    const auto physical=static_cast<std::uint64_t>(input->tellg());
    // Refuse a short middle part: concatenating a truncated region with
    // the next one would shift the virtual stream and corrupt stored blocks.
    check(declared_end<=physical,"ISZ segment shorter than declared");
    // An image consisting only of sparse-zero chunks has no stored data.
    check(begin>=48 && begin<=declared_end,
          "ISZ segment data area is outside the file");
    const auto length=declared_end-begin;
    check(isz_data_size_<=max_logical && length<=max_logical-isz_data_size_,
          "ISZ segment data stream exceeds size limit");
    isz_regions_.push_back({std::move(input),begin,isz_data_size_,length});
    isz_data_size_+=length;
}

std::vector<std::uint8_t> PackedOptical::read_isz_data(
        std::uint64_t position,std::size_t length) {
    check(length<=16u*1024u*1024u && position<=isz_data_size_ &&
          length<=isz_data_size_-position,
          "ISZ chunk references missing or truncated segment data");
    std::vector<std::uint8_t> result(length);
    std::size_t done=0;
    for(auto& region:isz_regions_) {
        if(done==length)break;
        const auto at=position+done;
        if(at>=region.logical_offset+region.length)continue;
        check(at>=region.logical_offset,"ISZ region map overlaps");
        const auto inside=at-region.logical_offset;
        const auto amount=static_cast<std::size_t>(
            std::min<std::uint64_t>(length-done,region.length-inside));
        auto& stream=*region.input;
        check(region.file_offset<=
                  static_cast<std::uint64_t>((std::numeric_limits<std::streamoff>::max)())-inside,
              "ISZ region seek overflow");
        stream.clear();
        stream.seekg(static_cast<std::streamoff>(region.file_offset+inside));
        check(bool(stream) &&
              (amount==0 || bool(stream.read(
                  reinterpret_cast<char*>(result.data()+done),
                  static_cast<std::streamsize>(amount)))),
              "ISZ segment ended unexpectedly");
        done+=amount;
    }
    check(done==length,"ISZ segment data is incomplete");
    return result;
}

void PackedOptical::parse_isz() {
    check(physical_size_>=48,"truncated ISZ header");
    const auto h=physical_read(0,static_cast<std::size_t>(
        std::min<std::uint64_t>(physical_size_,64)));
    const auto header_size=h[4];
    check(std::memcmp(h.data(),"IsZ!",4)==0 &&
          (header_size==48 || header_size==64) &&
          h[5]==1 && physical_size_>=header_size,
          "unsupported ISZ signature, header or version");
    check(h[16]==0 && h[34]==0,
          "encrypted or non-first-segment ISZ is unsupported");
    const auto sector=le16(h.data()+10);
    const auto sectors=le32(h.data()+12);
    check(sector==2048 && sectors>0,"ISZ is not a 2048-byte optical stream");
    logical_size_=std::uint64_t(sector)*sectors;
    block_bytes_=le32(h.data()+29);
    check(block_bytes_>=2048 && block_bytes_<=max_block &&
          block_bytes_%2048==0,"invalid ISZ block size");
    const auto expected=(logical_size_-1)/block_bytes_+1;
    check(expected<=max_chunks,"ISZ chunk count exceeds supported limit");
    const auto count=le32(h.data()+25);
    const auto pointer_width=h[33];
    const auto ptr_offset=le32(h.data()+35);
    const auto segment_table=le32(h.data()+39);
    const auto data_offset=le32(h.data()+43);
    check(data_offset>=header_size && data_offset<=physical_size_,
          "ISZ data offset outside image");

    struct Segment {
        std::uint64_t size;
        std::uint32_t chunks,first,offset,left;
    };
    std::vector<Segment> segments;
    constexpr std::array<std::uint8_t,4> key{0xb6,0x8c,0xa5,0xde};
    if(segment_table!=0) {
        check(segment_table>=header_size && segment_table<=physical_size_,
              "ISZ segment table outside first file");
        bool terminated=false;
        for(std::size_t i=0;i<=99;i++) {
            const auto pos=std::uint64_t(segment_table)+i*24u;
            check(pos<=physical_size_ && 24<=physical_size_-pos,
                  "ISZ segment table truncated");
            auto s=physical_read(pos,24);
            for(std::size_t j=0;j<s.size();j++)s[j]^=key[j%4];
            const auto size=le64(s.data());
            if(size==0){terminated=true;break;}
            check(i<99 && size>=48 && size<=max_logical,
                  "ISZ segment declares invalid length");
            const auto chunk_count=le32(s.data()+8);
            const auto first=le32(s.data()+12);
            const auto chunk_offset=le32(s.data()+16);
            const auto left=le32(s.data()+20);
            check(first<=expected && chunk_count<=expected-first &&
                  chunk_offset<=size && left<=block_bytes_+65536u,
                  "ISZ segment table has invalid chunk spans");
            segments.push_back({size,chunk_count,first,chunk_offset,left});
        }
        check(terminated && !segments.empty(),
              "ISZ segment table missing terminating entry");
        const auto table_limit=ptr_offset?std::uint64_t(ptr_offset):
                                         std::uint64_t(data_offset);
        check(std::uint64_t(segment_table)+
                  std::uint64_t(segments.size()+1)*24<=table_limit,
              "ISZ segment table overlaps the chunk table or data");
        std::uint64_t next_chunk=0;
        for(const auto& part:segments) {
            check(part.first==next_chunk,
                  "ISZ segment chunk ranges are not contiguous");
            next_chunk+=part.chunks;
        }
        if(ptr_offset!=0 || next_chunk!=0)
            check(next_chunk==expected,
                  "ISZ segment chunk ranges are incomplete");
    }

    if(segments.empty()) {
        append_isz_region(image_path_,data_offset,physical_size_);
    } else {
        // UltraISO supports .isz + .i01/.i02 and .part01.isz + .part02.isz.
        // An explicitly named part must not follow links outside the folder.
        const auto base=image_path_.stem().wstring();
        const auto part_pos=base.rfind(L".part");
        unsigned part_digits=0;
        if(part_pos!=std::wstring::npos) {
            const auto suffix=base.substr(part_pos+5);
            if((suffix.size()==2 || suffix.size()==3) &&
               std::all_of(suffix.begin(),suffix.end(),
                   [](wchar_t ch){return ch>=L'0' && ch<=L'9';}) &&
               std::stoul(suffix)==1)
                part_digits=static_cast<unsigned>(suffix.size());
        }
        for(std::size_t i=0;i<segments.size();++i) {
            auto path=image_path_;
            if(i!=0) {
                std::wstring name;
                if(part_digits!=0) {
                    auto number=std::to_wstring(i+1);
                    check(number.size()<=part_digits,"ISZ part number overflow");
                    name=base.substr(0,part_pos)+L".part"+
                        std::wstring(part_digits-number.size(),L'0')+number+L".isz";
                } else {
                    auto number=std::to_wstring(i);
                    if(number.size()<2)number=L"0"+number;
                    name=base+L".i"+number;
                }
                path=image_path_.parent_path()/name;
                check(std::filesystem::is_regular_file(path) &&
                      !std::filesystem::is_symlink(path),
                      "missing or unsafe ISZ companion segment");
                std::ifstream input(path,std::ios::binary);
                std::array<std::uint8_t,48> header{};
                check(bool(input.read(reinterpret_cast<char*>(header.data()),48)) &&
                      std::memcmp(header.data(),"IsZ!",4)==0 &&
                      (header[4]==48 || header[4]==64) && header[5]==1 &&
                      header[16]==0 &&
                      le32(header.data()+6)==le32(h.data()+6) &&
                      header[34]==i,
                      "ISZ companion segment signature, serial or index mismatch");
            }
            const auto& seg=segments[i];
            const auto begin=i==0
                ? std::uint64_t(seg.offset?seg.offset:data_offset)
                : std::uint64_t(seg.offset);
            const auto left=i==0?0u:segments[i-1].left;
            check(begin>=left && begin-left>=(i==0?header_size:48),
                  "ISZ segment continuation points inside header");
            append_isz_region(path,begin-left,seg.size);
        }
    }

    if(ptr_offset==0) {
        // ISZ without a pointer table is an uncompressed virtual ISO stream.
        check(count==0 || count==expected,
              "ISZ implicit chunk count disagrees with image size");
        check(logical_size_<=isz_data_size_,
              "ISZ implicit stored image is truncated");
        chunks_.reserve(static_cast<std::size_t>(expected));
        for(std::uint64_t i=0;i<expected;i++) {
            const auto length=static_cast<std::uint32_t>(
                std::min<std::uint64_t>(block_bytes_,
                                        logical_size_-i*block_bytes_));
            chunks_.push_back({i*block_bytes_,length,Codec::stored});
        }
        return;
    }
    check(count==expected && count<=max_chunks &&
          pointer_width>=1 && pointer_width<=4 &&
          ptr_offset>=header_size && ptr_offset<=physical_size_ &&
          std::uint64_t(count)*pointer_width<=physical_size_-ptr_offset &&
          ptr_offset<=data_offset &&
          std::uint64_t(count)*pointer_width<=data_offset-ptr_offset,
          "ISZ chunk table is invalid or overlaps data");
    const auto table_size=std::uint64_t(count)*pointer_width;
    std::vector<std::uint8_t> table;
    table.reserve(static_cast<std::size_t>(table_size));
    for(std::uint64_t at=0;at<table_size;) {
        const auto part=static_cast<std::size_t>(
            std::min<std::uint64_t>(table_size-at,1u<<20));
        auto bytes=physical_read(std::uint64_t(ptr_offset)+at,part);
        for(std::size_t j=0;j<part;j++)bytes[j]^=key[(at+j)%4];
        table.insert(table.end(),bytes.begin(),bytes.end());
        at+=part;
    }
    const auto shift=static_cast<unsigned>(8*pointer_width-2);
    const auto mask=(std::uint64_t(1)<<shift)-1;
    std::uint64_t stored_at=0;
    chunks_.reserve(static_cast<std::size_t>(count));
    for(std::uint64_t i=0;i<count;i++) {
        std::uint64_t value=0;
        for(unsigned j=0;j<pointer_width;j++)
            value|=std::uint64_t(table[static_cast<std::size_t>(i*pointer_width+j)])<<(8*j);
        const auto kind=value>>shift;
        const auto length=static_cast<std::uint32_t>(value&mask);
        const auto expanded=static_cast<std::uint32_t>(
            std::min<std::uint64_t>(block_bytes_,logical_size_-i*block_bytes_));
        Codec codec{};
        switch(kind) {
        case 0:
            check(length==0 || length==expanded || length==block_bytes_,
                  "invalid zero ISZ chunk size");
            codec=Codec::zeros;break;
        case 1:
            check(length==expanded || length==block_bytes_,
                  "uncompressed ISZ length mismatch");
            codec=Codec::stored;break;
        case 2:
            check(length>0 && length<=block_bytes_+65536u,
                  "invalid ISZ zlib size");
            codec=Codec::zlib;break;
        case 3:
            check(length>=4 && length<=block_bytes_+65536u,
                  "invalid ISZ bzip2 size");
            codec=Codec::bzip2;break;
        default:
            throw std::runtime_error("invalid ISZ chunk codec");
        }
        const auto consumed=codec==Codec::zeros?0u:length;
        check(stored_at<=isz_data_size_ &&
              consumed<=isz_data_size_-stored_at,
              "ISZ chunk references missing segment data");
        chunks_.push_back({stored_at,consumed,codec});
        stored_at+=consumed;
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
        const auto data = type_==Type::isz
            ? read_isz_data(entry.offset,expected)
            : physical_read(entry.offset,expected);
        std::copy(data.begin(),data.end(),cached_bytes_.begin());
        break;
    }
    case Codec::bzip2: {
        const auto stored = type_==Type::isz
            ? read_isz_data(entry.offset,entry.encoded_size)
            : physical_read(entry.offset,entry.encoded_size);
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
        const auto data = type_==Type::isz
            ? read_isz_data(entry.offset,entry.encoded_size)
            : physical_read(entry.offset,entry.encoded_size);
        inflate_exact(data,cached_bytes_,expected,
                      entry.codec == Codec::zlib ? MAX_WBITS : -MAX_WBITS);
        break;
    }
    }
    cached_chunk_=index;
}

std::vector<std::uint8_t> PackedOptical::read(std::uint64_t offset,std::size_t length) {
    if (chd_) return chd_->read(offset,length);
    if (mdsv2_) return mdsv2_->read(offset,length);
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
