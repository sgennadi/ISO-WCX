# ISO-WCX v0.20.0 — Visual Studio 2026 C++26 and fixed VHDX FAT Preview

Native Total Commander plugin: `ISO-WCX.wcx` (Win32) and `ISO-WCX.wcx64` (x64).
Stable binaries are built using **Visual Studio 2026 / MSVC 19.50+** with
experimental **C++26 draft flags** `/std:c++latest /Zc:__cplusplus`.
The generated Windows targets compile and run a dialect probe; CMake uses C++20
feature metadata only because its MSVC generator has no dedicated CXX26 mapping.
Full language-standard conformance is not implied.

## Changes since v0.19.0

- **Bounded fixed VHDX FAT editing (Preview)**: native add/replace/delete
  files and directories in selected `partitionN/` FAT12/16/32 volumes
  of **fully allocated fixed VHDX**, 512-byte logical sector only.
  Validates both CRC32C headers, clean log, complete and uniquely owned
  BAT physical block allocation, metadata/optional-region overlaps and FAT.
  Writes to a private physical clone only; compares the **entire logical
  image byte-for-byte** against an edited staging disk before atomic replace.
  Rebuilds the paired VHDX headers with fresh write GUIDs and sequence.
  Limits: x64 **1 GiB logical / 2 GiB physical**, Win32 **512 MiB
  logical and physical**. Requires free temporary disk space.
  Sparse/dynamic, partially allocated, differencing, dirty-log, mounted/
  live and 4K-sector VHDX remain read-only. Non-FAT writes remain unsupported.
- **WIMGAPI Win32 compatibility**: fixed a Windows Server 2025 system WIMGAPI
  behavior that wrote 16 bytes beyond the published WIM find-data structure.
  Added a guarded 16-byte tail, static Win32/x64 structure-size assertions,
  and boundary validation after each enumeration. The observed
  `0xC0000374` WIM/ESD heap corruption no longer occurs in the tested
  multi-index Windows x86/x64 fixtures.
- **WIM folder edit responsiveness (PR #61).** Total Commander receives
  stage labels and throttled WIMGAPI progress during Apply/Capture. Rebuilt
  WIMs are structurally enumerated, with byte-for-byte readback limited
  to changed files rather than every unchanged compressed file. Tests verify
  empty-folder creation, cancellation rollback and Win32/x64. This is
  faster but **still uses full Apply/Capture**, so large WIMs can take time.
- **Compiler modernization**: Stable uses VS2026 + C++26 draft for both
  Win32/x64. It additionally runs independent VS2026 C++20 compatibility
  checks for both architectures, GCC 14/Clang 19 C++26, Linux C++20,
  Clang ASan/UBSan, Windows WCX ABI, FAT/UDF/WIM and optical regressions.
- Built-in optional updater from v0.19.0 (manual Check now; daily background
  check; persistent opt-out), ISO/UDF write Preview, dynamic VHD FAT write
  Preview, CHD, MDF/MDX, WIM/ESD/SWM, BIF/IMA/IMZ remain available within
  their previously documented format limitations.

## Stable release gates and safety

The Stable distribution means **tested packaging and core** and still contains
explicitly bounded experimental writers, not generic filesystem editing.
Back up important images. VHDX writes are only available for the fixed,
fully allocated, 512-byte-sector FAT case described above.

The Stable GitHub Actions workflow gates publication on:
- VS2026 Win32/x64 C++26 complete builds, CTest and WCX DLL smoke tests.
- VS2026 Win32/x64 C++20 compatibility and Linux GCC14/Clang19 C++26 checks.
- Linux C++20, sanitizers, independent QEMU fixed VHDX FAT write/readback
  and dynamic VHD tests, real UDF/over-4-GiB SHA256, WIM/ESD/SWM,
  CHD, MDS/MDX, safe rollback and package integrity.

The `ISO-WCX-v0.20.0.zip` installer consists of both tested WCX DLLs,
`pluginst.inf` and documentation, without any external runtime helper EXE,
third-party parser DLL, QEMU, mounts or drivers. `SHA256SUMS.txt` is
verified before publication.

Only a fully passing `main` run can create the **unique** v0.20.0 GitHub
release and tag. Previously published v0.19.0 files and tag are immutable
and will never be overwritten.

## Install

After publication, download the ZIP from
https://github.com/sgennadi/ISO-WCX/releases/tag/v0.20.0
and open it directly in Total Commander. Confirm and restart TC.
The integrated updater can perform verified HTTPS checks/downloads and
offer the ZIP to TC; it never overwrites an in-use plugin DLL.
