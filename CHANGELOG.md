# Changelog

## v0.20.0 — 2026-10-25 — VS2026 C++26, fixed VHDX FAT and WIMGAPI safety

- Native WCX Win32/x64 compiled with Visual Studio 2026, MSVC 19.50+
  and experimental C++26 draft flags, with compiled dialect enforcement.
- Add bounded FAT12/16/32 `partitionN/` writes in fully mapped fixed VHDX:
  private physical clone, CRC32C paired headers and full bytewise readback.
- Guard WIM/ESD enumeration from Win32 WIMGAPI's observed extra 16-byte
  output; preserve native system-WIMGAPI backend and unchanged WCX ABI.
- Gate Stable publication on VS2026 C++26 and C++20 compatibility Win32/x64,
  Linux GCC14/Clang19 C++26, QEMU FAT VHDX/VHD, >4-GiB UDF, sanitizers,
  WIM, CHD, MDS/MDX and integrity checks.
- Preserve immutable existing v0.19.0 release and updater opt-out.

## v0.19.0 — 2026-10-25 — Built-in updater, FAT16/32 and dynamic VHD Preview

- Built-in optional automatic Stable update checks and manual Check now,
  secure HTTPS ZIP/SHA256 verification, never replacing a loaded WCX DLL.
- Native transactional FAT12/16/32 Unicode VFAT editing with cloned,
  disk-backed staging in standalone floppy and MBR/GPT RAW/IMG partitions.
- Validated FAT partition edits in fixed VHD without modifying unrelated
  partitions, sector map or end footer.
- Strict checksummed native dynamic sparse VHD reader with physical BAT,
  per-sector bitmap and zero-filled unallocated regions.
- **Preview:** FAT partition editing in clean dynamic base VHD v1 through
  full logical-RAW staging, changed-sector BAT/bitmap allocation, complete
  independently verified logical readback and atomic replacement.
- Stricter clean VHDX metadata/region/payload validation; VHDX remains
  read-only, as do differencing virtual disks and non-FAT volume writes.
- CI: Windows x86/x64, Linux/Clang sanitizers, QEMU/Microsoft interop,
  FAT rollback and independent large UDF validations required for Stable.

## v0.18.0 — 2026-10-25 — Stable Core + Floppy VFAT/IMZ Preview

- Raw IMA/IMG/VFD/FLP floppy reading with native FAT12/16/32 superfloppy
  detection; BIF FAT boot-image browsing and byte-exact opaque boot extraction.
- Native transactional FAT12 writing with long Unicode VFAT filenames
  (Cyrillic, Hebrew and emoji), checked 8.3 aliases and nested folder changes.
- Native WinImage IMZ ZIP Stored/Deflate read-only decoding using static
  zlib, strict ZIP metadata verification, CRC32 and decompression limits.
- Add IMA/VFD/FLP/BIF/IMZ installer associations without extra runtime DLLs.
- New regression tests for VFAT long names, 8.3 collisions, malformed ZIP,
  damaged FAT tables and fail-closed rollback. Full Windows x86/x64, Linux,
  sanitizer and independent large-UDF release checks remain mandatory.
- FAT12 writer, IMZ reader, UDF Type-1 mutations and advanced optical formats
  retain explicitly documented Preview limitations inside Stable packaging.

## v0.17.0 — 2026-10-25 — Stable Core + Transactional UDF Preview

- UDF physical Type-1 writer: atomic multi-file/mkdir batches and mixed
  create/replace/delete operations, including nested paths and rollback.
- UDF bitmap and LVID: bounded multi-ShortAD allocation and >4-GiB files,
  safe deleted FE/data sector reclamation, next-transaction block reuse,
  reclaimed FID slot reuse and independent mkudffs/7-Zip verification.
- UDF staging performance: optional Linux reflink / Windows block cloning
  with automatic sparse streaming fallback and SHA-256 parity tests.
- CHD v5 CDDA WAV/RF64 support with synchronized decoder access and
  full >4-GiB RF64 SHA-256 validation.
- Additional Preview interop for native MDS v2/MDX image data-track decoding.
- Stable release gates: Windows x86/x64, Linux C++20, ASan/UBSan, real and
  >4-GiB UDF readback, SHA256-checked installer ZIP. UDF write remains
  experimental for compatible UDF-only Type-1 media; no universal writer.

## v0.16.0 — 2026-10-24 — Stable Core

- First Stable packaging channel with installable Total Commander ZIP for
  Windows x86 and x64 and checksum verification.
- Native exFAT plus bounded experimental ext2/3/4 and NTFS volume readers.
- Long ISO9660/Joliet filenames, Unicode surrogate pairs, safer El Torito edits.
- Restricted same-size ISO/UDF Bridge content replacement.
- Restricted existing-file UDF Type-1 replacement, in-allocation resize and
  logical FID deletion; UDF-only .iso/.udf content-based dispatch.
- Better installer registration and English/Russian installation guidance.

No unsupported UDF block allocation or general ISO/UDF rebuilding is
advertised as stable. See RELEASE_NOTES.md and PREVIEW.md for limitations.
