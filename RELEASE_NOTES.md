# ISO-WCX v0.21.1 — Guarded ISO/WIM Editing and Modern Total Commander Integration

Stable Core update for Windows Total Commander Win32/x64, built using
Visual Studio 2026 / experimental C++26 draft mode. The native WCX
plug-ins ship together in one installable ZIP. No setup EXE, external
mount service, standalone parser DLL or third-party runtime is needed.

## Changes since v0.21.0

- **ISO F5/Delete safeguards:** transactional edits for compatible
  ISO9660/Joliet layouts with independent xorriso regression verification
  of preserved BIOS/EFI El Torito catalog and boot payloads in synthetic
  test media. Unsupported Rock Ridge and unrecognized/hybrid boot
  configurations are refused rather than rewritten.
- **WIM editing and rollback:** selective WIMGAPI Apply/Capture changes,
  verified readback and ACL-aware removal of temporary
  `.iso-wcx-edit-*` / `expanded` content on success and cancellation.
  Multi-gigabyte `install.wim` still requires substantial time/disk.
- **WCX association repair:** capabilities mask 159, local registration
  audit and guarded offline repair of owned ISO/WIM/UDF entries, with
  a backup of `wincmd.ini` and separate explicit consent to replace a
  legacy ISO handler. Unrelated WCX registrations remain untouched.
- **Updater and UI:** Stable/Preview channel selector, modern native
  Windows Task Dialogs, a separate Open installer / Later choice after
  SHA-256 verification, in-process F5 dispatch counters and improved
  Configure usability on high-DPI Windows displays.
- **Verification:** Win32/x64 VS2026 C++26 and C++20 tests, Linux C++26
  and C++20, ISO/WIM/UDF, QEMU/virtual media, sanitizers, installer
  checks and SHA-256 manifest. No Preview ZIP is auto-installed.

## Important limitations

**Stable means a tested core and distribution, not universal ISO/UDF,
WIM, NTFS or ext4 write support.** The bootability of *actual customer*
Windows/WinPE ISO images after edits has not been field-tested or
certified. Use a disposable copy and independently test BIOS/UEFI
booting before deployment. Unsupported layouts fail closed.
WIM still relies on Windows WIMGAPI Apply/Capture, not instantaneous
metadata-only modification; ESD/SWM and arbitrary NTFS/ext4 writes
remain unsupported. See PREVIEW.md for the safety matrix.

## Installation

Download `ISO-WCX-v0.21.1.zip` and `SHA256SUMS.txt` from
https://github.com/sgennadi/ISO-WCX/releases/tag/v0.21.1 .
Verify the SHA-256 manifest, open ZIP in Total Commander, confirm WCX
installation and restart Total Commander. Includes `ISO-WCX.wcx`
(Win32) and `ISO-WCX.wcx64` (x64).
All previous release tags/assets are preserved.
