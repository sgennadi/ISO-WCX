# ISO-WCX v0.16.0 — Stable Core

Stable installation channel for verified core functionality; advanced parsers
and editing modes remain strictly limited or experimental. The runtime includes
only the Total Commander WCX DLLs ISO-WCX.wcx (Win32) and ISO-WCX.wcx64 (x64),
plus documentation. No separate unpacker, mount driver or third-party parser
DLL is bundled.

## Core capabilities

- ISO9660/Joliet browsing and extraction, and transactional editing of validated
  plain 2048-byte images; ISO Long(221), Joliet Long(110), Unicode/surrogate
  pairs and El Torito BIOS/UEFI data protection.
- Native UDF Type-1 browsing. Experimental editing of existing files without
  new block allocation, bounded resize, and logical FID deletion.
- ISO/UDF Bridge: only same-size content replacement when both namespaces map
  an identical single extent in a nonbootable image.
- MBR/GPT RAW/IMG, fixed VHD and clean base VHDX browsing; FAT12/16/32, exFAT,
  plus restricted experimental ext2/3/4 and NTFS reading.
- WIM/ESD and complete split SWM browsing/extraction using Windows WIMGAPI.
  Restricted standard unsplit WIM editing requires elevated privileges.
- Documented optical formats including CUE/BIN, CCD, NRG, MDSv1, ISZ/DAA and CHD
  under their listed limitations.

## Installation

Open ISO-WCX-v0.16.0.zip directly in Total Commander and accept the WCX
installer prompt. Keep ISO-WCX.wcx and ISO-WCX.wcx64 together. SHA256SUMS.txt
verifies the ZIP's integrity.

## Unsupported or experimental

This is NOT a general-purpose NTFS/ext4/UDF editor. Arbitrary UDF file/dir
creation or disk-block reclamation, metadata/VAT/sparable UDF write operations,
ISO/UDF Bridge rebuilding, encrypted/compressed NTFS, advanced ext4 features,
VHD(X) parent chains, ESD/SWM writes and complex optical protections remain
unsupported. Real-media and Total Commander GUI coverage remains incomplete.
Edit copies of valuable boot/install images, never originals.

UDF bitmap allocation development is separate in PR #33 and is NOT included
in this stable release.

## Quality gates

Publication follows successful Windows x86/x64, Linux C++20, Clang ASan/UBSan,
WCX DLL load/extract tests and SHA256-verified installer packaging.
