#pragma once
#include <cstdint>
#include <string_view>

namespace iso_wcx::tc_association {
constexpr unsigned cap_new=1, cap_modify=2, cap_multiple=4,
                   cap_delete=8, cap_options=16, cap_by_content=64,
                   cap_searchtext=128;
constexpr unsigned reported_caps=cap_modify|cap_multiple|cap_delete|
                                 cap_options|cap_by_content|cap_searchtext; // 222

// wincmd.ini entries are a decimal capability mask followed by a comma
// and the path to the WCX. Invalid or empty masks are never writable.
constexpr unsigned flags(std::wstring_view entry) noexcept {
    const auto delimiter=entry.find(L',');
    if(delimiter==std::wstring_view::npos||delimiter==0)return 0;
    unsigned mask=0;
    for(std::size_t i=0;i<delimiter;++i){
        const wchar_t c=entry[i];
        if(c<L'0'||c>L'9')return 0;
        if(mask>100000u)return 0;
        mask=mask*10+static_cast<unsigned>(c-L'0');
    }
    return mask;
}
constexpr bool can_edit(unsigned mask) noexcept {
    return (mask&(cap_modify|cap_delete))==(cap_modify|cap_delete);
}
constexpr bool can_create(unsigned mask) noexcept {
    return (mask&cap_new)!=0;
}
constexpr bool can_modify(unsigned mask) noexcept {
    return (mask&cap_modify)!=0;
}
constexpr bool can_delete(unsigned mask) noexcept {
    return (mask&cap_delete)!=0;
}
} // namespace iso_wcx::tc_association
