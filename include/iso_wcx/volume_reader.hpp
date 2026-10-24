#pragma once
#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

namespace iso_wcx {
// Common read-only filesystem interface. Never resolves host paths or writes
// to a disk image. All paths must first be discovered by list().
struct VolumeFile {
    std::string path;
    std::uint64_t size = 0;
    std::uint32_t first_cluster = 0;
    bool directory = false;
    std::uint32_t dos_time = 0;
    std::uint32_t attributes = 0;
};
class VolumeReader {
public:
    virtual ~VolumeReader() = default;
    virtual std::vector<VolumeFile> list() = 0;
    virtual void extract(std::string_view path, std::ostream& output) = 0;
};
} // namespace iso_wcx
