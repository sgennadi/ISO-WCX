#include "iso_wcx/universal_diagnostics.hpp"
#include <cassert>
#include <iostream>
#include <string>

int main() {
    using namespace iso_wcx::universal_diagnostics;
    for(const auto area : {Category::iso_write, Category::wim_write,
                           Category::udf_edit, Category::mount_commit,
                           Category::read_extract, Category::other}) {
        const auto json=payload(area,0,21,0);
        assert(json.find("\"project\":\"sgennadi/ISO-WCX\"")!=std::string::npos);
        assert(json.find("\"version\":\"0.21.0\"")!=std::string::npos);
        assert(json.find("\"component\":\"")!=std::string::npos);
        assert(json.find("\"error_type\":\"USER_REPORT\"")!=std::string::npos);
        assert(json.find("\"code\":\"MANUAL\"")!=std::string::npos);
        assert(json.find("\"level\":\"error\"")!=std::string::npos);
        assert(json.find("excerpt")==std::string::npos);
        for(const char* forbidden : {"password","token","user-name","\\\\",
                                     "C:\\Users", "WimFile", "bitlocker"}) {
            assert(json.find(forbidden)==std::string::npos);
        }
    }
    assert(std::string(component(Category::other))=="ISO-WCX Settings");
    std::cout << "PASS ISO-WCX UniversalDiagnostics privacy contract 0.21.0\n";
    return 0;
}
