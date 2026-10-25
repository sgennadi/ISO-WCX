# ISO-WCX modern build toolchains

## Stable v0.20.0 and Development Preview

- Windows runner: `windows-2025`, GitHub-hosted Visual Studio 2026 (v18).
- CMake generator: `Visual Studio 18 2026`, platform `Win32` or `x64`.
- CMake option: `-DISO_WCX_CXX_STANDARD=26`.
- MSVC 19.50 or newer (`_MSC_VER >= 1950`) and `/std:c++latest` /
  `/Zc:__cplusplus`; both language macros must indicate the post-C++23
  working draft (`_MSVC_LANG >= 202400L`).
- Every Preview WCX DLL must pass its full CTest suite, independent
  FAT/UDF/WIM/optical fixture tests and Windows ABI smoke tests before
  its installable ZIP is uploaded.
- Parallel Linux GCC 14 and Clang 19 C++26 suites run via Ninja.
- Separate `windows-2025` C++20 Win32/x64 baselines ensure the application
  remains source-compatible with C++20 and exposes compiler regressions.

This selects MSVC's **current draft C++26 feature set**, not a complete
or finalized ISO C++26 implementation. Microsoft describes the mode as
`/std:c++latest` and cautions that its experimental features may change.

## Stable release reproducibility

Published **v0.19.0** and its SHA-256 manifest remain immutable. The
v0.20.0 Stable workflow uses the same validated VS2026/C++26 draft configuration
as the Preview for **both Win32 and x64**. It also requires Win32/x64
C++20 compatibility, GCC14/Clang19 C++26, Linux C++20, Windows WCX ABI,
QEMU VHDX, WIM/ESD, UDF, CHD, optical and sanitizer regressions.
The ZIP is packaged only from tested WCX DLL artifacts; SHA256SUMS.txt
is generated and checked before publication. Existing releases and tags
must never be overwritten.

## Local Visual Studio 2026 build

`cmake -S . -B build-vs2026 -G "Visual Studio 18 2026" -A x64 -DISO_WCX_CXX_STANDARD=26`

`cmake --build build-vs2026 --config Release --parallel 2`

`ctest --test-dir build-vs2026 -C Release --output-on-failure`

Use `-A Win32` for the Total Commander 32-bit plugin.
Tests and helper executables are developer-only, not plugin dependencies.
