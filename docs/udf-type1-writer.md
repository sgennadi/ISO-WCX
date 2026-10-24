# Native UDF Type-1 Writer — ISO-WCX Preview

The WCX plugin supports **in-process edits on nonhybrid UDF-only 2048-byte
Type-1 optical images**. It does not mount the image or require external
executables, disk drivers or parser DLLs. This is an experimental write mode,
not universal UDF 1.02–2.60 editing.

## Supported writes

- Replace existing regular files and grow/shrink only within their previously
  reserved FE/EFE embedded space or one existing Short Allocation Descriptor
  (ShortAD) extent. Supports nested paths.
- Logically delete an existing regular file by setting its FID deleted bit,
  updating descriptor CRC/checksum. Deletion **does not reclaim blocks**.
- Create **one new file per transaction, up to 8 MiB**, in an already existing
  embedded directory with enough room for a new File Identifier Descriptor.
  Files up to 1872 bytes use an embedded File Entry; larger files use one
  contiguous ShortAD run allocated from a validated Type-1 Space Bitmap.
- Create **one empty subdirectory per transaction** with an embedded FE.
  Subsequent operations can create files in the new directory.
- Encode newly created names with OSTA CS0 (BMP Unicode including Cyrillic and
  Hebrew; non-BMP names intentionally rejected in this write mode).
- Update the parent directory FE, new child FE/FID, bitmap, closed LVID free
  block count, next unique ID and file/directory counter in the same staged
  transaction. Reparse the entire result and compare extracted file bytes.

## Write preconditions

- UDF-only image, empty 16-sector system area, no ISO/UDF Bridge and no
  bootable/hybrid system-area rewriting.
- 2048-byte logical sectors, one physical Type-1 partition map, validated AVDP,
  File Set Descriptor, LVID and descriptor tag CRC16/checksums.
- New allocations require a consistent Unallocated Space Bitmap Descriptor
  up to 8 MiB (including descriptors spanning multiple 2048-byte sectors)
  and a closed LVID. The original descriptor CRC coverage is preserved; a
  legal header-only CRC is not enlarged to cover the entire bitmap.
  A contiguous free run must exist for each file's FE and ShortAD content.
  An existing embedded directory must have room for the new FID.
- No VAT, Metadata Partition, Sparable partition, hard-link, sparse-file,
  multi-extent or conflicting allocator edits; no symlink source files.
- An entire copy of the source image must fit in a sibling temporary workspace.

## Explicitly unsupported

Renaming, removing directories, editing ISO/UDF Bridge metadata, allocating
multiple new files atomically, growing directory data beyond embedded capacity,
freeing blocks from deleted files, bitmap descriptors larger than 8 MiB,
fragmented new files, non-BMP new names and any VAT/Sparable/Metadata writer.

A rejected mutation, validation failure or cancellation must leave the
original image byte-for-byte unchanged.

## Validation

Synthetic fixtures check FE/EFE replacement, ShortAD growth, FID deletion,
embedded and multi-block file creation, multi-sector bitmap media,
bitmap overlap/counter consistency, Unicode, no-space conditions and rollback. CI independently generates real
UDF 1.02 HD/DVD-RAM images with `mkudffs` and validates edits via
`udfinfo` and 7-Zip. Those three programs are CI-only test utilities and are
**not** runtime dependencies. Manual Total Commander GUI coverage and broader
real-disc interoperability remain necessary before stable write support.

## Source provenance

UDF bitmap conventions, allocation strategies and large-media edge cases
were reviewed against [pali/udftools](https://github.com/pali/udftools).
Its GPL-licensed implementation code has not been copied or linked; the
ISO-WCX writer remains a separate C++20 implementation. Independent
`mkudffs` / `udfinfo` / 7-Zip programs remain CI-only.
