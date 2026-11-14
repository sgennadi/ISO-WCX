#include "iso_wcx/settings_command.hpp"
#include <initializer_list>
#include <iostream>
#include <stdexcept>
namespace {
void check(bool condition,const char* description) {
    if(!condition)throw std::runtime_error(description);
}
}
int main() {
    using iso_wcx::settings_command::is_button_click;
    static_assert(is_button_click(0u,true));
    static_assert(!is_button_click(6u,true)); // BN_SETFOCUS
    static_assert(!is_button_click(7u,true)); // BN_KILLFOCUS
    static_assert(!is_button_click(5u,true)); // BN_DOUBLECLICKED
    static_assert(!is_button_click(0u,false)); // menu or synthetic WM_COMMAND
    try {
        check(is_button_click(0u,true),"button click ignored");
        for(const unsigned notification:{1u,2u,3u,4u,5u,6u,7u,8u}) {
            check(!is_button_click(notification,true),
                  "non-click button notification invoked an action");
        }
        check(!is_button_click(0u,false),
              "WM_COMMAND without our button handle invoked an action");
        std::cout<<"PASS: only BN_CLICKED from a real Configure action button "
                    "may trigger a side effect; focus never reopens reports\n";
        return 0;
    }catch(const std::exception& e){
        std::cerr<<"FAIL: "<<e.what()<<"\n";
        return 1;
    }
}
