# Native UDF Type-1 Writer — ISO-WCX Preview

The plugin handles **nonhybrid UDF-only optical images** through its in-process
C++20 writer. It does not mount the image or require third-party executables,
kernel drivers or additional runtime DLLs.

## Supported writes

- Replace existing **regular** files, including those inside nested directories
- Grow or shrink within the file's existing reserved space — for embedded
  FE/EFE data or a single Short Allocation Descriptor (ShortAD), never by
  allocating new blocks or changing the logical partition mapping
- Update Information Length, EFE Object Size, embedded AD lengths and ShortAD
  extent lengths where needed
- Mark an existing regular file's FID as **deleted**, with appropriate CRC16
  and descriptor-tag checksum updates; any embedded parent FE checksum is
  recalculated too. This hides the file, **but does not reclaim its blocks**

The editor copies the full source to a sibling staging image, performs only
validated targeted updates, reparses the staged UDF and extracts every changed
file to verify exact byte equality. Only then does it atomically replace the
original archive, subject to the source image not having changed.

## Strict preconditions

- UDF-only (no hybrid system area / ISO9660 Bridge), 2048-byte sectors
- Valid AVDP, File Set Descriptor, UDF descriptor tags and CRC16
- One Type-1 physical partition map; no VAT, Metadata, Sparable or snapshot
  partition maps, unknown FE object types, hard links, sparse allocations,
  multi-extent file modifications or overlapping target allocations
- Target is an existing file; requested replacement fits into its previously
  allocated whole blocks or embedded FE/EFE capacity
- Replacement comes from an ordinary file, not a reparse/symlink
- Sufficient free disk space for a **complete duplicate staged image**

## Not implemented

No creation or rename of files, no deletion/creation of directories, no UDF
free-space allocator, no reclamation of blocks after a FID deletion, no complex
UDF allocation descriptors or metadata/VAT/sparable writers, no ISO/UDF Bridge
metadata changes, and no hybrid BIOS/UEFI image rewriting.

Those features require an allocation bitmap and consistent FE/FID/
partition bookkeeping, plus independent validation of ISO and UDF views
for bridges. They must not be simulated through partial ISO-only changes.

## Validation

The independent Python fixture generates UDF file entries and FIDs, including
FE/EFE embedded data, bounded ShortAD size changes, nested directory targets,
FID deletion, malformed CRC16, over-capacity and overlapping allocations,
cancellation and original-byte-preserving rollback. Linux and Windows x86/x64
CI include the test; real media plus GUI compatibility still need validation.
