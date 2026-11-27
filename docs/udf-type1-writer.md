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
- New allocations require exactly one independently validated UDF
  unallocated-space representation and a closed LVID: either
  (a) a Space Bitmap Descriptor (tag 264) up to 8 MiB, including
  multi-sector maps, or (b) a single-block Unallocated Space Table Entry
  (tag 263) with ordered, nonoverlapping type-1 ShortAD free runs.
  The table writer uses a bounded virtual map in memory and writes only the
  existing tag-263 table extent, updating its CRC; it NEVER fabricates a
  tag-264 bitmap or assumes unreferenced zero data is free. All new ShortAD
  runs must fit the original table's available descriptor bytes.
  Bitmap tag CRC coverage is preserved for bitmap media; both formats
  must agree with the LVID free-block count.
  A free FE block and enough allocator-authorized space are mandatory.
  File data may use multiple noncontiguous ShortAD runs within available FE
  descriptor slots; parent-directory relocation still requires **one
  contiguous** extent. AED continuation writing, sparse extents and
  overlapping allocations remain unsupported.
- No VAT, Metadata Partition, Sparable partition, hard links, unrecorded or
  sparse extents, AED-continuation allocation, conflicting allocator edits,
  or symlink source files. A volume without either usable allocator,
  one with only a Freed Space Table/Bitmap, or one whose tag-263 table
  requires an allocation-extent continuation remains read-only for creation.
  If new allocations fragment the free list beyond the current tag-263 table
  capacity, the transaction aborts before committing any image changes.
- A sibling temporary workspace on the **same filesystem** is mandatory.
  The writer first tries filesystem-level copy-on-write staging (Linux
  `FICLONE` or Windows `FSCTL_DUPLICATE_EXTENTS_TO_FILE`, only for aligned
  supported filesystems); if unavailable, it falls back to the existing
  sparse buffered copy, requiring sufficient free disk space. Never use
  a hardlink or modify the original image in place. Set
  `ISO_WCX_UDF_FORCE_SPARSE_STAGE=1` to force the slower fallback for
  diagnostics or interoperability troubleshooting. This only changes
  performance, not transactional safety or supported file formats.

## Explicitly unsupported

Renaming, removing directories, editing ISO/UDF Bridge metadata,
delete-and-recreate of the same path in one batch, directory growth beyond
8 MiB, automatic reclamation of **old** logically deleted/orphaned files,
bitmap descriptors larger than 8 MiB, sparse/unrecorded
file extents, chained AED writing, non-BMP new names and any
VAT/Sparable/Metadata writer.

A rejected mutation, validation failure or cancellation must leave the
original image byte-for-byte unchanged.

## Staging performance

All UDF edits still use a private sibling staging file and final atomic
rename. When the backing filesystem supports reflinks or aligned block
cloning, the original file data is shared copy-on-write until modified,
avoiding a complete sequential copy of large image data. Cloning may not be
supported by the filesystem, Windows build or image alignment: in that case,
the prior sparse streaming copy is used automatically.

The cross-platform `udf_stage_clone_sparse_parity` test compares every byte
of final images produced by automatic and forced sparse staging, checks that
an independent hardlink observer of the original inode is untouched, tests
rollback and extraction, and checks cleanup of temporary workspaces. A
separate non-gating 512-MiB benchmark reports wall-clock times on Linux and
Windows without asserting unreliable absolute speed improvements. The full
UDF >4-GiB independent 7-Zip SHA-256 check remains mandatory.

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


### Type-2 partition diagnostics and VAT read compatibility

The reader distinguishes the UDF 1.50 VAT ICB (file type 0, 36-byte
EntityID trailer) from UDF 2.x (file type 248, variable-length header).
It validates candidate ICB locations, table bounds, previous-transaction
locations, recorded data extents and every non-hole physical mapping
before accepting a table. A damaged VAT candidate may still be skipped
during **read-only** recovery. Tests cover embedded and external tables,
noncontiguous physical data, prior VAT recovery and malformed candidates.

The Type-1 edit preflight reports VAT, Sparable and Metadata partitions
separately. No Type-1 allocator fallback is permitted.

### Initial VAT append writer (standalone UDF 2.x)

The standalone dispatcher supports batched ordinary file and directory
operations, with a VAT 2.x FE at the last recorded sector of a
write-once image, an open integrity sequence and capacity already
declared by the physical partition descriptor. Embedded or external
physical VAT storage is supported (16 MiB table limit). File data
larger than the embedded FE area is streamed into physical space using
long_ad with partition map 0, as required by UDF 2.01 section 2.3.10.
The replacement FE keeps its virtual TagLocation. The new VAT ICB,
recorded last, refers to the old VAT and remaps only the selected
virtual block. Multiple physical long_ad runs are bounded by the FE
descriptor area and available capacity. The original prefix and all independently parsed root payloads
are verified before atomic commit. Neither damaged historical-VAT
fallback nor zero-filled space authorizes a write.

This is intentionally a narrow backend. EFE,
external allocation continuations,
full/closed sessions, partition growth and coordinated ISO/UDF
bridge VAT writing remain unsupported. Physical data runs that alias
VAT entries, the previous VAT, other files, or the table storage are
rejected. Tests cover embedded-to-external replacement, repeat external
replacement and shrink, including external VAT storage. Multi-GiB VAT
write interoperability still needs dedicated tests. Sparable and
Metadata partition writers are still read-only. Synthetic regression
fixtures exercise repeated append, original-prefix preservation,
independent extraction, cancellation and malformed-table rejection.
Physical disc recording and external-reader interoperability are not
established by these tests.

References: OSTA UDF 1.50 §2.2.10 and UDF 2.60 §2.2.11.


### VAT root namespace updates

Single-file creation and deletion are supported within the validated
embedded root-directory profile. Creation appends a virtual address
rather than recycling a deleted address; the directory and new FE
are appended, the VAT mapping is updated, and the previous session
remains byte-identical. Deletion compacts the root FIDs, marks the
removed virtual address unmapped, and preserves the retired file's
physical data in earlier sessions. File counts and the VAT ICB
UniqueID counter are maintained (OSTA UDF 2.x, section 2.3.4.3 and
the UniqueID rules). The strict input requires consistent live IDs,
FID IDs, file counts in the original root-only profile. A full embedded root
directory is rejected rather than relocated. Tests cover creation,
deletion, re-creation, non-reuse of retired virtual addresses,
transition to external VAT, cancellation and complete old-session
preservation. Legacy VAT 1.50 extended-attribute writing and physical recording remain unsupported.


### VAT nested embedded directories

The standalone VAT 2.x backend traverses and validates the full directory
tree (bounded depth and number of directories). Each directory retains
its own virtual ICB address. Creating a directory writes its mandatory
parent FID, allocates a fresh UniqueID, and increments the parent's link
count. Creation and replacement of regular files inside existing
directories, and removal of files or empty directories, use the same
append-only transaction. Parent FIDs identify the actual parent
including its UniqueID. VAT NumberOfDirectories includes the root,
as required by UDF 2.01 section 2.2.11 and later VAT definitions.
Directory link counts are validated against immediate children.
All prior physical bytes, surviving namespace entries, and newly
written metadata are checked before commit. special ICBs,
and unsupported parent geometry still fail closed.


### VAT atomic batches

One request supports up to 4096 distinct file/directory operations.
Paths are normalized and case-colliding requests are rejected.
Explicit descendant removals are sorted deepest-first and new parent
directories are created before their children, irrespective of the
order supplied by the WCX host. Mixed replacement, creation, and
deletion update one in-memory directory tree, stream each external
payload once, write each modified FE or surviving dirty directory
once, and record exactly one final VAT ICB referencing the original
transaction. All selected replacement sources and the original image
prefix are checked before the single atomic commit. A nonempty
directory removal now expands its validated descendants automatically.
Recreation/replacement of the same normalized
path twice in one batch remain unsupported. A capacity or validation
failure anywhere in the batch leaves the original image unchanged.

### VAT external directory bodies

Directories may use embedded data or recorded virtual ShortAD/LongAD runs. The writer validates every referenced virtual block, FID CRC and virtual TagLocation, rejects metadata aliases, and normalizes valid sector padding. Dirty directories are repacked into fresh virtual blocks when they outgrow the FE, or compacted back into an embedded FE when possible. Retired directory-data mappings are cleared, never reused; all previous physical bytes remain intact. Metadata limits: 8 MiB per directory and 64 MiB aggregate directory metadata, plus the existing namespace and VAT table limits. New external bodies use one contiguous virtual ShortAD with packed, four-byte-aligned FIDs. Regression tests exercise multiple blocks, crossing FIDs, simultaneous root/child growth, repeated changes, shrinking, cancellation after actual staged growth, and malformed AD rejection. EFE, AED chains and unsupported directory allocation types remain read-only.


### VAT recursive directory removal

A directory Delete selects the entire validated subtree, including
external directory-data mappings. Exact selected paths are checked
before expansion, and overlapping directory/child selections are
coalesced. Descendant paths are resolved by canonical component prefix
(not by wildcard matching). The explicit request limit is 4096; the
expanded deletion is bounded by the existing namespace limit. Mixed
batches cannot add or replace objects in a deleted subtree. Internal
directory bodies scheduled for removal are not rewritten: only the
surviving parents, remaining dirty FEs and one final VAT are emitted.
Retired virtual addresses remain unmapped and historical physical bytes
remain byte-identical. Tests exercise deep deletion, external bodies,
overlapping selection, similarly prefixed siblings, conflicting batches
and cancellation after real staged growth.


### UDF 1.50 sequential VAT editing

The standalone sequential writer now also recognizes the restricted
UDF 1.50 profile: NSR02, UDF domain and virtual-map revision 0x0150,
type-0 VAT ICB with zero link count, and a validated 36-byte trailer.
The existing validated append planner is shared with 2.x, but legacy
VAT entry allocation excludes the trailer, which is preserved except
for PreviousVAT ICB. Legacy FIDs do not require the 2.x implementation
use UniqueID field; existing values are preserved and new values are
zero. The writer requires an existing monotonically maintained FE
UniqueID counter and rejects extended attributes, including legacy VAT
LVExtension metadata. Supported operations include file replacement,
creation, directories, batch and recursive removal, and external VAT
storage. Tests use independently constructed type-0 VAT fixtures and
validate the suffix, previous pointer, old-prefix integrity, transition
to external storage, extraction, and rollback. This is not a claim
of complete compatibility with all historical CD-R masters or physical
recording devices. Source: OSTA UDF 1.50 sections 2.2.10, 2.3.6.5,
and 6.10.1.

### Sequential VAT in a split ISO/UDF bridge

The raw UDF reader exposes whether its successfully parsed namespace uses a virtual partition. ISO/UDF dispatch checks this before the equal-length in-place shortcut, which must never modify recorded VAT history. For a nonpartitioned plain ISO/Joliet bridge with bounded El Torito extents, a strict split namespace can use the VAT append planner. Both ISO views must have top-level components disjoint from the UDF namespace and requested targets. Physical UDF payload extents must not overlap protected ISO/boot regions. The staged image is reparsed as an ISO bridge, and every original byte (including ISO VolumeSpaceSize and boot sectors) is verified unchanged before atomic commit. The final LBA contains a new VAT ICB rather than a ROM end anchor. Tests cover equal-length external replacement, batch create, recursive removal, cancellation after staged growth, shared-name rejection and unbounded boot rejection. Rock Ridge/system-use bridge and partitioned MBR/GPT VAT growth remain unsupported. The separate shared-file profile below handles matching regular files.

### Sequential VAT bridge shared-file replacement and removal

Existing regular files that have a matching name and identical single physical extent in UDF and both ISO views may be replaced or removed as a coordinated batch. The existing ISO record coordinator is reused without ROM end-anchor creation. Replacements are always stored externally, including small files, and their new contiguous appended data is shared with ISO/Joliet. The complete original image is compared before coordination; after coordination, only exact planned ISO metadata fields located before sector 256 may differ. VRS, all UDF descriptors and partition bytes, boot data, unrelated ISO files, historical VAT/payloads and newly appended VAT metadata stay protected. The final UDF namespace and replacement sources are verified after ISO coordination. Tests cover equal-length replacement, resizing, small payloads, shared deletion, Joliet, boot, conflicting batches, cancellation and extent mismatch. Shared creation, mixed shared/independent batches, alternate primary names, ISO metadata inside the UDF area, and MBR/GPT VAT growth remain unsupported. Historical UDF transactions remain intact; the ISO namespace itself is updated and is not a historical snapshot.

### Fully mirrored VAT bridge tree remaster

When both ordinary ISO views and the VAT namespace agree on every original path and regular-file extent, the editor can add shared files and directories and mix regular-file replacement/removal in a single append. It rebuilds ISO directory trees and both endian path tables in a bounded zero-filled physical reservation directly preceding the final VAT table/ICB. The reservation is not mapped into VAT; UDF allocation and payload extents are checked independently. Only the planned pre-anchor ISO volume descriptor fields in the old prefix may change. Newly appended UDF FEs, directory blocks and VAT bytes are checked again after ISO coordination, and the resulting ISO/UDF trees and all replacement sources must match before commit. Repeat edits do not patch appended historical ISO directory blocks. Ordinary ISO-representable names, nonempty regular shared files within single ISO extent bounds, and fully mirrored source trees are required. Primary-name aliases, mixed split/mirrored namespaces for tree creation, and MBR/GPT growth remain unsupported. Regression tests cover repeated edits, mixed batches, primary/Joliet path tables, embedded and external VAT storage, bootable fixtures, failed batch and cancellation.


### Recursive mirrored VAT directory removal

The ISO tree planner independently expands exact selected UDF directory
paths to all their descendants after verifying the original ISO/UDF
namespaces. The explicit selections and expanded canonical paths are
separate coordination fields. The strict VAT parser must independently
expand the identical deletion set; mismatch aborts before writing.
Overlapping parent/child selections are coalesced, while missing
explicit paths, case collisions and additions inside a removed
subtree fail closed. The private ISO tree removes children first,
clears non-owning directory references before freeing nodes, and
rebuilds only surviving directory trees and path tables in the new
reservation. The UDF planner retires all removed FEs and virtual
directory data blocks, leaving historical physical bytes intact.
Directory-only mirrored namespaces remain eligible for later appends.
Tests exercise nested external directories, repeated remasters,
overlapping removals, similarly prefixed siblings, VAT map retirement,
path table readback and cancellation. This does not authorize
partitioned VAT, split-tree recursive shared deletion, or Type-1
interpretation of a VAT-only recursive plan.
