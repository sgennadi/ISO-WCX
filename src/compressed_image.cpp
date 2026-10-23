#include "iso_wcx/compressed_image.hpp"
#include <zlib.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace iso_wcx {
namespace {
void verify(bool yes, const char* message) {
    if(!yes) throw std::runtime_error(message);
}
std::uint32_t le32(const std::uint8_t* b) {
    return std::uint32_t(b[0]) | std::uint32_t(b[1])<<8 | std::uint32_t(b[2])<<16 |
           std::uint32_t(b[3])<<24;
}
std::uint64_t le64(const std::uint8_t* b) {
    return std::uint64_t(le32(b)) | (std::uint64_t(le32(b+4))<<32);
}
void deflate_read(const std::vector<std::uint8_t>& input,std::vector<std::uint8_t>& output,
                  std::size_t minimum) {
    verify(input.size()<=std::numeric_limits<uInt>::max() &&
           output.size()<=std::numeric_limits<uInt>::max(),"CSO zlib input exceeds limit");
    z_stream z{};
    verify(inflateInit2(&z,-MAX_WBITS)==Z_OK,"cannot initialize raw DEFLATE");
    z.next_in=const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data()));
    z.avail_in=static_cast<uInt>(input.size());
    z.next_out=reinterpret_cast<Bytef*>(output.data());
    z.avail_out=static_cast<uInt>(output.size());
    const int result=inflate(&z,Z_FINISH);
    const auto produced=z.total_out;
    inflateEnd(&z);
    verify(result==Z_STREAM_END && produced>=minimum && produced<=output.size(),
           "invalid CSO raw DEFLATE block");
}
void lz4_read(const std::vector<std::uint8_t>& input,std::vector<std::uint8_t>& output,
              std::size_t minimum,unsigned alignment) {
    // Bare LZ4 block, not a framed stream. Every length and backreference is
    // checked before accessing the input/output buffer.
    std::size_t src=0,dst=0;
    auto trailing_padding=[&]() {
        // Alignment bytes follow compressed blocks, including a final
        // literal-only sequence. Never drop legitimate nonzero payload.
        return dst>=minimum && input.size()-src < (std::size_t(1)<<alignment) &&
               std::all_of(input.begin()+static_cast<std::ptrdiff_t>(src),input.end(),
                           [](std::uint8_t x){return x==0;});
    };
    while(src<input.size()) {
        if(trailing_padding())break;
        const auto token=input[src++];
        auto grow=[&](std::size_t len)->std::size_t {
            if(len!=15)return len;
            for(;;) {
                verify(src<input.size(),"truncated LZ4 length");
                auto extension=static_cast<std::size_t>(input[src++]);
                verify(len<=output.size() && extension<=output.size()-len,
                       "LZ4 expansion exceeds block size");
                len+=extension;
                if(extension!=255)break;
            }
            return len;
        };
        const auto literal=grow(token>>4);
        verify(literal<=input.size()-src && literal<=output.size()-dst,
               "invalid LZ4 literal length");
        std::copy_n(input.begin()+static_cast<std::ptrdiff_t>(src),
                    static_cast<std::ptrdiff_t>(literal),
                    output.begin()+static_cast<std::ptrdiff_t>(dst));
        src+=literal;dst+=literal;
        if(src==input.size() || trailing_padding())break;
        verify(input.size()-src>=2,"missing LZ4 match offset");
        const auto offset=std::uint32_t(input[src]) | (std::uint32_t(input[src+1])<<8);
        src+=2;
        verify(offset>0 && offset<=dst,"invalid LZ4 match backreference");
        const auto match_extra=grow(token&15u);
        verify(match_extra<=output.size()-dst && 4<=output.size()-dst-match_extra,
               "LZ4 match exceeds output block");
        for(std::size_t j=0;j<match_extra+4;j++){
            output[dst]=output[dst-offset];
            ++dst;
        }
    }
    verify(dst>=minimum && dst<=output.size(),"truncated ZSO LZ4 block");
}
}

bool is_compressed_image_path(const std::filesystem::path& file) {
    auto ext=file.extension().string();
    std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return char(std::tolower(c));});
    return ext==".cso" || ext==".zso";
}

CompressedImage::CompressedImage(const std::filesystem::path& path)
    :file_(path,std::ios::binary|std::ios::ate) {
    verify(bool(file_),"cannot open compressed ISO");
    const auto n=file_.tellg();verify(n>=0,"cannot stat compressed ISO");
    file_size_=static_cast<std::uint64_t>(n);
    verify(file_size_>=28,"compressed ISO header or index missing");
    const auto header=physical_read(0,24);
    const std::string_view magic(reinterpret_cast<const char*>(header.data()),4);
    const auto version=header[20];
    if(magic=="CISO"){
        verify(version<=2,"unsupported CSO version");
        format_=version==2?Format::CsoV2:Format::CsoV1;
    }else if(magic=="ZISO"){
        verify(version<=1,"unsupported ZSO version");
        format_=Format::Zso;
    }else throw std::runtime_error("unrecognized CISO/ZISO magic");
    const auto header_bytes=le32(header.data()+4);
    verify(header_bytes==24 || (header_bytes==0 && format_==Format::CsoV1),
           "invalid compressed ISO header length");
    total_size_=le64(header.data()+8);
    block_size_=le32(header.data()+16);
    alignment_=header[21];
    verify(total_size_>0 && total_size_<=(std::uint64_t(1)<<44),
           "compressed ISO virtual size outside supported range");
    verify(block_size_>=2048 && block_size_<=1<<20 && (block_size_%2048)==0,
           "invalid compressed ISO block size");
    verify(alignment_<=15,"unsupported CSO/ZSO index alignment");
    const auto blocks=(total_size_+block_size_-1)/block_size_;
    verify(blocks>0 && blocks<=16'000'000,"compressed ISO block count exceeds limit");
    verify(24+(blocks+1)*4<=file_size_,"truncated compressed ISO index");
    const auto index_data=physical_read(24,static_cast<std::size_t>((blocks+1)*4));
    indexes_.reserve(static_cast<std::size_t>(blocks+1));
    for(std::size_t p=0;p<index_data.size();p+=4)
        indexes_.push_back(le32(index_data.data()+p));
    std::uint64_t previous=0;
    for(std::size_t i=0;i<indexes_.size();i++) {
        const auto current=std::uint64_t(indexes_[i]&0x7fffffffu)<<alignment_;
        verify(current<=file_size_ && (i==0 || current>=previous),
               "compressed ISO index offsets are out of order or out of file");
        if(i==0)
            verify(current>=24+(blocks+1)*4,"compressed ISO data overlaps index");
        if(i+1<indexes_.size()) {
            const auto next=std::uint64_t(indexes_[i+1]&0x7fffffffu)<<alignment_;
            verify(next>=current && next-current<=std::uint64_t(block_size_)+65536,
                   "compressed ISO block length invalid");
        }
        previous=current;
    }
}
std::string CompressedImage::label() const {
    switch(format_) {
        case Format::CsoV1:return "CSO v1 / raw DEFLATE";
        case Format::CsoV2:return "CSO v2 / DEFLATE-LZ4";
        case Format::Zso:return "ZSO / LZ4";
    }
    return "compressed ISO";
}
std::vector<std::uint8_t> CompressedImage::physical_read(std::uint64_t at,std::size_t count) {
    verify(at<=file_size_ && count<=file_size_-at &&
           at<=static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()),
           "compressed ISO extent outside file");
    std::vector<std::uint8_t> bytes(count);
    file_.clear();file_.seekg(static_cast<std::streamoff>(at));
    verify(bool(file_) && (count==0 || bool(file_.read(reinterpret_cast<char*>(bytes.data()),
                       static_cast<std::streamsize>(count)))),"cannot read compressed ISO");
    return bytes;
}
void CompressedImage::decode(std::uint64_t block) {
    verify(block+1<indexes_.size(),"compressed ISO block index invalid");
    const auto start=std::uint64_t(indexes_[static_cast<std::size_t>(block)]&0x7fffffffu)<<alignment_;
    const auto end=std::uint64_t(indexes_[static_cast<std::size_t>(block)+1]&0x7fffffffu)<<alignment_;
    verify(end>=start && end<=file_size_,"compressed ISO index out of bounds");
    const auto expected=static_cast<std::size_t>(std::min<std::uint64_t>(block_size_,total_size_-block*block_size_));
    const auto stored=static_cast<std::size_t>(end-start);
    const bool flagged=(indexes_[static_cast<std::size_t>(block)]&0x80000000u)!=0;
    const bool plain = (format_==Format::CsoV2 && stored>=block_size_) ||
                       (format_!=Format::CsoV2 && flagged);
    verify(stored>0 && stored<=std::size_t(block_size_)+65536,"invalid CSO/ZSO encoded length");
    const auto compressed=physical_read(start,stored);
    cached_data_.assign(block_size_,0);
    if(plain) {
        verify(compressed.size()>=expected,"truncated uncompressed CSO/ZSO block");
        std::copy_n(compressed.begin(),static_cast<std::ptrdiff_t>(expected),cached_data_.begin());
    }else if(format_==Format::Zso || (format_==Format::CsoV2 && flagged)) {
        lz4_read(compressed,cached_data_,expected,alignment_);
    }else {
        deflate_read(compressed,cached_data_,expected);
    }
    cached_index_=block;
}
std::vector<std::uint8_t> CompressedImage::read(std::uint64_t offset,std::size_t bytes) {
    verify(bytes<=32*1024*1024 && offset<=total_size_ && bytes<=total_size_-offset,
           "compressed ISO read outside image");
    std::vector<std::uint8_t> out(bytes);
    std::size_t copied=0;
    while(copied<bytes) {
        const auto absolute=offset+copied;
        const auto block=absolute/block_size_;
        if(block!=cached_index_)decode(block);
        const auto in=static_cast<std::size_t>(absolute%block_size_);
        const auto n=std::min<std::size_t>(bytes-copied,block_size_-in);
        std::copy_n(cached_data_.begin()+static_cast<std::ptrdiff_t>(in),
                    static_cast<std::ptrdiff_t>(n),
                    out.begin()+static_cast<std::ptrdiff_t>(copied));
        copied+=n;
    }
    return out;
}
} // namespace iso_wcx
