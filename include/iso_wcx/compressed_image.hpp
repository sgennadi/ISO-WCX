#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace iso_wcx {
// Read-only indexed CISO/ZISO compressed ISO container.
// Designed for bounded on-demand block decompression without an intermediate ISO.
class CompressedImage {
public:
    explicit CompressedImage(const std::filesystem::path& file);
    [[nodiscard]] std::uint64_t size() const noexcept {return total_size_;}
    [[nodiscard]] std::string label() const;
    std::vector<std::uint8_t> read(std::uint64_t offset,std::size_t bytes);
private:
    enum class Format { CsoV1, CsoV2, Zso };
    std::ifstream file_;
    std::uint64_t file_size_{},total_size_{};
    std::uint32_t block_size_{};
    unsigned alignment_{};
    Format format_{};
    std::vector<std::uint32_t> indexes_;
    std::uint64_t cached_index_ = std::numeric_limits<std::uint64_t>::max();
    std::vector<std::uint8_t> cached_data_;
    void decode(std::uint64_t block);
    std::vector<std::uint8_t> physical_read(std::uint64_t at,std::size_t count);
};
bool is_compressed_image_path(const std::filesystem::path& file);
} // namespace iso_wcx
