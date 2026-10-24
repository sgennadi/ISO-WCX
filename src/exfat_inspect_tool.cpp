#include "iso_wcx/exfat_volume.hpp"
#include "iso_wcx/virtual_disk.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc,char** argv) {
    if(argc<3 || argc>4) {
        std::cerr<<"Usage: exfat_inspect IMAGE list | IMAGE cat PATH\n";
        return 2;
    }
    try {
        iso_wcx::VirtualDiskReader disk{std::filesystem::path(argv[1])};
        iso_wcx::ExfatVolume volume(disk,0,disk.size());
        const std::string operation=argv[2];
        if(operation=="list" && argc==3) {
            for(const auto& f:volume.list())
                std::cout<<(f.directory?'D':'F')<<'\t'<<f.size
                         <<'\t'<<f.path<<'\n';
        } else if(operation=="cat" && argc==4) {
#if defined(_WIN32)
            if(_setmode(_fileno(stdout),_O_BINARY)<0)
                throw std::runtime_error("cannot enable binary stdout");
#endif
            volume.extract(argv[3],std::cout);
            std::cout.flush();
            if(!std::cout)throw std::runtime_error("output stream error");
        } else return 2;
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<"exFAT inspector: "<<error.what()<<'\n';
        return 1;
    }
}
