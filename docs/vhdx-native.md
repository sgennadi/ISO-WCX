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

No source from these projects was copied, linked or vendored.
