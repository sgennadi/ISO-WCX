#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace iso_wcx {
class ChdStream;
class MdsV2TrackStream;

// Read-only, random-access optical payload stream. All codecs are in-process.
// No mounting, external helper executable, third-party plugin DLL or
// intermediate ISO file. Unsupported/encrypted variants fail closed.
bool is_packed_optical_path(const std::filesystem::path& path);

class PackedOptical {
public:
    explicit PackedOptical(const std::filesystem::path& path);
    ~PackedOptical();
    [[nodiscard]] std::uint64_t size() const noexcept { return logical_size_; }
    [[nodiscard]] std::string label() const;
    [[nodiscard]] std::vector<std::uint8_t> read(std::uint64_t offset,
                                                 std::size_t length);
private:
    enum class Type { isz, daa_v1, chd_v5, mds_v2 };
    enum class Codec : std::uint8_t { zeros, stored, zlib, raw_deflate, bzip2 };
    struct Chunk {
        std::uint64_t offset = 0;
        std::uint32_t encoded_size = 0;
        Codec codec = Codec::zeros;
    };
    // Non-owning virtual offsets into securely checked, opened ISZ parts.
    struct IszRegion {
        std::unique_ptr<std::ifstream> input;
        std::uint64_t file_offset = 0;
        std::uint64_t logical_offset = 0;
        std::uint64_t length = 0;
    };

    void append_isz_region(const std::filesystem::path& path,
                           std::uint64_t begin, std::uint64_t declared_end);
    [[nodiscard]] std::vector<std::uint8_t> read_isz_data(
        std::uint64_t position, std::size_t length);
    void parse_isz();
    void parse_daa();
    void decode(std::size_t index);
    [[nodiscard]] std::vector<std::uint8_t> physical_read(
        std::uint64_t position, std::size_t length);
    std::unique_ptr<ChdStream> chd_;
    std::unique_ptr<MdsV2TrackStream> mdsv2_;
    std::filesystem::path image_path_;
    std::ifstream file_;
    std::uint64_t physical_size_ = 0;
    std::uint64_t logical_size_ = 0;
    std::uint32_t block_bytes_ = 0;
    Type type_ = Type::isz;
    std::vector<Chunk> chunks_;
    std::vector<IszRegion> isz_regions_;
    std::uint64_t isz_data_size_ = 0;
    std::size_t cached_chunk_ = std::numeric_limits<std::size_t>::max();
    std::vector<std::uint8_t> cached_bytes_;
};

} // namespace iso_wcx
