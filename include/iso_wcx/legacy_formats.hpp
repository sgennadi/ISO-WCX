#pragma once
#include "iso_wcx/disc.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace iso_wcx {
using SectorReader = std::function<std::vector<std::uint8_t>(std::uint64_t,std::size_t)>;
// El Torito files are exposed as virtual, read-only files under boot.images/.
// The no-emulation length is the advertised *boot load* length, not the
// guaranteed size of an entire EFI partition. Never invent the missing bytes.
std::vector<Item> el_torito_images(std::span<const std::uint8_t> boot_record,
                                   std::uint64_t image_bytes,
                                   const SectorReader& reader,
                                   std::uint32_t absolute_session_start = 0);
// A reader returning zero or more XDVDFS files; a missing Xbox signature
// yields an empty catalog. All extents are logical 2048-byte offsets.
std::vector<Item> xbox_xdvdfs(std::uint64_t image_bytes, const SectorReader& reader);
// DOS file time is expressed without timezone information by the WCX ABI.
std::uint32_t iso_dos_timestamp(std::span<const std::uint8_t> seven_bytes);
// ISO 9660 17-byte ASCII date format; also used by RRIP TF long timestamps.
std::uint32_t iso_long_dos_timestamp(std::span<const std::uint8_t> seventeen_bytes);
std::uint32_t udf_dos_timestamp(std::span<const std::uint8_t> twelve_bytes);
std::uint32_t xbox_filetime_dos(std::uint64_t filetime);
} // namespace iso_wcx
