// Cross-platform read-only report regression, exercising the exact library
// that ConfigurePacker uses on Windows. No Total Commander or helper EXE.
#include "iso_wcx/image_report.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs=std::filesystem;

void need(bool ok,const char* reason) {
    if(!ok)throw std::runtime_error(reason);
}
void le32(std::vector<std::uint8_t>& b,std::size_t at,std::uint32_t n) {
    for(unsigned i=0;i<4;++i)b[at+i]=static_cast<std::uint8_t>(n>>(i*8));
}
std::vector<std::uint8_t> rec(std::uint32_t lba,std::uint32_t size,
                              std::uint8_t flags,const std::string& name) {
    const auto length=33+name.size()+(name.size()%2==0?1:0);
    std::vector<std::uint8_t> out(length,0);
    out[0]=static_cast<std::uint8_t>(length);
    le32(out,2,lba);le32(out,10,size);
    for(unsigned i=0;i<4;++i) {
        out[6+i]=static_cast<std::uint8_t>(lba>>(8*(3-i)));
        out[14+i]=static_cast<std::uint8_t>(size>>(8*(3-i)));
    }
    out[18]=126;out[19]=10;out[20]=23;
    out[21]=12;out[22]=34;out[23]=58;
    out[25]=flags;
    out[28]=1;out[31]=1;
    out[32]=static_cast<std::uint8_t>(name.size());
    std::memcpy(out.data()+33,name.data(),name.size());
    return out;
}
int main() {
    fs::path directory;
    try {
        constexpr std::size_t sector=2048;
        std::vector<std::uint8_t> bytes(32*sector,0);
        bytes[0]=0xeb; // Nonzero boot/system area (no actual payload export).
        const auto pvd=16*sector;
        bytes[pvd]=1;std::memcpy(bytes.data()+pvd+1,"CD001",5);
        bytes[pvd+6]=1;
        std::memcpy(bytes.data()+pvd+40,"SAFE_ISO9660",12);
        le32(bytes,pvd+80,32);
        bytes[pvd+128]=0;bytes[pvd+129]=8;
        const auto root=rec(20,sector,2,std::string(1,'\0'));
        std::copy(root.begin(),root.end(),bytes.begin()+static_cast<std::ptrdiff_t>(pvd+156));
        auto boot=17*sector;
        bytes[boot]=0;std::memcpy(bytes.data()+boot+1,"CD001",5);
        std::memcpy(bytes.data()+boot+7,"EL TORITO SPECIFICATION",23);
        bytes[boot+6]=1;le32(bytes,boot+71,25);
        const auto svd=18*sector;
        bytes[svd]=2;std::memcpy(bytes.data()+svd+1,"CD001",5);
        bytes[svd+6]=1;
        bytes[svd+88]='%';bytes[svd+89]='/';bytes[svd+90]='E';
        const auto joliet_root=rec(22,sector,2,std::string(1,'\0'));
        std::copy(joliet_root.begin(),joliet_root.end(),
                  bytes.begin()+static_cast<std::ptrdiff_t>(svd+156));
        const auto end=19*sector;
        bytes[end]=255;std::memcpy(bytes.data()+end+1,"CD001",5);
        bytes[end+6]=1;
        const auto catalog=25*sector;
        bytes[catalog]=1;bytes[catalog+30]=0x55;bytes[catalog+31]=0xaa;
        auto dotdot=rec(20,sector,2,std::string(1,'\1'));
        auto file=rec(21,5,0,"HELLO.TXT;1");
        std::size_t pos=20*sector;
        for(const auto& record:{root,dotdot,file}) {
            std::copy(record.begin(),record.end(),
                      bytes.begin()+static_cast<std::ptrdiff_t>(pos));
            pos+=record.size();
        }
        // A second, UTF-16BE Joliet tree matches real bootable images
        // with independent PVD and Joliet directory records.
        const auto unicode_name=[](std::u16string_view value) {
            std::string encoded;
            for(const auto codepoint:value) {
                encoded.push_back(static_cast<char>(codepoint>>8));
                encoded.push_back(static_cast<char>(codepoint&0xff));
            }
            return encoded;
        };
        const auto joliet_parent=rec(22,sector,2,std::string(1,'\1'));
        const auto joliet_hello=rec(21,5,0,unicode_name(u"HELLO.TXT;1"));
        const auto joliet_hebrew=rec(21,5,0,unicode_name(u"\u05E9\u05DC\u05D5\u05DD.TXT;1"));
        pos=22*sector;
        for(const auto& record:{joliet_root,joliet_parent,joliet_hello,joliet_hebrew}) {
            std::copy(record.begin(),record.end(),
                      bytes.begin()+static_cast<std::ptrdiff_t>(pos));
            pos+=record.size();
        }
        std::memcpy(bytes.data()+21*sector,"world",5);
        const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
        directory=fs::temp_directory_path()/("iso-wcx-report-"+std::to_string(stamp));
        fs::create_directories(directory);
        const auto image=directory/fs::path(std::u8string(u8"test-\u05e9\u05dc\u05d5\u05dd.iso"));
        {
            std::ofstream out(image,std::ios::binary);
            need(bool(out),"Cannot create test image");
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        }
        const auto report=iso_wcx::build_image_structure_report(image);
        need(report.find("IMAGE STRUCTURE REPORT")!=std::string::npos,"Report header missing");
        const auto utf8=u8"test-\u05e9\u05dc\u05d5\u05dd.iso";
        const std::u8string utf8_name(utf8);
        need(report.find(std::string(utf8_name.begin(),utf8_name.end()))!=std::string::npos,
             "Unicode filename lost");
        need(report.find("Detected filesystem / container:")!=std::string::npos,
             "Detected format missing");
        need(report.find("PVD Volume ID: SAFE_ISO9660")!=std::string::npos,
             "PVD label missing");
        need(report.find("System area (first 32 KiB) nonzero: yes")!=std::string::npos,
             "System area not detected");
        need(report.find("El Torito boot descriptor: yes")!=std::string::npos,
             "El Torito not detected");
        need(report.find("Joliet present: yes")!=std::string::npos,
             "Bootable ISO Joliet descriptor not detected");
        const std::u8string expected_hebrew=u8"\u05E9\u05DC\u05D5\u05DD.TXT";
        need(report.find(std::string(expected_hebrew.begin(),expected_hebrew.end()))!=
                 std::string::npos,"Joliet Unicode file path missing");
        need(report.find("UDF VRS NSR02: not detected")!=std::string::npos,
             "Invalid UDF reporting");
        need(report.find("FILE\t5\t")!=std::string::npos &&
             report.find("\tHELLO.TXT\n")!=std::string::npos,
             "Complete file tree missing");
        need(report.find("Image modified: no")!=std::string::npos,
             "Read-only disclaimer missing");
        need(report.size()<100000,"Unexpected oversized report");
        {
            std::ifstream in(image,std::ios::binary);
            const std::vector<std::uint8_t> unchanged(
                (std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>{});
            need(unchanged==bytes,"Image changed during metadata report");
        }
        fs::remove_all(directory);
        std::cout<<"PASS: read-only UTF-8 ISO image structure, boot and tree metadata\n";
        return 0;
    }catch(const std::exception& e) {
        std::cerr<<"Image structure export regression: "<<e.what()<<'\n';
        if(!directory.empty()) {
            std::error_code ignored;
            fs::remove_all(directory,ignored);
        }
        return 1;
    }
}
