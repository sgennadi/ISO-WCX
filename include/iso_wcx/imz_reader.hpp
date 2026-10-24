#pragma once
#include <cstdint>
#include <fstream>
#include <vector>
namespace iso_wcx {
// WinImage IMZ is a single raw floppy image wrapped in ZIP.
// Reads one stored/deflated member, with bounded output and verified CRC.
std::vector<std::uint8_t> decode_imz(std::ifstream& input, std::uint64_t length);
}
