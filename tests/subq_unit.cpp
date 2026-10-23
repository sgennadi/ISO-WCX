#include "iso_wcx/subq.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs=std::filesystem;
using iso_wcx::SubQLayout;
using Q=std::array<std::uint8_t,12>;
using Packet=std::array<std::uint8_t,96>;
void insist(bool ok,const char* why) {if(!ok)throw std::runtime_error(why);}

int main() {
    try {
        // Independent published example with CRC 0x2832:
        // 41 01 01 00 00 00 00 00 02 00 28 32.
        const Q known{0x41,0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x02,0x00,0x28,0x32};
        insist(iso_wcx::subq_crc_valid(known),"known Q CRC example rejected");
        Q bad=known; bad[8]^=1;
        insist(!iso_wcx::subq_crc_valid(bad),"corrupted Q CRC accepted");
        const auto decoded=iso_wcx::decode_subq_position(known);
        insist(decoded && decoded->track==1 && decoded->index==1 && decoded->absolute_frame==150,
               "incorrect BCD/MSF position decoding");
        Q invalid_bcd=known;invalid_bcd[2]=0x1a;
        insist(!iso_wcx::decode_subq_position(invalid_bcd),"invalid index BCD accepted");
        Packet direct{};
        std::copy(known.begin(),known.end(),direct.begin()+12);
        insist(iso_wcx::extract_subq(direct,SubQLayout::deinterleaved)==known,
               "deinterleaved Q extraction failed");
        Packet interleaved{};
        for(std::size_t i=0;i<96;++i)
            interleaved[i]=((known[i/8]>>(7-i%8))&1u)?0x40:0;
        insist(iso_wcx::extract_subq(interleaved,SubQLayout::interleaved)==known,
               "interleaved Q extraction failed");
        const auto path=fs::temp_directory_path()/
            ("iso-wcx-subq-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".sub");
        {std::ofstream out(path,std::ios::binary);
         out.write(reinterpret_cast<const char*>(direct.data()),direct.size());
         out.write(reinterpret_cast<const char*>(interleaved.data()),interleaved.size());
         Packet garbage{};
         out.write(reinterpret_cast<const char*>(garbage.data()),garbage.size());}
        const auto report=iso_wcx::subq_index_report(path);
        fs::remove(path);
        insist(report.find("# crc_valid,2\n")!=std::string::npos,"wrong valid CRC counter");
        insist(report.find("# crc_failed,1\n")!=std::string::npos,"wrong invalid CRC counter");
        insist(report.find("deinterleaved,1,1,0,0,1")!=std::string::npos,
               "missing deinterleaved position range");
        insist(report.find("interleaved,1,1,1,1,1")!=std::string::npos,
               "missing interleaved position range");
        std::cout << "PASS: CloneCD Q CRC, BCD/MSF, both P-W layouts, invalid frames and CSV\n";
    } catch(const std::exception& e) {
        std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;
    }
    return 0;
}
