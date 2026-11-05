#include "iso_wcx/universal_diagnostics.hpp"
#include <stdexcept>
#include <iostream>
#include <string>

static void require(bool ok, const char* what) {
    if(!ok)throw std::runtime_error(what);
}
int main() {
    using namespace iso_wcx::universal_diagnostics;
    for(const auto area : {Category::iso_write, Category::wim_write,
                           Category::udf_edit, Category::mount_commit,
                           Category::read_extract, Category::other}) {
        const auto json=payload(area,0,21,0);
        require(json.find("\"project\":\"sgennadi/ISO-WCX\"")!=std::string::npos);
        require(json.find("\"version\":\"0.21.0\"")!=std::string::npos);
        require(json.find("\"component\":\"")!=std::string::npos);
        require(json.find("\"error_type\":\"USER_REPORT\"")!=std::string::npos);
        require(json.find("\"code\":\"MANUAL\"")!=std::string::npos);
        require(json.find("\"level\":\"error\"")!=std::string::npos);
        require(json.find("excerpt")==std::string::npos);
        for(const char* forbidden : {"password","token","user-name","\\\\",
                                     "C:\\Users", "WimFile", "bitlocker"}) {
            require(json.find(forbidden)==std::string::npos);
        }
    }
    require(std::string(component(Category::other))=="ISO-WCX Settings");
    std::cout << "PASS ISO-WCX UniversalDiagnostics privacy contract 0.21.0\n";
    return 0;
}
