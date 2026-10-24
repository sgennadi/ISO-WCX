# ISO-WCX v0.17.0 — Stable Core + Transactional UDF Preview

ISO-WCX is a native C++20 Total Commander WCX plugin for Windows x86/x64.
The installable ZIP contains two self-contained WCX binaries, installer
metadata and documentation, without external unpackers, disk drivers or
parser DLLs. The verified core is distributed as Stable; advanced UDF writing
and some optical/virtual-disk decoding remain explicitly experimental.

## Highlights since v0.16.0

- **UDF Type-1 transactional writes (Preview):** create files and empty
  subdirectories, including nested paths, in one batch; mix creation, bounded
  in-place replacement and logical deletion; update FE/EFE, ShortAD, FID,
  bitmap and LVID consistently and commit the entire operation atomically.
- **UDF reclaimed space (Preview):** scrub and release exclusively owned
  deleted-file FE and recorded ShortAD sectors, reject stale/aliased references,
  and reuse that space on a later transaction. Reuse safe deleted FID slots
  to prevent unbounded directory growth across repeated add/delete cycles.
- **UDF files larger than 4 GiB:** multi-ShortAD data allocation and
  independent 7-Zip streaming SHA-256 readback; bounded extension into the
  final already allocated ShortAD sector with strict overflow rejection.
- **Faster UDF staging:** optional same-filesystem COW reflink on Linux and
  aligned native block cloning on supported Windows filesystems. Unsupported
  filesystems automatically fall back to the existing sparse buffered copy.
  All paths retain full staged verification and atomic replacement. Benchmark
  results are environment-dependent; no guaranteed speedup is claimed.
- **CHD v5 native CDDA extraction:** audio-only and mixed-mode CHD tracks
  available as WAV (RIFF) or RF64 for audio beyond 4 GiB, decoded within
  the plugin. Parallel CHD hunk reads are synchronized. Full >4-GiB RF64
  output was checked with an independent streaming SHA-256 test.
- **MDS v2 / MDX Preview:** native metadata decode and supported plain or
  compressed data-track extraction, with real-image interoperability tests.

## Validation

Release gates cover Visual Studio 2022 Win32 and x64 WCX loading/extraction
and CTest, Linux C++20 CTest, Clang ASan/UBSan, independent mkudffs/udfinfo/
7-Zip UDF interoperability, >4-GiB UDF write and complete streaming readback,
and SHA256-verified installable ZIP packaging. Dedicated Preview CI adds
C++26 compiler compatibility, CHD RF64, VHDX/QEMU and MDS/MDX interop.

## Install

Download **ISO-WCX-v0.17.0.zip** from this release and open it directly in
Total Commander. Accept the plugin installation prompt. Keep
`ISO-WCX.wcx` and `ISO-WCX.wcx64` together and check `SHA256SUMS.txt`
when verifying a downloaded ZIP.

If an old missing `iso.wcx` registration persists, remove only that obsolete
association in Total Commander packer configuration and associate the new
`ISO-WCX.wcx`. Installation instructions are included in English/Russian.

## Write safety and limitations

UDF writing is still **experimental**, not a universal UDF editor. It applies
only to validated nonhybrid UDF-only images using writable physical Type-1
2048-byte partitions. Unsupported: ISO/UDF Bridge rebuilding; metadata,
VAT and sparable partition writes; unrecorded/sparse AED chain creation;
arbitrary hard links, symlinks and directory deletion; rebuilding bootable
media; mixed delete-and-recreate of the same path. Directory/FID allocation
and known extents are bounded; an unsupported image is refused rather than
modified. Disk space is required for a sibling staging file when a COW clone
is unavailable.

WIM/ESD/SWM, optical formats and NTFS/ext4 remain subject to the documented
read/write limits; password-protected optical image data tracks remain
unsupported. GUI tests in Total Commander and broader real optical media
coverage remain outstanding. Edit backup copies of important images.

The v0.16.0 tag and published assets are preserved unchanged.
