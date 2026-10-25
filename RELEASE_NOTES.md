# ISO-WCX v0.19.0 — Stable Core + Native FAT/VHD Writer Preview and Built-in Updater

Native C++20 Total Commander WCX plugin for Windows x86 and x64. The installable
ZIP contains only ISO-WCX.wcx, ISO-WCX.wcx64, pluginst.inf and human-readable
documentation. No helper EXE, external parser DLL, mount, Hyper-V driver,
libvhdi, Python, Rust or Go runtime is needed by Total Commander.

## Changes since v0.18.0

- **Built-in update checker:** Total Commander WCX ConfigurePacker interface,
  manual Check now, nonblocking automatic Stable checks at most daily,
  persistent Do not check for updates automatically option in EN/RU.
  Downloads are obtained over HTTPS and verified with SHA-256 before a
  user-confirmed Total Commander installation. Never overwrites a loaded DLL.
  The previous v0.18.0 release did not contain the updater.
- **FAT12/16/32 transactional editing (Preview):** UTF-8/UTF-16 VFAT long
  filenames, add/replace/delete, directory operations, FAT mirroring,
  FAT32 FSInfo/backup checks and independent readback. Disk-backed, temporary
  staging replaces the previous 256-MiB in-RAM strategy. Maximum raw or
  fixed-VHD physical source size: 64 GiB (x64) or 512 MiB (Win32). Edits
  require available temporary disk space.
- **Partitioned RAW/IMG and fixed VHD:** a validated FAT partition may be
  edited under its exact partitionN/ path, one partition per transaction.
  Unchanged partitions and fixed-VHD footer bytes are preserved.
- **Native sparse dynamic VHD reader:** independent C++ BAT/bitmap decoder
  validates checksums, paired VHD footers, allocation extents and sparse
  zero-filled sectors. MBR/GPT partitions can be browsed and extracted.
- **Dynamic base VHD FAT writes (Preview):** for validated non-differencing
  dynamic VHD v1, stage a complete logical RAW view; update changed sectors
  only in a physical VHD clone, extending BAT/bitmaps where needed; reopen,
  verify the entire logical image, and atomically commit. Maximum *logical*
  VHD size: 1 GiB (x64) or 512 MiB (Win32); requires substantial scratch
  space. No write to live or mounted VHDs. This writer is experimental and
  more limited than raw/fixed-VHD FAT editing.
- **VHDX parser hardening:** strict metadata/region and payload-overlap
  validation with clean base fixed/dynamic VHDX read support. VHDX writing,
  dirty-log recovery and AVHDX/differencing chains are not available.
- Existing ISO/UDF, WIM/ESD/SWM, IMA/IMG/BIF/IMZ, CHD, MDSv2/MDX and other
  reader limitations remain documented in PREVIEW.md and README.md.

## Release quality gate

The automated Stable workflow must pass Visual Studio 2022 Win32/x64 builds,
the full CTest suites and WCX ABI smoke checks, Linux C++20, Clang ASan/UBSan,
independent UDF interoperability and >4-GiB checks, disk staging rollback,
and **QEMU-created VHDX / independent dynamic VHD FAT editing/readback**.
Only then can the version-tagged ZIP and SHA256SUMS.txt be published.
The publication job never overwrites an existing tag or release.

## Install and update

Download ISO-WCX-v0.19.0.zip from the published v0.19.0 Stable release and
open it directly in Total Commander. Verify SHA256SUMS.txt if required.
Restart Total Commander after installation. An upgrade from v0.18.0 may
require re-creating the WCX extension association if Total Commander has
cached capability flags and the Configure button does not appear.

This is a stable **distribution channel and verified core**, not a claim of
unrestricted FAT, UDF, NTFS or VHD writers. For important disks retain a
separate backup; only compatible FAT configurations may be modified.
Opaque BIF, IMZ, compressed/vendor images, WIM splits/ESD, non-FAT HDDs,
differencing VHD/AVHDX, and all VHDX remain read-only for disk editing.
