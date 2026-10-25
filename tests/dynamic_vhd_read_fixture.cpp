// Read logical bytes from a native dynamic VHD without mounting the disk.
// Test utility only; release WCX does not ship this executable.
#include "iso_wcx/virtual_disk.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc,char** argv) {
    if(argc!=4) {
        std::cerr<<"Usage: dynamic_vhd_read_fixture IMAGE.vhd OFFSET LENGTH\n";
        return 2;
    }
    try {
        std::size_t end=0;
        const auto offset=std::stoull(argv[2],&end,10);
        if(end!=std::string(argv[2]).size())
            throw std::runtime_error("invalid offset");
        end=0;
        const auto size=std::stoull(argv[3],&end,10);
        if(end!=std::string(argv[3]).size() ||
           size>4u*1024u*1024u ||
           size>std::numeric_limits<std::size_t>::max())
            throw std::runtime_error("invalid bounded read length");
        iso_wcx::VirtualDiskReader disk{std::filesystem::path(argv[1])};
        if(disk.format()!=iso_wcx::VirtualDiskFormat::dynamic_vhd)
            throw std::runtime_error("fixture requires a dynamic VHD");
        const auto bytes=disk.read(offset,static_cast<std::size_t>(size));
#ifdef _WIN32
        if(_setmode(_fileno(stdout),_O_BINARY)<0)
            throw std::runtime_error("binary stdout setup failed");
#endif
        std::cout.write(reinterpret_cast<const char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size()));
        std::cout.flush();
        if(!std::cout)throw std::runtime_error("cannot write VHD readback");
        return 0;
    }catch(const std::exception& e) {
        std::cerr<<"Dynamic VHD fixture: "<<e.what()<<'\n';
        return 1;
    }
}
