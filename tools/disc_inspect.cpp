#include "iso_wcx/disc.hpp"
#include "iso_wcx/image_report.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc,char** argv) {
    if(argc!=2 && argc!=3 && argc!=4){std::cerr<<"Usage: disc_inspect image.iso [--extract OUTPUT_DIR]\n";return 1;}
    if(argc==3 && std::string(argv[2])!="--structure-report")return 1;
    if(argc==4 && std::string(argv[2])!="--extract")return 1;
    try {
        // This developer-only test entry point calls the same report library
        // embedded in ConfigurePacker; no EXE is shipped as a WCX dependency.
        if(argc==3) {
            std::cout<<iso_wcx::build_image_structure_report(
                std::filesystem::path(argv[1]));
            return 0;
        }
        iso_wcx::Disc disc{std::filesystem::path(argv[1])};
        std::cout<<"Format: "<<disc.format()<<"\n";
        for(std::size_t i=0;i<disc.items().size();i++) {
            const auto& entry=disc.items()[i];
            std::cout<<(entry.directory?"DIR ":"FILE ")<<entry.size<<" "<<entry.path<<"\n";
            if(argc==4) {
                auto path=std::filesystem::path(argv[3]) / std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(entry.path.c_str())));
                if(!disc.copy(i,path))throw std::runtime_error("extraction aborted");
            }
        }
    }catch(const std::exception& e){std::cerr<<"Disc error: "<<e.what()<<"\n";return 2;}
    return 0;
}
