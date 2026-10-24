# FAT12 / FAT16 / FAT32 virtual disk filesystems

This is an independently developed, read-only native C++ module. Its
layered design was informed by the official 7-Zip FatHandler.cpp and
HandlerCont interfaces (https://github.com/ip7z/7zip). No 7-Zip source
has been copied or linked.

## How it works

The existing VirtualDiskReader supplies logical bytes from partitioned HDD
IMG/RAW, fixed VHD and clean base fixed/dynamic VHDX. The MBR/EBR/GPT parser returns a partition extent. FatVolume
then validates the FAT BIOS Parameter Block, FAT type by cluster count,
FAT allocation-table capacity, cluster mapping and file/directory chains.

Two safe interfaces are available:

- list: reads validated directory entries, including nested folders and
  VFAT long names, and returns relative UTF-8 paths.
- cat: extracts bytes of a listed file to the caller's binary stream.

CLI examples:

    virtual_partition_inspect disk.img
    virtual_fat_inspect list disk.img 1
    virtual_fat_inspect cat disk.img 1 README.TXT > README.TXT
    virtual_fat_inspect list windows-fixed.vhd 1
    virtual_fat_inspect list disk-4kn.img 1 4096

Partition numbers start at 1. The command never constructs output
filenames from untrusted metadata. Check the exit status: a corrupt
cluster chain can leave partial data in redirected stdout.

## Integrity limits

- No disk writes, mounting, physical-drive access or parent paths.
- BPB and file extents must lie inside the discovered partition.
- Up to 100,000 listing items, depth 32 and 8 MiB per directory.
- Each cluster is at most 1 MiB, and chains reject cycles, out-of-range
  sectors, reserved/free/bad clusters and incomplete file allocation.
- Every VFAT name must have a consistent ordinal sequence and matching
  short-name checksum, otherwise a validated 8.3 fallback is used.
- Unsafe filename components and duplicate paths are rejected.
- 8.3 OEM non-ASCII bytes use ~XX hex tokens instead of guessing the
  disk's encoding from the host locale.

## Current limits

Since v0.13 Preview, this same native reader is also built directly into
the WCX DLL. IMG/RAW, fixed VHD and supported VHDX images expose FAT
files under partition1/, etc. Extraction participates in the WCX progress,
cancellation and rollback transaction. The standalone CLI is developer-only.

This document describes the original FAT12/16/32 backend. Separate
native readers now add exFAT plus deliberately limited ext2/3/4 and NTFS
subsets inside WCX. NTFS remains read-only and rejects compression, encrypted
files, attribute lists and reparse restoration. APFS, physical disk access,
snapshot parents and FAT timestamps/extended attributes remain unsupported.

The regression suite uses independently constructed synthetic FAT12,
FAT16 and FAT32 partitioned images and fixed-size VHD plus malformed
directory and cluster-chain fixtures. See tests/test_virtual_fat.py.
