# Changelog

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
