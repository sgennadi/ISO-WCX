# Native VHDX read-only core (experimental)

Independently authored C++20, with **no libvhdi, Rust runtime, .NET,
ReFS block cloning, VirtDisk mount, or third-party disk DLL**.
The native VHDX reader is now also linked into ISO-WCX and is registered
as a **read-only Total Commander WCX format** for clean base images that
contain supported FAT12/16/32 partitions. Standalone inspector tools are
optional development tests; the optical ISO/UDF reader remains available.

## Implemented

- Clean VHDX v1 base images with fixed or dynamic block allocation.
- Both header copies, CRC32C, header sequence, region tables, metadata,
  logical/physical sector sizes, payload block BAT mapping and bounds.
- Sparse/zero/unmapped states return zero bytes; fully present blocks
  provide logical payload data. Partial/differencing bitmap states fail.
- Existing MBR/EBR/GPT and FAT12/16/32 readers work on VHDX logical bytes.
- Logical sector size is automatically selected from VHDX metadata,
  including 4096-byte sectors.

Examples after compiling:

    virtual_partition_inspect disk.vhdx
    virtual_fat_inspect list disk.vhdx 1
    virtual_fat_inspect cat disk.vhdx 1 README.TXT

The cat operation writes raw file bytes to stdout, not to a path supplied
by an untrusted image. Check its exit code before accepting extraction.

## Rejected until separately implemented and tested

- Parent/differencing/snapshot chains and missing parents.
- Non-clean journals requiring transaction-log replay.
- Partially present BAT entries and bitmap-dependent payload sectors.
- Corrupt CRC32C, reserved BAT flags, out-of-file or metadata-overlapping
  payload addresses, unknown required metadata and ambiguous headers.
- Duplicate/overlapping BAT payload physical owners encountered during a
  logical read; unknown optional regions that alias payload/structural bytes;
  overlapping region-table and metadata item extents.
- Writes, mounts, host devices, filesystem repair, NTFS/exFAT extraction.
- VHDX bytes renamed as .img do not silently fall through as raw IMG.

## Verification

tests/test_native_vhdx.py independently synthesizes VHDX containers with
dual headers, region tables, required metadata and valid/invalid BAT
entries. It uses partitioned FAT12 data to validate listing and binary
extraction, sparse blocks, offset bounds and 512/4096-byte sectors.
tests/vhdx_read_fixture.cpp verifies actual logical byte values and
out-of-range reads. Real QEMU/Microsoft fixture interoperability and
Win32/x64 Total Commander GUI extraction on real media remains a separate
release-validation gate; synthetic WCX public-ABI smoke tests run in CI.

## Format documentation and code provenance

- Microsoft [MS-VHDX] specification:
  https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-vhdx/
- 7-Zip optical/virtual-disk design:
  https://github.com/ip7z/7zip
- SecurityRonin/vhdx-core (MIT) structures and test ideas:
  https://github.com/SecurityRonin/vhdx-core
- 0xbadfca11/makevhdx was reviewed as a ReFS-based disk *converter*,
  not a reader: https://github.com/0xbadfca11/makevhdx
- Microsoft Hyper-V Storage samples provide a Windows metadata-only
  VirtDisk comparator in the test suite, not in the WCX reader.

## Extended source review (GitHub VHDX topic, October 2026)

- **maxpat78/FATtools** — Python read/write FAT12/16/32 and exFAT with
  MBR/GPT and Dynamic/Differencing VHD, VHDX, VDI and VMDK. Its useful
  architecture is filesystem-independent sector I/O, RLE free-cluster maps,
  cluster-chain caching and transactional write ordering. It is GPL-licensed;
  **no source has been copied into ISO-WCX**. This is an independent test
  oracle and a design reference for future FAT16/32 and VHDX write support.
  https://github.com/maxpat78/FATtools
- **sumeshi/ntfsdump / ntfsfind** — examples of NTFS file/alternate-data-stream
  extraction and MFT search from raw/VHDX/AVHDX images. Their underlying
  implementations use Python with pytsk3/libvhdi/libvmdk, so importing
  them would violate our no external runtime tools/DLLs requirement.
  Valuable test cases: parent-chain cycles, ID mismatches, path traversal,
  multiple snapshots, NTFS ADS and extracted-name collisions.
  https://github.com/sumeshi/ntfsdump
  https://github.com/sumeshi/ntfsfind
- **SecurityRonin/vhdx-forensic** — independent Rust VHDX reader and forensic
  anomaly analysis. Useful checks include duplicate/overlapping BAT extents,
  unexplained data in absent payload blocks, metadata overlap, parent-chain
  safety and dirty-log sequence/CRC validation. We now reject overlapping
  physical owners **when they are encountered in a single VhdxReader**;
  exhaustive whole-image forensic analysis remains unimplemented.
  https://github.com/SecurityRonin/vhdx-forensic
- **Velocidex/go-vhdx / libyal VHDX documentation** — field-level format
  comparisons and safety checks, especially both CRC32C header copies,
  region ordering, metadata reservation and BAT block geometry.
  https://github.com/Velocidex/go-vhdx
  https://github.com/libyal/libvhdi/blob/main/documentation/Virtual%20Hard%20Disk%20version%202%20(VHDX)%20image%20format.asciidoc
- **TobiasFaller/VHDTool**, WSL compactors and VHDXToFolders — Windows
  VirtDisk attach/detach, Optimize-VHD or DiskPart wrappers. These are
  unrelated to portable, unmounted WCX archive traversal; no embedded
  driver, elevated mount, or administrator-only dependency was added.
  https://github.com/TobiasFaller/VHDTool

### Recommended implementation sequence

1. Finish updater integration, configure dialog and x86/x64 WCX ABI smoke.
2. Add FAT16/FAT32 write operations atop a common native block-writer with
   FAT32 FSInfo, backup boot sector and mirrored FAT updates validated.
3. Implement explicit, bounded **read-only** AVHDX parent chains with strong
   parent identifier checks, cycle/depth limits, canonical path containment
   and per-sector bitmap ownership; refuse unknown/missing parents.
4. Implement bounded **read-only** dirty-log overlays only after independent
   QEMU/Hyper-V fixtures verify replay ordering and CRC behaviour.
5. Explore VHDX writes/compaction behind separate atomic staging and
   integrity tests; never mutate a mounted or live disk.

No source from these projects was copied, linked or vendored.
