# Disk image architecture study: 7-Zip and ISO-WCX

Reviewed official sources: https://github.com/ip7z/7zip (October 2026).

## Design learned from 7-Zip (no code copied)

7-Zip separates virtual-disk containers from partitions and file systems.
Its CHandlerImg abstraction (CPP/7zip/Archive/HandlerCont.h and .cpp)
offers a logical random-access stream to MBR/GPT/filesystem recognizers,
rather than requiring disk mounting. The "GetImgExt" probe identifies
possible GPT, MBR, or ext content, but probing is NOT file extraction.

This is the architecture we are following in ISO-WCX:

1. Container: VirtualDiskReader supplies bounded logical bytes. Raw HDD IMG
   and native fixed-size VHD are supported in the built-in WCX reader.
   No external virtual disk library is linked or loaded.
2. Partitions: inspect_partition_table interprets GPT and MBR and now
   validates and follows bounded extended MBR (EBR) links.
3. Filesystems: FAT12/16/32 reader is built into WCX and developer tools;
   exFAT/NTFS/ext4 readers are future work.
4. WCX: FAT-backed IMG/VHD/VHDX handlers are integrated in v0.13 Preview;
   other formats require independent parser and extraction tests.

Keep optical CD sector handling (2048/2336/2352/2448 bytes) separate from
512/4096-byte HDD logical sectors. File extension is not a substitute for
file signature and validated metadata.

## Source comparison and future work

| Official 7-Zip source (CPP/7zip/Archive) | Key structure | ISO-WCX |
| --- | --- | --- |
| VhdHandler.cpp | VHD footer, BAT, bitmaps | Native fixed VHD, dynamic VHD needs a future native BAT reader |
| VhdxHandler.cpp | CRC32C regions, metadata/BAT | Native clean fixed/dynamic VHDX reader linked directly into WCX |
| VmdkHandler.cpp | Sparse grain mapping | Separate read-only implementation later |
| QcowHandler.cpp | L1/L2 clusters and decompression | Separate read-only QCOW2 later |
| VdiHandler.cpp | Block allocation maps | Separate read-only VDI later |
| GptHandler.cpp | GUIDs, CRC32, extents | Independent primary/backup GPT CRC and recovery |
| MbrHandler.cpp | Primary partitions and EBR links | Independent bounded EBR implementation |
| NtfsHandler.cpp | MFT and attribute runlists | File reader not yet implemented |
| FatHandler.cpp | FAT12/16/32 cluster chaining | Native FAT12/16/32 WCX and CLI list/extract implemented |
| ExtHandler.cpp | Inodes/extents | Future independent Linux filesystem reader |
| ApfsHandler.cpp, HfsHandler.cpp, DmgHandler.cpp | macOS disks/images | Separate milestones |

Read handlers at https://github.com/ip7z/7zip/tree/main/CPP/7zip/Archive.

## What is implemented vs not implemented

- ISO/UDF and optical disc images: existing WCX plugin, read-only.
- Raw HDD IMG GPT/MBR and extended EBR, fixed VHD GPT/MBR:
  read-only WCX/CLI native parser for supported FAT partitions.
- Dynamic VHD: unsupported. Fixed/dynamic base VHDX: native experimental reader in CLI with bounded BAT, CRC32C and metadata validation.
- FAT12/16/32 file listing and extraction: linked into WCX for partitioned
  IMG/RAW, fixed VHD and clean base VHDX; also testable via developer CLI.
- VMDK/QCOW2/VDI and exFAT/NTFS/ext filesystems within virtual disks:
  not yet supported.
- Differencing parents and snapshot chains: explicitly disabled.

## License provenance

7-Zip DOC/License.txt (https://github.com/ip7z/7zip/blob/main/DOC/License.txt)
states LGPL-2.1-or-later for most code, plus unRAR restrictions and BSD
exceptions. No 7-Zip or libvhdi source is copied, linked, vendored or
shipped by ISO-WCX. Any actual source reuse
needs file-level provenance and compliance with the relevant license terms.

## CI and threat model

- Preserve C++20 Windows x86/x64 WCX binaries and installer; experimental
  C++26 tests do not modify the release ABI.
- Reject out-of-range sectors, metadata overlaps, cyclic/bad EBR links,
  invalid GPT CRC, incorrect VHD footer checksums, corrupt VHDX, and
  unsafe parent/backing path references.
- A valid signature is not proof of a safe or fully supported filesystem.
- Use synthetic invalid-image regression tests, sanitizer runs and
  independent real-image fixtures before supporting a new WCX association.
- No writes, no physical-drive access, no disk mounts, no automatic
  cross-file parent resolution.

Next objective: validate FAT using more independent real-world image samples,
then add exFAT and NTFS using bounded native read-only parsers with strict
corruption handling. The stable optical ISO/UDF handler stays isolated.
