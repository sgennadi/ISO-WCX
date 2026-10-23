// Metadata-only MDS/MDF and MDX diagnostic utility.
// Identifies on-disc layout but does not decrypt, unpack, mount or extract.
#include "iso_wcx/mds_probe.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    if(argc!=2) {
        std::cerr<<"Usage: mds_descriptor_inspect <image.mds|image.mdx>\n"
                 <<"This is a read-only header probe; MDSv2/MDX extraction is NOT implemented.\n";
        return 2;
    }
    try {
        const auto info=iso_wcx::probe_mds_descriptor(
            std::filesystem::path(argv[1]));
        const char* label="Alcohol MDS v1";
        switch(info.kind) {
        case iso_wcx::MdsDescriptorKind::alcohol_v1: break;
        case iso_wcx::MdsDescriptorKind::daemon_mds_v2:
            label="DAEMON Tools MDS v2";break;
        case iso_wcx::MdsDescriptorKind::daemon_mdx:
            label="DAEMON Tools MDX (embedded MDS v2)";break;
        }
        std::cout<<"Format: "<<label
                 <<"\nVersion: "<<unsigned(info.major)<<'.'<<unsigned(info.minor)
                 <<"\nContainer bytes: "<<info.file_bytes<<'\n';
        if(info.kind==iso_wcx::MdsDescriptorKind::alcohol_v1) {
            std::cout<<"Support: legacy descriptor and uncompressed data tracks\n";
        } else {
            std::cout<<"Encrypted header offset: "<<info.encrypted_header_offset
                     <<"\nPacked metadata extent: "<<info.packed_metadata_offset
                     <<" + "<<info.packed_metadata_length
                     <<"\nSupport: metadata layout recognized only; "
                       "decryption and CD-track extraction not implemented\n";
        }
        return 0;
    } catch(const std::exception& e) {
        std::cerr<<"MDS/MDX probe: "<<e.what()<<'\n';
        return 2;
    }
}
