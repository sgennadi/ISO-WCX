#pragma once
// UI event safety guard: a focus notification is not a button activation.
// BS_NOTIFY sends BN_SETFOCUS/BN_KILLFOCUS for keyboard/high-DPI scrolling.
// This pure helper is shared by ISO-WCX Configure and a host-independent
// regression test; Win32's BN_CLICKED is 0.
namespace iso_wcx::settings_command {
[[nodiscard]] constexpr bool is_button_click(
        unsigned notification, bool from_own_button) noexcept {
    return notification==0u && from_own_button;
}
} // namespace iso_wcx::settings_command
