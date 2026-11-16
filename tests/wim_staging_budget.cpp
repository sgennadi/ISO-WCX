#include "iso_wcx/wim_staging_budget.hpp"
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace iso_wcx::wim_detail;

static void require(bool yes,const char* message){
    if(!yes)throw std::runtime_error(message);
}
static_assert(wim_staging_budget(0,128u*wim_mib).enough);
static_assert(!wim_staging_budget(0,128u*wim_mib-1u).enough);
static_assert(wim_staging_budget(8u*wim_gib,9u*wim_gib).enough);
static_assert(!wim_staging_budget(8u*wim_gib,9u*wim_gib-1u).enough);
static_assert(wim_staging_budget(std::numeric_limits<std::uint64_t>::max(),
                                  std::numeric_limits<std::uint64_t>::max())
              .arithmetic_overflow);
static_assert(wim_mib_ceil(wim_mib+1u)==2u);

int main(){
    try {
        // Minimum reserve for small WIMs is 128 MiB. This is a floor
        // for staging, not a promise the expanded Apply tree will fit.
        auto small=wim_staging_budget(1u*wim_mib,129u*wim_mib);
        require(small.enough && small.reserve_bytes==128u*wim_mib &&
                small.required_bytes==129u*wim_mib,
                "small WIM reserve boundary is incorrect");
        require(!wim_staging_budget(1u*wim_mib,129u*wim_mib-1u).enough,
                "just-under-boundary capacity unexpectedly accepted");

        // For 4 GiB archives reserve 512 MiB (12.5 percent).
        auto medium=wim_staging_budget(4u*wim_gib,4u*wim_gib+512u*wim_mib);
        require(medium.enough && medium.reserve_bytes==512u*wim_mib,
                "proportional WIM reserve is incorrect");
        require(!wim_staging_budget(4u*wim_gib,
            4u*wim_gib+512u*wim_mib-1u).enough,
                "proportional WIM reserve has an off-by-one");

        // Maximum reserve of one GiB, never unbounded image-size math.
        auto large=wim_staging_budget(12u*wim_gib,13u*wim_gib);
        require(large.enough && large.reserve_bytes==wim_gib,
                "max WIM reserve is incorrect");

        constexpr auto maximum=std::numeric_limits<std::uint64_t>::max();
        const auto overflow=wim_staging_budget(maximum,maximum);
        require(overflow.arithmetic_overflow && !overflow.enough,
                "overflow was not safely rejected");
        require(wim_mib_ceil(0)==0 && wim_mib_ceil(1)==1 &&
                wim_mib_ceil(wim_mib)==1 &&
                wim_mib_ceil(maximum)==maximum/wim_mib+1,
                "rounded MiB conversion failed at boundary");
        std::cout<<"PASS: non-mutating WIM staging capacity policy and overflow\n";
    }catch(const std::exception& error){
        std::cerr<<"FAIL WIM staging budget: "<<error.what()<<"\n";
        return 1;
    }
    return 0;
}
