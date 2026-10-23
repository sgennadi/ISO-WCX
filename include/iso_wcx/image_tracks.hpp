#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace iso_wcx {
// Uncompressed, supported filesystem-bearing tracks only. All file offsets are
// absolute physical byte offsets; 'end' is an exclusive physical byte bound.
// Parsing these descriptors never interprets the files inside a disc image.
struct ImageTrack {
    std::filesystem::path image;
    std::uint64_t begin{}, end{};
    std::uint32_t physical_sector{};
    std::uint32_t data_offset{};
    std::uint8_t mode{};    // 0=2048 data, 1=CD mode 1, 2=CD mode 2 Form 1
    std::uint32_t session_lba{}; // Optional absolute LBA for later sessions
    unsigned track_number{};
    std::string container;
    bool scrambled=false; // ECMA-130 CD data scrambler indicated by CloneCD CCD
};
[[nodiscard]] bool is_nrg_path(const std::filesystem::path& image);
[[nodiscard]] bool is_mds_path(const std::filesystem::path& descriptor);
[[nodiscard]] bool is_ccd_path(const std::filesystem::path& descriptor);
// Each loader validates the descriptor/footer and throws for invalid containers;
// unsupported audio or packed/subchannel tracks are ignored.
[[nodiscard]] std::vector<ImageTrack> nrg_data_tracks(const std::filesystem::path& image);
[[nodiscard]] std::vector<ImageTrack> mds_data_tracks(const std::filesystem::path& descriptor);
// CloneCD v3, single-session, unscrambled 2352-byte .img tracks only.
[[nodiscard]] std::vector<ImageTrack> ccd_data_tracks(const std::filesystem::path& descriptor);
}
