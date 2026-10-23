# Virtual hard-disk images: independent read-only architecture

Reference reviewed: [TalAloni/DiskAccessLibrary](https://github.com/TalAloni/DiskAccessLibrary).
DiskAccessLibrary is a C# implementation covering raw IMG, VHD, VMDK,
MBR/GPT, Logical Disk Manager and NTFS. It is **LGPL-3.0-or-later**, not
the GPL-2-only libMirage code; source reuse/linking is possible only with
the applicable license obligations. The existing source has **not** been
copied or linked into ISO-WCX.

## Stage 1 implemented here: raw IMG MBR/GPT reader

- New standalone native C++20 `iso_virtual_partitions` library and
  `virtual_partition_inspect` CLI.
- Read-only, regular-file images only. Logical sectors 512 or 4096 bytes.
- Primary MBR entries, or GPT with a protective MBR.
- Bounds checking and 4 MiB cap on GPT partition entry metadata.
- GPT header/entry-array CRC32 verification, usable range enforcement,
  partition ordering and overlap rejection.
- Reject invalid/cyclic/extended legacy MBR forms rather than presenting
  incorrect or partially reconstructed data.
- Synthetic fixtures: MBR NTFS/Linux partitions, overlapping/out-of-range
  entries, GPT valid/invalid CRC32 and both logical sector sizes.

Example after a CMake build:

    virtual_partition_inspect path/to/raw-hard-disk.img
    virtual_partition_inspect path/to/raw-4k-disk.img 4096

**This does not add virtual hard-disk opening to the Total Commander WCX
plugin yet.** The module and CLI are built/tested independently, and cannot
affect existing optical `.img` interpretation.

## Stage 2–4 (not yet implemented)

1. `IVirtualBlockDevice` byte/sector reading interface for raw IMG,
   fixed/dynamic VHD (validate footer/checksum, BAT, bitmap) and selected
   VMDK flat/sparse extents. Do not follow parent/snapshot paths until they
   are safely resolved and independently tested.
2. GPT backup-header fallback, extended MBR (EBR) with strict cycle limits,
   GUID names and labels. Keep mutation tests for malicious metadata.
3. Read-only FAT12/16/32, exFAT and NTFS volume readers; expose partition
   folders and file metadata with correct WCX extraction. NTFS sparse runs,
   compressed attributes, reparse points and alternate streams need careful
   explicit behavior.
4. Use real-world VHD/VMDK/NTFS image corpus and Windows Win32/x64 WCX
   smoke tests before registering file extensions.

## Important separation

Optical sectors (CD-DA/Mode 1/Mode 2, 2048/2352/2448 bytes) have different
semantics from HDD logical sectors (512/4096 bytes) and must not share
heuristic offset detection. No writes, no disk mounting, no physical-disk
device access are needed for ISO-WCX.

DiskAccessLibrary's README specifically warns that **NTFS write/create**
was tested only against XP/Server 2003 behavior. ISO-WCX must not expose
disk writes or present vendor VHD/VMDK coverage until independently tested.

This is a format-interoperability research and implementation effort, not
a dependency on, fork of, or endorsement by Tal Aloni.
