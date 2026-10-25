// Test-only: assert that target compilation uses VS2026 C++26 draft.
#ifndef _MSC_VER
#error MSVC is required for this test
#endif
#if _MSC_VER < 1950
#error MSVC 19.50 or newer is required
#endif
#ifndef _MSVC_LANG
#error MSVC language macro is absent
#endif
#if _MSVC_LANG < 202400L
#error C++ latest draft mode is disabled
#endif
#if __cplusplus < 202400L
#error C++ latest draft macro was not enabled
#endif
int main() { return 0; }
