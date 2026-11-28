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


### Split-namespace MBR VAT bridge

A validated active LBA-zero isohybrid type-0x17 container with
bounded, nonoverlapping independent EFI partitions may grow by an
append-only VAT transaction. ISO and UDF namespaces must be disjoint.
The existing ISO MBR preflight checks boot/partition geometry and
live extents; the VAT writer independently checks the original MBR
and derives the exact new header. Alignment padding is included in
the partition-capacity calculation and inserted before the new VAT
table and ICB, so the last physical optical sector remains the clean
VAT ICB. Only the MBR container's ending CHS and length fields change
in the old image; all UDF history, ISO descriptors, boot and EFI
payloads and the remainder of the prefix stay byte-identical.
Regression tests cover repeated append, external and embedded VAT
tables, exact MBR geometry, rollback, protected ISO paths, capacity
and malformed partition rejection. See the later profiles below for
partially shared MBR and GPT VAT support. Physical boot testing remains
separate from image-level validation.


### Fully mirrored MBR VAT mastering

The validated whole-image MBR profile also permits the fully mirrored
ISO/Joliet/VAT tree coordinator. Add, replace and recursively remove
shared entries through a single append transaction. Cylinder-alignment
padding precedes the reserved ISO directory/path-table area so the
coordinator can derive the reservation independently from the final
VAT ICB. The old image may change only at the exact validated MBR
header and planned ISO descriptor fields; independent EFI partitions,
boot resources, old ISO trees, UDF metadata and historical payloads
remain unchanged. The MBR header and appended VAT blocks are checked
again after ISO coordination, and the staged hybrid geometry is
reparsed. Tests extend the mirrored-tree matrix to MBR with and
without Joliet, embedded and external VAT, repeated remastering,
recursive deletion, cancellation, malformed CHS and EFI-alias
rejection. A paired plain/MBR test proves that a transaction fitting
the declared capacity without alignment is rejected when mandatory
cylinder rounding would overflow it. Later sections extend this support
to partially shared MBR namespaces and GPT.
Physical boot behavior is not established by these image-level tests.


### Split-namespace sequential GPT VAT bridge (initial restricted profile)

A standard 512-byte-sector protective-only GPT with 128 entries of
128 bytes, a trailing nine-optical-sector backup reservation, and
a clean VAT ICB immediately before that reservation may grow. The ISO
preflight validates both GPT arrays, partition/boot bounds and live ISO
extents. The VAT writer independently validates GPT geometry and CRCs,
the exact last recorded VAT and its mappings. Appends begin at the
physical end of the original file; the old VAT and all preceding UDF
sectors are preserved. Only the protective MBR length and primary GPT
header change at the front. The obsolete backup array/header is cleared
and a new identical partition array and CRC-correct backup header are
placed after the new VAT. No partition extent moves. The final staged
GPT is independently reparsed and compared to the expected growth
plan; the full unchanged prefix and appended bytes are verified.
Regression tests cover embedded/external VAT, repeated appends, EFI
payload preservation, cancellation, CRC corruption, nonzero trailer
padding and refusal of damaged current VAT. This does not support nonstandard arrays, hybrid protective MBR
entries, or physical recording. Physical BIOS/UEFI boot remains
unverified.

### Mirrored sequential GPT VAT bridge

The same protective-only GPT profile supports a proven, fully mirrored
ISO/Joliet/UDF tree. The coordinated remaster reserves new ISO directory
and path-table sectors immediately before the final VAT table/ICB, not
inside the trailing GPT. The recorded optical volume excludes the nine
GPT trailer sectors. The VAT backend independently verifies the exact
relocated GPT headers/arrays and all historical bytes outside the
explicit ISO descriptor fields. It rechecks appended VAT metadata after
ISO coordination, then independently validates all live UDF extents
before atomic commit. Regression coverage extends mirrored create,
replace, recursive delete, cancellation and repeated growth across
embedded and external VAT, with and without Joliet; checks GPT CRCs and
unchanged EFI partition data; and rejects corrupt GPT, trailer or latest
VAT bytes.

### Partially shared sequential GPT VAT bridge

For a partially shared ISO/Joliet/UDF namespace, a regular file already
present in all applicable views with the same verified nonempty physical
extent may be replaced or removed atomically. The ISO coordinator
changes only the explicitly planned original record fields (or checked
directory-sector compaction on deletion) and volume lengths. All mutable
historical ISO metadata must precede the UDF anchor at sector 256.
The VAT writer appends a new mapping and verifies the exact relocated
GPT envelope, independent partition contents, old VAT history and all
remaining namespace entries. Repeated edits are supported while the
original ISO directory fields remain in the permitted area. Tests cover
embedded and external VAT, with and without Joliet, repeated replacement,
delete, cancellation, mismatched shared extents, corrupt GPT/VAT and
a CRC-valid GPT partition alias of the shared payload. Partial MBR
coordination uses the fresh-tree profile described below.

### Shared-subtree additions in partially shared MBR/GPT VAT

For the supported sequential MBR and GPT profiles, the ISO/Joliet tree may be a
verified strict subset of the UDF tree. Every existing ISO path must
have a corresponding UDF entry of the same type and each regular ISO
file must have the same nonempty, single contiguous extent in all
applicable views. At least one common regular file must establish the
shared namespace. In this profile a batch may create new files and
directories only beneath an existing shared non-root directory,
including explicitly created descendants in the same batch. The
coordinator independently records all original UDF-only paths and
requires them to remain UDF-only with the same type after remastering;
the VAT backend validates their complete preserved layouts. New root
paths, conflicts with independent UDF entries, ISO-only trees and
unsupported mappings fail closed. Regression fixtures cover repeated nested creation,
preservation of independent UDF files both at root and within a
shared directory, GPT CRCs, path tables, invalid batches and staged
cancellation, with and without Joliet and external VAT storage.
GPT shared-file-only batches with original pre-anchor ISO records
retain the narrower byte-minimal coordinator. MBR partial transactions
always use a fresh ISO directory/path-table reservation, including
replacement and deletion of files whose records precede the anchor.

### Shared-subtree mixed transactions and recursive deletion

The partial MBR/GPT remaster also accepts replacement and deletion of
existing shared paths, including paths whose ISO records were appended
past the original UDF anchor. New entries still require an unambiguous
shared non-root parent. A single transaction may replace a file,
recursively unlink a fully shared subtree, and add another subtree.
The planner checks exact UDF names and case-folded uniqueness; a
directory removal is rejected if any independent UDF-only entry lies
beneath it. The expanded deletion contract is checked again against
the original shared tree before serialization. Both planners validate
identical expanded deletions. The final staged namespace preserves all
independent paths and the VAT backend verifies their original layouts.
Regression coverage includes post-remaster replacements, mixed batches,
overlapping recursive selections, conflicts with independent descendants,
subsequent appends, cancellation, historical byte preservation and GPT
integrity. The MBR matrix seeds the partial tree through the supported
split-namespace MBR writer and verifies cylinder-aligned growth, exact
container length and CHS, an unchanged independent EFI partition,
the final VAT ICB at physical EOF, and rollback for invalid CHS,
unknown partition type, inconsistent length and an EFI payload alias.
The original MBR coordinator and its reservation requirement remain
unchanged. The following profile extends the subset requirement.
New root paths remain ambiguous, and physical boot behavior remains
unverified.

### Mixed bridges with independent ISO-only and UDF-only paths

For the established sequential MBR and protective GPT profiles, the
ISO/Joliet and UDF trees may now overlap without either being a subset
of the other. A common regular file with matching physical extent is
still required. The coordinator classifies original shared entries,
ISO-only paths and UDF-only paths separately, verifies the primary
and Joliet views, and rejects independent ISO payload extents that
overlap live UDF payload ranges. ISO-only records and payloads remain
unchanged in the planned tree (directory locations may be remastered);
UDF-only paths retain their original layout. The staged verification
checks independent path membership and type separately from the shared
namespace and retains the existing byte-level historical prefix guard.

New entries are allowed only beneath confirmed shared non-root
directories or their new descendants. Direct edits to independent
paths, ambiguous root additions, and recursive deletion of a directory
containing independent descendants on either side are rejected.
Shared subtree additions, replacements and recursive deletes still
use one atomic transaction. The expanded matrix covers MBR and GPT,
embedded and external VAT, primary ISO and Joliet, ISO-only root files
and directories, ISO-only children of shared directories, colliding
names, aliasing payload extents, repeated remastering and rollback.
Independent primary/Joliet trees that disagree with each other and
multi-extent ISO-only records remain outside this profile. The next
profile adds independent UDF-only transactions. Physical boot behavior
is not established by the image-level tests.

### Independent UDF-only transactions inside mixed MBR/GPT bridges

A pure UDF-only batch in a partially shared partitioned VAT bridge may
replace or remove existing independent entries (including an existing
private file below a shared directory), recursively remove independent
directories, or create new entries under an existing independent UDF
directory and its new descendants. Exact existing names and folded
uniqueness are required. Both ISO views must agree with the original
ISO tree, every shared regular file must have its matching physical
extent, independent UDF extents must avoid the protected ISO layout,
and shared extents must not alias independent ISO payloads. Synthetic
boot paths and shared descendants are protected.

This route appends a VAT transaction without reauthoring ISO volume
descriptors, directory records or path tables. The existing MBR and
GPT growth checks, byte-exact historical guard, strict VAT planner and
readback remain mandatory. Mixed shared/private batches and ambiguous
new root paths remain unsupported; new entries below shared directories
retain the shared-tree policy. The original disjoint-namespace route
is unchanged. Regression tests exercise repeated private replacement
including zero length, creation, recursive deletion with overlapping
selections, subsequent shared remastering, invalid paths, aliases,
staged cancellation and byte-identical optical metadata across the
MBR/GPT, embedded/external VAT and ISO/Joliet fixture matrix.

### Fixed-length independent ISO-only payloads in mixed VAT bridges

A pure batch of existing nonempty regular ISO-only files may be
replaced at exactly their original lengths, provided each is a unique,
single physical extent wholly before the primary UDF anchor at sector
256. Primary ISO and Joliet must independently agree on the selected
extent and namespace. No shared UDF file, directory, EFI partition,
boot resource, path table, VRS descriptor, or other file may overlap
a selected payload. The strict VAT parser provides a read-only proof
of the backing partition, descriptor sequences and current mapping
before and after staging. No VAT session is appended, no partition is
resized, and no UDF metadata or historical VAT payload is overwritten.
The transaction verifies every byte outside the selected payloads
and independently reads back the replacement sources before commit.

This is deliberately a restricted fixed-length profile, not an
ISO-only remasterer. Creation, deletion, resizing, payloads at or
after the primary UDF anchor, mixed independent/shared batches and
unsupported VAT or hybrid geometry remain outside this profile.
Regression tests exercise MBR/GPT, primary/Joliet, embedded/external
VAT, exact byte preservation, repeated shared remastering after an
ISO-only patch, cancellation and protected extent rejection. Physical
boot and Total Commander host behavior require separate verification.

### ISO-only subtree removal in mixed sequential MBR/GPT bridges

A pure removal batch may unlink existing independent ISO-only regular
files or recursively remove independent ISO-only directories. A shared
directory is not an independent target, even when it contains private
ISO children. Each explicit removal must exist, and overlapping parent
and child selections are coalesced after checking every selected path.
The input namespace and physical shared extents must meet the existing
partial VAT remaster profile. Any shared UDF descendant, alias of live
UDF data, or protected boot/partition target is rejected.

A VAT-only remaster contract passes zero UDF changes to the strict
sequential writer and reserves space for the new ISO/Joliet directories
and path tables. All UDF file layouts and virtual-to-physical mappings
remain unchanged; the new VAT ICB records the previous VAT. Historical
sectors and removed payload bytes remain intact, with the established
exceptions for relocated GPT/MBR metadata and planned ISO descriptor
fields. This is logical removal, not secure erasure or space reclamation.
The staged ISO namespace, both path-table orders, VAT layouts and
historical byte ranges are independently validated before commit.
The contract is rejected by the Type-1 writer.

The regression matrix adds nested/overlapping ISO-only removals,
removal under a shared directory, cancellation after append, ambiguous
mixed batches, aliases, unchanged VAT mappings, and a subsequent shared
append. The next profile extends this remaster to independent payload
replacement. ISO-only creation, mixed ISO-only/UDF/shared mutation
batches and unsupported map families remain outside the profile.

### Independent ISO-only payload resizing in sequential MBR/GPT bridges

Existing independent regular ISO-only files may also be replaced by
nonempty files of different lengths. A pure ISO-only batch may combine
these replacements with the existing ISO-only deletion profile.
The fixed-size pre-anchor route remains unchanged; a replacement of
a previously appended ISO-only payload uses the append route even
if its size is unchanged. The ISO coordinator reserves complete
payload sectors before the directory and path-table sectors within
the existing unmapped VAT reservation. No UDF file entry or virtual
mapping is created for an ISO-only file. Each payload is streamed into
verified zero-filled sectors, with zero padding in its final sector;
its planned ISO and Joliet records point to the new physical extent.
The final readback independently compares replacement data and padding
with the source files, in addition to the existing metadata, namespace,
partition-envelope and historical-byte checks.

The combined independent payload and ISO metadata reservation is
limited to 32768 optical blocks (64 MiB) and the remaining recorded
capacity of the backing partition. Empty replacement files, new
ISO-only names, mixed shared/private operations, and unsupported
geometry remain outside this profile. No original payload sector is
reclaimed or overwritten by this append operation. Regression tests
cover growth, shrinkage, a second equal-size replacement of appended
data, multiple independent replacements in one batch, unchanged VAT
mapping, capacity and budget failure, conflicting operations, and
cancellation, using both MBR/GPT and ISO/Joliet with embedded or external
VAT. These are image-level tests, not physical boot validation.

### Independent ISO-only file and directory creation

The sequential MBR/GPT remaster can also create nonempty files and
directories below an existing directory proven to be ISO-only in the
original namespace. Newly requested nested directories must have an
explicitly requested or existing parent. A pure ISO-only batch can
combine creation, replacement and recursive removal. Parent and child
selections are checked for conflicts before mutation; case-folded name
collisions, missing parents, shared or UDF-only targets, new root names
and descendants of deleted directories are rejected or routed through
the existing shared/UDF-only policy as appropriate. New files occupy
fresh unmapped reservation sectors and receive independently verified
primary and Joliet directory records. No UDF objects or virtual
mappings are created. The combined 64 MiB reservation limit and all
existing staged readback, partition and rollback checks remain.
Regression coverage adds nested creation, empty directories,
out-of-order parent selections, subsequent private edits, cancellation,
mixed-operation rejection and recursive deletion of newly created
ISO-only subtrees. Zero-length payloads and larger reservations remain
outside this profile.

### Streamed independent ISO-only payload reservations and empty files

The independent ISO-only remaster separates its data-sector count
from the directory and path-table reservation. The 32768-block
(64 MiB) metadata budget remains; independent payloads are instead
bounded by the remaining backing-partition capacity, the optical
block address space, and the single ISO extent length field
(0xffffffff bytes per file). Multi-extent ISO-only authoring above
that individual file limit remains unsupported.

The VAT append plan records the reservation as a 64-bit physical
range, not a full-size in-memory zero buffer. Staging and zero
readback use bounded chunks. The ISO coordinator streams replacement
and new file data, verifies each destination is zero-filled before
writing, and independently reads back file data and sector padding.
The subsequent VAT validation still checks all appended metadata
outside the explicit reservation and all historical protected bytes.
The Type-1 writer rejects this VAT-only payload contract.

Independent ISO-only files can now be empty, whether newly created,
truncated to zero, or replaced while empty. Their directory record
has a valid LBA and a zero length, allocating no data sectors.
The pre-anchor equal-size patch route remains restricted to nonempty
payloads; zero-length operations use the remaster route. Regression
cases cover empty creation, truncation, repeated empty replacement,
refilling a formerly empty file, insufficient partition capacity,
and >64 MiB payloads in MBR and GPT variants with preserved VAT maps.
Physical boot and host behavior remain separately unverified.

### Independent ISO Level-3 multi-extent files in mixed VAT bridges

The independent ISO-only append remaster supports files larger than
one 32-bit ISO record length. It plans block-aligned nonfinal sections
(up to 0xfffff800 bytes each) and a final section before computing
directory sizes, and assigns their physical addresses inside the
unmapped ISO reservation. Both primary ISO and Joliet serialize
consecutive records with the continuation flag set on nonfinal
sections only. Data and padding are streamed and read back under the
existing VAT-only contract.

Existing independent ISO-only chains, including noncontiguous sections,
retain their original raw continuation records in each view when
unrelated files are edited. Independent namespace checks compare every
section against the public reader; overlapping sections and aliases
of live UDF payloads are rejected. Replacement may shrink a chain to
one record, and recursive removal may unlink the entire chain. Shared
ISO/UDF files retain the existing single-extent policy. Existing
capacity, address-space, metadata budget, boot and partition protection
and historical byte checks remain mandatory.

The small regression matrix covers fragmented input chains, subsequent
shared and independent transactions, chain replacement, mismatched
Joliet records and later-section aliases. Separate Linux jobs exercise
a real payload exceeding 4 GiB in MBR and GPT sequential VAT bridges,
including primary and Joliet section records, independent xorriso
SHA-256 extraction, preserved UDF files and VAT mappings, subsequent
private-UDF replacement and ISO-only shrink or deletion. Physical boot
and Total Commander host behavior still require separate validation.
