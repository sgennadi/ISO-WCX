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

**Virtual hard disks are not yet registered as WCX archives.** The module
and CLI are built/tested independently, without changing optical `.img` logic.

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
invalid footer checksum, corrupt capacity, incomplete footer, saved-state
images; without the optional library, dynamic VHD and VHDX are rejected. This is an isolated
**CLI and library reader**, not a registered WCX format. It does not mount
disks, enumerate physical drives, perform writes or follow differencing
parents. The existing optical ISO/BIN/IMG parsers are not changed.

## libyal/libvhdi evaluation

[libvhdi](https://github.com/libyal/libvhdi) is a native C library whose
upstream README lists VHD v1, VHDX v2, fixed/dynamic and differencing formats.
It currently labels itself **alpha** and uses **LGPL-3.0-or-later**.
No code was copied and no extra DLL is distributed in the Windows WCX package.
An **optional Linux-only, read-only** libvhdi adapter can be enabled with
`-DISO_WCX_WITH_LIBVHDI=ON` for dynamic VHD/VHDX research. It is off by default,
and parent/differencing images remain disabled. Windows WCX does not link it.

For format references, see
[libvhdi's VHD specification](https://github.com/libyal/libvhdi/blob/main/documentation/Virtual%20Hard%20Disk%20%28VHD%29%20image%20format.asciidoc)
and
[libvhdi's VHDX specification](https://github.com/libyal/libvhdi/blob/main/documentation/Virtual%20Hard%20Disk%20version%202%20%28VHDX%29%20image%20format.asciidoc).

## Stage 3–5 (not yet implemented)

1. Validate optional libvhdi against a real-world dynamic VHD/VHDX corpus,
   including corruption and parent/snapshot rejection, before any shipment.
2. GPT GUID names and labels, metadata consistency reporting; CRC-verified
   backup fallback and linked EBR are implemented with mutation tests.
3. Read-only FAT12/16/32, exFAT and NTFS volume readers; expose partition
   folders and file metadata with correct WCX extraction. NTFS sparse runs,
   compressed attributes, reparse points and alternate streams need careful
   explicit behavior.
4. Add a real VHD/VHDX/VMDK/NTFS corpus and Windows Win32/x64 WCX smoke tests
   before registering virtual hard-disk extensions.

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
