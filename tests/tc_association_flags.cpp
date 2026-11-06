#include "iso_wcx/tc_association_flags.hpp"
#include <cassert>
#include <string_view>

int main() {
    using namespace iso_wcx::tc_association;
    static_assert(reported_caps==222);
    static_assert(!can_edit(flags(L"192,C:\\TotalCmd\\plugins\\wcx\\ISO-WCX\\ISO-WCX.wcx")));
    static_assert(can_edit(flags(L"158,C:\\tc\\x.wcx")));
    static_assert(can_edit(flags(L"159,C:\\tc\\x.wcx")));
    static_assert(can_edit(flags(L"222,C:\\tc\\x.wcx")));
    static_assert(!can_create(flags(L"158,C:\\tc\\x.wcx")));
    static_assert(can_create(flags(L"159,C:\\tc\\x.wcx")));
    static_assert(!can_create(reported_caps));
    static_assert(!can_edit(flags(L"64,C:\\tc\\x.wcx")));
    static_assert(!can_edit(flags(L"invalid,C:\\tc\\x.wcx")));
    static_assert(!can_edit(flags(L"C:\\tc\\x.wcx")));
    static_assert(!can_edit(flags(L"158")));
    static_assert(!can_edit(flags(L"999999999999,C:\\tc\\x.wcx")));
    static_assert(!can_edit(flags(L"")));
    return 0;
}
