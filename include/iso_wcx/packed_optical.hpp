#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace iso_wcx {

// Read-only, random-access optical payload stream. All codecs are in-process.
// No mounting, external helper executable, third-party plugin DLL or
// intermediate ISO file. Unsupported/encrypted variants fail closed.
bool is_packed_optical_path(const std::filesystem::path& path);

class PackedOptical {
public:
    explicit PackedOptical(const std::filesystem::path& path);
    [[nodiscard]] std::uint64_t size() const noexcept { return logical_size_; }
    [[nodiscard]] std::string label() const;
    [[nodiscard]] std::vector<std::uint8_t> read(std::uint64_t offset,
                                                 std::size_t length);
private:
    enum class Type { isz, daa_v1, chd_v5 };
    enum class Codec : std::uint8_t { zeros, stored, zlib, raw_deflate, bzip2 };
    struct Chunk {
        std::uint64_t offset = 0;
        std::uint32_t encoded_size = 0;
        Codec codec = Codec::zeros;
    };

    void parse_isz();
    void parse_daa();
    void parse_chd();
    void decode(std::size_t index);
    [[nodiscard]] std::vector<std::uint8_t> physical_read(
        std::uint64_t position, std::size_t length);
    std::ifstream file_;
    std::uint64_t physical_size_ = 0;
    std::uint64_t logical_size_ = 0;
    std::uint32_t block_bytes_ = 0;
    Type type_ = Type::isz;
    std::vector<Chunk> chunks_;
    std::size_t cached_chunk_ = std::numeric_limits<std::size_t>::max();
    std::vector<std::uint8_t> cached_bytes_;
};

} // namespace iso_wcx
