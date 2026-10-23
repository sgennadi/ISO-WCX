#include "iso_wcx/legacy_formats.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>

namespace {
void expect(bool value,const char* what) {
    if(!value)throw std::runtime_error(what);
}
constexpr std::uint32_t pack(unsigned year,unsigned month,unsigned day,
                             unsigned hour,unsigned min,unsigned sec) {
    return ((year-1980)<<25)|(month<<21)|(day<<16)|(hour<<11)|(min<<5)|(sec/2);
}
}
int main() {
    try {
        std::array<std::uint8_t,7> iso{126,10,23,12,34,58,0};
        expect(iso_wcx::iso_dos_timestamp(iso)==pack(2026,10,23,12,34,58),
               "ISO directory record timestamp not packed correctly");
        iso[1]=2;iso[2]=30;
        expect(iso_wcx::iso_dos_timestamp(iso)==0,"invalid calendar date not rejected");
        iso[0]=70;iso[1]=1;iso[2]=1;
        expect(iso_wcx::iso_dos_timestamp(iso)==0,"DOS date before 1980 not rejected");
        std::array<std::uint8_t,12> udf{};
        udf[2]=0xea;udf[3]=0x07; // 2026 little endian
        udf[4]=10;udf[5]=23;udf[6]=12;udf[7]=34;udf[8]=58;
        expect(iso_wcx::udf_dos_timestamp(udf)==pack(2026,10,23,12,34,58),
               "ECMA-167 UDF timestamp not packed correctly");
        udf[8]=65;
        expect(iso_wcx::udf_dos_timestamp(udf)==0,"invalid UDF second accepted");
        constexpr std::uint64_t windows_epoch=116444736000000000ULL;
        using namespace std::chrono;
        const auto clock_time=sys_seconds{sys_days{year{2026}/10/23}}+hours{12}+minutes{34}+seconds{58};
        const auto unix_seconds=clock_time.time_since_epoch().count();
        const auto ticks=windows_epoch+static_cast<std::uint64_t>(unix_seconds)*10000000ULL;
        expect(iso_wcx::xbox_filetime_dos(ticks)==pack(2026,10,23,12,34,58),
               "XDVDFS volume creation FILETIME decode error");
        expect(iso_wcx::xbox_filetime_dos(0)==0,"invalid Xbox FILETIME not rejected");
        std::cout<<"PASS: ISO, UDF and Xbox date metadata\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr<<"metadata test: "<<e.what()<<"\n";
        return 1;
    }
}
