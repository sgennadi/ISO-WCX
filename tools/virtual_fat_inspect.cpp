// Read-only virtual FAT filesystem inspector. This tool never mounts disks,
// scans physical drives or constructs output paths from untrusted filenames.
#include "iso_wcx/fat_volume.hpp"
#include "iso_wcx/partition_map.hpp"
#include "iso_wcx/virtual_disk.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace {
std::size_t index_arg(const std::string& text) {
    std::size_t end = 0;
    const auto n = std::stoull(text, &end, 10);
    if (end != text.size() || n == 0 || n > 65535)
        throw std::runtime_error("partition number must be in range 1..65535");
    return static_cast<std::size_t>(n - 1);
}
std::uint32_t sector_arg(const std::string& text) {
    if (text == "512") return 512;
    if (text == "4096") return 4096;
    throw std::runtime_error("logical sector size must be 512 or 4096");
}
}

int main(int argc, char** argv) {
    if (argc < 4 || argc > 6) {
        std::cerr <<
            "Read-only FAT12/16/32 filesystem explorer\n"
            "Usage: virtual_fat_inspect list IMAGE PARTITION_NUMBER [512|4096]\n"
            "       virtual_fat_inspect cat  IMAGE PARTITION_NUMBER FAT_PATH [512|4096]\n"
            "The cat command streams binary bytes to stdout. Redirect it to a\n"
            "file of your choice; no host filesystem path is created implicitly.\n";
        return 2;
    }
    try {
        const std::string command = argv[1];
        const bool listing = command == "list";
        if (!listing && command != "cat")
            throw std::runtime_error("unknown FAT command (use list or cat)");
        if ((listing && (argc != 4 && argc != 5)) ||
            (!listing && (argc != 5 && argc != 6)))
            throw std::runtime_error("wrong argument count");
        const auto logical_sector = sector_arg(
            listing ? (argc == 5 ? argv[4] : "512") :
                      (argc == 6 ? argv[5] : "512"));
        const auto index = index_arg(argv[3]);
        const std::filesystem::path image(argv[2]);
        iso_wcx::VirtualDiskReader reader(image);
        const auto partitions = iso_wcx::inspect_partition_table(reader, logical_sector);
        if (index >= partitions.size())
            throw std::runtime_error("partition number is outside the discovered table");
        const auto& partition = partitions[index];
        const std::uint64_t start = partition.first_lba * logical_sector;
        const std::uint64_t bytes = partition.sector_count * logical_sector;
        iso_wcx::FatVolume volume(reader, start, bytes);
        if (listing) {
            for (const auto& item : volume.list())
                std::cout << (item.directory ? 'D' : 'F') << '\t' << item.size
                          << '\t' << item.path << '\n';
        } else {
#if defined(_WIN32)
            if (_setmode(_fileno(stdout), _O_BINARY) < 0)
                throw std::runtime_error("could not switch stdout to binary mode");
#endif
            volume.extract(argv[4], std::cout);
            std::cout.flush();
            if (!std::cout)
                throw std::runtime_error("could not flush extracted FAT data");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAT inspector: " << error.what() << '\n';
        return 1;
    }
}
