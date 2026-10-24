#pragma once

#include <cstdint>
#include <vector>

namespace iso_wcx {
// Standard Red Book CDDA PCM: stereo, 44.1 kHz, signed 16-bit little-endian.
// Emit RIFF/WAVE for tracks under the RIFF 32-bit limit and RF64 with ds64
// for larger streams. The WCX extractor writes audio in bounded pieces.
[[nodiscard]] std::vector<std::uint8_t> cdda_wave_header(std::uint64_t pcm_bytes);
[[nodiscard]] std::uint64_t cdda_wave_file_length(std::uint64_t cd_frames);

} // namespace iso_wcx
