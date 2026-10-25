# Virtual hard-disk images: independent read-only architecture

Reference reviewed: [TalAloni/DiskAccessLibrary](https://github.com/TalAloni/DiskAccessLibrary).
DiskAccessLibrary is a C# implementation covering raw IMG, VHD, VMDK,
MBR/GPT, Logical Disk Manager and NTFS. It is **LGPL-3.0-or-later**, not
the GPL-2-only libMirage code; source reuse/linking is possible only with
the applicable license obligations. The existing source has **not** been
copied or linked into ISO-WCX.

## Stage 1 implemented here: raw IMG MBR/GPT reader

- New standalone native C++20-compatible `iso_virtual_partitions` library and
  `virtual_partition_inspect` CLI.
- Read-only, regular-file images only. Logical sectors 512 or 4096 bytes.
- Primary MBR + bounded linked extended MBR (EBR), or GPT with protective MBR.
- Bounds checking and 4 MiB cap on GPT partition entry metadata.
- Primary and independent backup GPT header/entry-array CRC32 verification,
  fallback recovery, usable range enforcement and overlap rejection.
- Reject malformed/out-of-bounds EBR links, third/fourth EBR entries, reverse
  chains, overlapping partitions and hybrid GPT/MBR instead of guessing.
- Synthetic fixtures: MBR NTFS/Linux partitions, overlapping/out-of-range
  entries, GPT valid/invalid CRC32 and both logical sector sizes.

Example after a CMake build:

    virtual_partition_inspect path/to/raw-hard-disk.img
    virtual_partition_inspect path/to/raw-4k-disk.img 4096

**As of v0.13 Preview, the tested read-only FAT virtual-disk layer is
linked into WCX itself.** Standalone CLI tools remain for developer tests,
not installation. The existing optical IMG/ISO reader is retained.

## Stage 2 implemented: safe native fixed-size VHD (read-only)

The `VirtualDiskReader` block abstraction now handles **raw hard disk
images and fixed VHD v1**. A fixed VHD has a 512-byte footer appended after
the logical disk data. The reader validates its `conectix` signature, VHD
version, features, disk type, checksum, saved-state flag, and declared byte
capacity before allowing MBR/GPT access. Logical reads are bounded to the
data region; the footer can never be interpreted as a partition or file.

    virtual_partition_inspect path/to/raw-hard-disk.img
    virtual_partition_inspect path/to/windows-fixed.vhd

Synthetic regression tests cover fixed VHD partition enumeration, byte reads,
invalid footer checksum, corrupt capacity, incomplete footer and saved-state
images. The newer Preview includes a separate, native **dynamic base VHD v1**
reader: it requires matching first/last footers, the `cxsparse` header,
correct checksums and BAT extents, sector bitmap ownership and sparse zero
fill. It rejects corrupted/overlapping blocks and differencing VHD parents.
The WCX reader does not mount disks or enumerate physical drives.
Transactional FAT **writes** remain restricted to RAW/IMG and **fixed** VHD;
dynamic VHD and all VHDX are **read-only**. Optical ISO/BIN/IMG parsing is
unchanged.

## Native-only disk readers (no libvhdi)

ISO-WCX no longer contains the experimental Linux-only libvhdi adapter.
The CMake option, linkage, source conditionals and dedicated CI job were
removed. No `libvhdi.dll`, `libvhdi.so` or .NET dependency is needed.
Raw HDD IMG and fixed/dynamic VHD v1 are implemented independently in C++.

The experimental native C++20 **dynamic VHD** reader supports read-only
MBR/GPT filesystem traversal through its bounded 32-bit big-endian BAT,
MSB-first sector bitmaps and absent-block zero fill; it refuses unsupported
differencing images. An independent experimental C++20 VHDX v1
reader handles clean, non-differencing fixed/dynamic base images
with CRC32C-validated headers, required metadata and BAT-mapped payload
reads, including sparse blocks. See [VHDX details](vhdx-native.md).
Differencing parents and log replay are deliberately unsupported.
VHDX renamed to raw IMG is rejected rather than silently misinterpreted.

7-Zip and libvhdi remain external technical references, not source-code
dependencies. The ISO-WCX implementation neither imports nor links their
format handler code.

## Stage 3–5 (not yet implemented)

1. Extend independent QEMU/Microsoft interop and hostile-image tests
   for the VHD and VHDX readers. Dynamic VHD **read-only** footer/BAT/bitmap
   parsing is implemented and tested using independent FAT16/FAT32 synthetic
   fixtures; safe dynamic **write** requires a separate transactional
   allocator that updates physical blocks, BAT, bitmaps, both footers and
   checks external-drive interoperability before enabling WCX PackFiles.
2. GPT GUID names and labels, metadata consistency reporting; CRC-verified
   backup fallback and linked EBR are implemented with mutation tests.
3. FAT12/16/32 is available both in WCX and a developer-only bounded reader
   (`virtual_fat_inspect` list/cat) over IMG, fixed VHD and clean base VHDX.
   Broader real-image and GUI validation remains required. exFAT is
   implemented as a separate bounded reader, ext2/3/4 is an experimental
   subset, and native NTFS preview reads basic MFT/index/resident/data runs.
   NTFS compression, reparse restoration, attribute-list continuations,
   alternate streams and journal replay remain unsupported.
4. Expand the real VHD/VHDX/VMDK/NTFS corpus. Native Windows Win32/x64 WCX
   smoke tests now exercise in-process FAT extraction for supported images.

## Stage 3 implemented: FAT12/16/32 inside WCX and developer CLI

A separate `FatVolume` class validates the FAT BPB, directory and cluster
chains, VFAT long-name sequences, bounds and unsafe names. It can list and
extract files via `virtual_fat_inspect` without disk mounting, writes or
implicit output-path creation. Synthetic tests cover FAT12/16/32, nested
folders, noncontiguous files, fixed VHD/VHDX and corrupted chains. This
is registered as a read-only WCX image format for supported FAT volumes. See [FAT design](fat-filesystems.md).

## Experimental C++26 compatibility

C++20 remains the supported WCX build. The opt-in
`-DISO_WCX_CXX_STANDARD=26` builds the same sources with experimental
C++26 dialect support and *fails rather than silently downgrading* when
unsupported. GitHub Actions validates GCC 14 and Clang 19 on Ubuntu.
C++26 features are not presumed complete in those compilers.

## Important separation

Optical sectors (CD-DA/Mode 1/Mode 2, 2048/2352/2448 bytes) have different
semantics from HDD logical sectors (512/4096 bytes) and must not share
heuristic offset detection. No writes, no disk mounting, no physical-disk
device access are needed for ISO-WCX.

DiskAccessLibrary's README specifically warns that **NTFS write/create**
was tested only against XP/Server 2003 behavior. ISO-WCX must not expose
disk writes or present vendor VHD/VMDK coverage until independently tested.

This is a format-interoperability research and implementation effort, not a
fork of, or endorsement by Tal Aloni. The architecture also draws on the
[official 7-Zip disk handlers](7zip-disk-formats.md); no 7-Zip code was copied.

## Current filesystem reader layering (post v0.15 Preview)

Partition mapping selects each MBR/GPT extent and probes distinct in-process
readers via a common VolumeReader interface: FAT12/16/32, exFAT 1.0 and
experimental ext2/3/4, and restricted NTFS. File names are validated and mapped beneath
`partitionN/`. The existing WCX transactional staging and cancellation
mechanism is reused for extraction. No mount or external disk DLL is loaded.

The ext2/3/4 reader is intentionally limited to supported superblocks,
block maps, ext4 extents, regular files and directories; modern ext4 features
such as journal replay, encryption and inline data are refused. NTFS reads
MFT/INDX records with USA validation, directories and ordinary resident and
nonresident data runs, but rejects complex attribute lists and NTFS compressed
and encrypted streams. Real-media validation is outstanding; these readers
are preview, read-only.
