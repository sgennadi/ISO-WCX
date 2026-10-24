# Native UDF Type-1 Writer — ISO-WCX Preview

The WCX plugin supports **in-process edits on nonhybrid UDF-only 2048-byte
Type-1 optical images**. It does not mount the image or require external
executables, disk drivers or parser DLLs. This is an experimental write mode,
not universal UDF 1.02–2.60 editing.

## Supported writes

- Replace existing regular files and grow/shrink within their previously
  reserved FE/EFE embedded capacity or recorded ShortAD runs. For a file
  containing multiple ShortADs, replacement may additionally extend the
  **final recorded ShortAD** into its already allocated final sector (up to
  the 30-bit ShortAD length limit), without modifying preceding extents or
  reserving bitmap blocks. Expanding an earlier extent, allocating more
  blocks during replacement or changing the ShortAD count is unsupported.
  Supports nested paths and streamed verification.
- Delete an existing regular file by setting its FID Deleted bit and updating
  descriptor CRC/checksum. On supported bitmap-equipped Type-1 volumes,
  deletion also zeros the 16-byte FID ICB reference, wipes the orphan FE and
  every recorded ShortAD sector **in the staged image**, marks those blocks
  free in the Space Bitmap and increments the LVID free count. The LVID
  file counter is decremented where ImplementationUse provides one.
  Every extent must have exactly one verified owner and be recorded as
  allocated. Previously deleted FIDs with stale ICB references block
  unsafe reclamation. On legacy images **without a writable bitmap**, only
  logical deletion remains supported and no sectors are reclaimed.
- Create **multiple regular files and empty subdirectories in one atomic
  transaction** under compatible embedded or single-ShortAD parents. Its size is bounded by
  image/partition capacity, available bitmap blocks and the finite number of
  ShortAD slots in a File Entry. Files up to 1872 bytes use an embedded FE;
  larger files can use multiple **fragmented** ShortAD runs, each with a
  legal 30-bit extent length. The stream/verification logic uses bounded
  buffers instead of a second extracted multi-gigabyte file.
- Create several empty subdirectories, including nested parents, in the
  same transaction. Child paths can appear before their parent mkdir changes;
  planning orders them by depth and rejects missing parents.
- Automatically relocate an embedded parent directory to a ShortAD run when
  its inline FIDs exceed FE capacity. Grow existing ShortAD directories to
  larger **contiguous** extents, up to the 8 MiB directory-data limit.
  All FIDs remain four-byte aligned and tightly packed, including records
  that straddle logical-sector boundaries. TagLocation is recalculated from
  the sector where each descriptor begins.
- Reuse verified, previously deleted file FID slots without expanding the
  parent directory. Only Deleted FIDs with an entirely cleared ICB, a valid
  descriptor tag/CRC, correct TagLocation and four-byte alignment qualify.
  Exact-size replacements keep zero ImplementationUse; a sufficiently larger
  slot may carry a zero-padded ImplementationUse region with its required
  32-byte EntityID. Unsafe leftover spans (1–31 bytes) are never split or
  repurposed. Multiple new files can reserve different slots in one batch;
  newly deleted FIDs are not reused until a later transaction. The original
  directory logical length and unrelated FID offsets remain unchanged.
  If no suitable slot exists, the writer still appends normally.
- Reclaim **superseded directory data extents** and **newly deleted regular
  file FE/ShortAD allocations**, each after exclusive-ownership verification.
  File sectors are scrubbed and released only after all new FE/ShortAD and
  directory allocation planning completes. Therefore, a newly deleted
  file's blocks become available to the **next transaction**, never to
  another addition within the same batch. Update bitmap/LVID and directory
  FE LogicalBlocksRecorded atomically.
- Encode newly created names with OSTA CS0 (BMP Unicode including Cyrillic and
  Hebrew; non-BMP names intentionally rejected in this write mode).
- In one **mixed atomic batch**, create new files/directories, replace
  existing regular files within their already reserved FE/ShortAD capacity,
  and logically delete existing regular files. Apply deleted FIDs to the
  same parent payload that receives appended FIDs (including embedded FE
  parents and relocated external directories); update CRC/TagLocation
  together. All new FE/FIDs, data extents and parent relocations are planned
  together; update the bitmap and closed LVID **once** with aggregate
  allocated/reclaimed sectors, unique IDs and file/directory counters.
  Deleted files' blocks are released only when uniquely owned and all
  allocation descriptors validate. Reclamation happens after new allocations
  are planned, preventing same-batch reuse. Stage the image once, reparse it
  and verify **every** new/replaced file and deletion before atomic replacement. Cancellation or any
  failed change leaves the original image unchanged.
- If a single path occurs more than once in a mixed batch (including delete
  then recreate at that same path, or case-only collisions), reject the
  entire transaction rather than depending on operation ordering.

## Write preconditions

- UDF-only image, empty 16-sector system area, no ISO/UDF Bridge and no
  bootable/hybrid system-area rewriting.
- 2048-byte logical sectors, one physical Type-1 partition map, validated AVDP,
  File Set Descriptor, LVID and descriptor tag CRC16/checksums.
- New allocations require a consistent Unallocated Space Bitmap Descriptor
  up to 8 MiB (including descriptors spanning multiple 2048-byte sectors)
  and a closed LVID. The original descriptor CRC coverage is preserved; a
  legal header-only CRC is not enlarged to cover the entire bitmap.
  A free FE block and sufficient bitmap space are required for new files.
  File data may use multiple noncontiguous ShortAD runs within available FE
  descriptor slots; parent-directory relocation still requires **one
  contiguous** extent. AED continuation writing, sparse extents and
  overlapping allocations remain unsupported.
- No VAT, Metadata Partition, Sparable partition, hard links, unrecorded or
  sparse extents, AED-continuation allocation, conflicting allocator edits,
  or symlink source files.
- An entire copy of the source image must fit in a sibling temporary workspace.

## Explicitly unsupported

Renaming, removing directories, editing ISO/UDF Bridge metadata,
delete-and-recreate of the same path in one batch, directory growth beyond
8 MiB, automatic reclamation of **old** logically deleted/orphaned files,
bitmap descriptors larger than 8 MiB, sparse/unrecorded
file extents, chained AED writing, non-BMP new names and any
VAT/Sparable/Metadata writer.

A rejected mutation, validation failure or cancellation must leave the
original image byte-for-byte unchanged.

## Validation

Synthetic fixtures check FE/EFE and multi-ShortAD replacement, final-ShortAD
in-place growth, rejected over-allocation, FID deletion, fragmented file
allocation, mixed atomic create/replace/delete batches with nested
directories, embedded/external FID relocation, exact-size and padded deleted
FID slot reuse, repeated add/delete cycling without directory growth,
exclusive FE/ShortAD reclamation, wiped sectors and next-transaction
allocation reuse, stale deleted-FID alias rejection, tag CRC,
bitmap/LVID accounting, Unicode and atomic rollback. CI independently generates UDF 1.02 HD/DVD-RAM images
with `mkudffs` and validates changes via `udfinfo` and 7-Zip. A dedicated
CI job creates, grows, rejects an overrun and replaces a file larger than
4 GiB, checking complete independently decoded 7-Zip SHA-256 each time.
Both UDF interoperability workflows run on matching PRs and pushes to main,
with a manual dispatch option. These external programs are CI-only and are
**not** runtime dependencies. Manual Total Commander GUI coverage and broader
real-disc interoperability remain necessary before stable write support.

## Source provenance

UDF bitmap conventions, allocation strategies and large-media edge cases
were reviewed against [pali/udftools](https://github.com/pali/udftools).
Its GPL-licensed implementation code has not been copied or linked; the
ISO-WCX writer remains a separate C++20 implementation. Independent
`mkudffs` / `udfinfo` / 7-Zip programs remain CI-only.

### Compatibility lesson: multi-sector FIDs

Inspection of [7-Zip's UDF reader](https://github.com/ip7z/7zip/blob/main/CPP/7zip/Archive/Udf/UdfIn.cpp) revealed that directory FIDs must remain a tightly packed four-byte-aligned sequence; arbitrary zero padding between logical sectors makes that independent reader reject the volume. FIDs can span a sector boundary. The native writer therefore avoids injecting sector gaps and updates each FID's TagLocation to the block containing the **beginning** of that descriptor. This behavior is exercised by independent 7-Zip extraction, not inferred from our own parser alone.

## Very large files

The implementation can represent 64-bit InformationLength and split file
content across several 30-bit Short Allocation Descriptors. This is **not**
unlimited capacity: the physical image, UDF partition size, number of FE
descriptor slots and free bitmap runs all impose hard bounds. The writer
streams source content and compares it with independently reparsed staged
extents in fixed-size buffers; it does not extract another multi-gigabyte
temporary file for validation. CI includes fragmented small fixtures, a
real independently formatted file larger than 8 MiB and an over-4-GiB
create/grow/replace test with independently streamed content verification.
This does not imply that all rewritable UDF types are supported: broad
Windows/optical interoperability for files over 4 GiB remains pending.
