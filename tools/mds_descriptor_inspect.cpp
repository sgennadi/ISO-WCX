// MDS v2 and MDX descriptor diagnostic utility.
// Opt-in --decode decrypts/inflates metadata only; no data tracks are extracted.
#include "iso_wcx/mds_probe.hpp"
#include "iso_wcx/mds_v2.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    if(argc!=2 && argc!=3) {
        std::cerr<<"Usage: mds_descriptor_inspect <image.mds|image.mdx> [--decode]\n"
                 <<"--decode: decrypt MDSv2/MDX descriptor only (no sectors).\n";
        return 2;
    }
    try {
        if (argc == 3 && std::string(argv[2]) != "--decode")
            throw std::runtime_error("unexpected argument (use --decode)");
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
                       "CD-track extraction not implemented\n";
            if(argc==3) {
                const auto decoded=iso_wcx::decode_mds_v2_descriptor(argv[1]);
                const unsigned sessions=static_cast<unsigned>(decoded[0x14]) |
                    (static_cast<unsigned>(decoded[0x15])<<8);
                std::cout << "Decoded metadata bytes: " << decoded.size()
                          << "\nSessions: " << sessions << "\n";
            }
        }
        return 0;
    } catch(const std::exception& e) {
        std::cerr<<"MDS/MDX probe: "<<e.what()<<'\n';
        return 2;
    }
}
