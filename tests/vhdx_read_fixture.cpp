// Test-only bounded logical VHDX/raw/VHD sector reader.
// No mounts or external libraries. Nonzero exit status means comparison failed.
#include "iso_wcx/virtual_disk.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc,char** argv) {
    if(argc!=5) {
        std::cerr<<"Usage: vhdx_read_fixture IMAGE OFFSET LENGTH EXPECT_BYTE_0_TO_255\n";
        return 2;
    }
    try {
        const auto parse=[](const char* value)->std::uint64_t {
            std::size_t end=0;
            const std::string text(value);
            const auto out=std::stoull(text,&end,10);
            if(end!=text.size())throw std::runtime_error("not a decimal integer");
            return out;
        };
        const auto start=parse(argv[2]);
        const auto length=parse(argv[3]);
        const auto expected=parse(argv[4]);
        if(length>4u*1024u*1024u || expected>255)
            throw std::runtime_error("length / expected byte is out of range");
        iso_wcx::VirtualDiskReader disk{std::filesystem::path(argv[1])};
        const auto bytes=disk.read(start,static_cast<std::size_t>(length));
        if(!std::all_of(bytes.begin(),bytes.end(),
            [&](std::uint8_t byte){return byte==expected;}))
            throw std::runtime_error("logical byte content mismatch");
        return 0;
    }catch(const std::exception& error) {
        std::cerr<<"VHDX read test: "<<error.what()<<"\n";
        return 1;
    }
}
