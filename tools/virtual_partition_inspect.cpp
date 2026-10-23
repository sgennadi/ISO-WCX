// Read-only raw virtual hard-disk partition inspector.
// Standalone CLI; neither mounts disks nor changes the WCX image formats.
#include "iso_wcx/partition_map.hpp"
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

int main(int argc,char** argv) {
    if(argc!=2 && argc!=3) {
        std::cerr<<"Usage: virtual_partition_inspect IMAGE.img [512|4096]\n";
        return 2;
    }
    unsigned sector=512;
    if(argc==3) {
        const std::string size=argv[2];
        if(size!="512" && size!="4096") {
            std::cerr<<"Logical sector size must be 512 or 4096\n";
            return 2;
        }
        sector=size=="512"?512u:4096u;
    }
    try {
        const auto result=iso_wcx::inspect_partition_table(argv[1],sector);
        std::cout<<"Scheme\tFirstLBA\tSectors\tType\n";
        for(const auto& partition:result) {
            std::cout<<(partition.scheme==iso_wcx::PartitionTableKind::gpt?"GPT":"MBR")
                     <<'\t'<<partition.first_lba<<'\t'<<partition.sector_count<<'\t';
            if(partition.scheme==iso_wcx::PartitionTableKind::mbr) {
                std::cout<<"0x"<<std::hex<<std::setw(2)<<std::setfill('0')
                         <<static_cast<unsigned>(partition.mbr_type)<<std::dec;
            } else {
                for(const auto octet:partition.gpt_type)
                    std::cout<<std::hex<<std::setw(2)<<std::setfill('0')
                             <<static_cast<unsigned>(octet);
                std::cout<<std::dec;
            }
            std::cout<<'\n';
        }
    } catch(const std::exception& e) {
        std::cerr<<"Invalid or unsupported raw virtual disk: "<<e.what()<<"\n";
        return 1;
    }
    return 0;
}
