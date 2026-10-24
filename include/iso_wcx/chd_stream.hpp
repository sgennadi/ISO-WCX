#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// chd_file is a C API opaque handle. libchdr and its compression codecs are
// linked statically into ISO-WCX; the installer ships no additional DLLs.
struct _chd_file;
typedef struct _chd_file chd_file;

namespace iso_wcx {

struct ChdAudioTrack {
    std::uint32_t number = 0;   // Physical one-based CHD CD track.
    std::uint64_t first_frame = 0; // Absolute 2448-byte CHD frame.
    std::uint64_t frames = 0;      // Only real sectors; excludes MAME padding.
};

class ChdStream final {
public:
    explicit ChdStream(const std::filesystem::path& path);
    ~ChdStream();
    ChdStream(const ChdStream&) = delete;
    ChdStream& operator=(const ChdStream&) = delete;

    [[nodiscard]] std::uint64_t size() const noexcept { return logical_size_; }
    [[nodiscard]] const std::string& label() const noexcept { return label_; }
    [[nodiscard]] const std::vector<ChdAudioTrack>& audio_tracks() const noexcept {
        return audio_tracks_;
    }
    // PCM is 2352 bytes/frame, 16-bit big-endian in standard CD CHD storage.
    // Convert samples to little-endian for WAVE; no raw subcode is emitted.
    [[nodiscard]] std::vector<std::uint8_t> read_audio(std::uint32_t track,
        std::uint64_t pcm_offset, std::size_t pcm_length);
    [[nodiscard]] std::vector<std::uint8_t> read(std::uint64_t start,
                                                 std::size_t length);
private:
    void load_hunk(std::uint64_t hunk_index);

    struct FileCloser {
        void operator()(std::FILE* f) const noexcept;
    };
    struct ChdCloser {
        void operator()(chd_file* chd) const noexcept;
    };
    std::unique_ptr<std::FILE, FileCloser> file_;
    std::unique_ptr<chd_file, ChdCloser> chd_;
    std::uint64_t logical_size_ = 0;    // Exposed ISO/UDF stream, 2048/sector.
    std::uint32_t hunk_bytes_ = 0;
    std::uint32_t unit_bytes_ = 0;      // 2048 (DVD) or 2448 (CD frame).
    std::uint32_t data_offset_ = 0;
    std::uint64_t leading_frames_ = 0; // Absolute CHD frame of selected data track, plus validated lead-in.
    // Guard libchdr decoding and the shared decompressed-hunk buffer.
    // Multiple Total Commander extraction threads may share one archive.
    mutable std::mutex read_mutex_;
    std::uint64_t cached_hunk_ = ~std::uint64_t(0);
    std::vector<std::uint8_t> hunk_;
    std::vector<ChdAudioTrack> audio_tracks_;
    std::string label_;
};

} // namespace iso_wcx
