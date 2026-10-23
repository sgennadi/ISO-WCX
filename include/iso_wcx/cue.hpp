#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <cstddef>
#include <vector>

namespace iso_wcx {
// A CUE describes a disc track, not a file archive by itself. These descriptors
// intentionally include only data-bearing BINARY tracks whose sector layouts
// can be decoded without an audio codec.
struct CueTrack {
    std::filesystem::path image;
    std::uint32_t start_frame{};
    std::optional<std::uint32_t> end_frame;  // Exclusive file-relative frame
    std::uint32_t physical_sector{};
    std::uint32_t data_offset{};
    std::uint8_t mode{};
    unsigned number{};
};

// CD-DA BINARY tracks are exposed as virtual RIFF/WAVE PCM files. This is
// intentionally limited to 2352-byte frames; compressed CUE audio sources
// (WAVE/MP3/FLAC) require a separate codec and are not decoded here.
struct CueAudioTrack {
    std::filesystem::path image;
    std::uint64_t start_byte{};
    std::uint64_t pcm_bytes{};
    unsigned number{};
};

[[nodiscard]] std::vector<CueAudioTrack> cue_audio_tracks(const std::filesystem::path& cue);

[[nodiscard]] bool is_cue_path(const std::filesystem::path& path);
// Rejects absolute, escaping and symlink-out-of-directory FILE references.
// Throws if CUE syntax, track indexes or file references are malformed.
[[nodiscard]] std::vector<CueTrack> cue_data_tracks(const std::filesystem::path& cue);
}
