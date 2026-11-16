# ISO editing through Total Commander (design and current status)

**Goal:** F5 add/replace, Delete, and F7 create-directory from inside Total
Commander for supported optical images. No separate mount program, command
script, external WCX editor or runtime DLL. Never silently corrupt
BIOS/UEFI bootability, ISO9660/Joliet, UDF, Rock Ridge or hybrid
USB/CD metadata.

## Current safe capabilities

| Optical layout | Add/delete/resize | Same-length replacement |
| --- | --- | --- |
| Validated nonhybrid ISO9660 / Joliet, optional no-emulation El Torito | Native transactional rebuild, subject to boot-extent and name checks | Native transactional rebuild |
| Nonhybrid ISO9660 / Joliet + El Torito floppy/HDD emulation | **Preview:** add/delete/replace unrelated files only, with full boot-disk extent and MBR safety checks | Same limits; boot image files themselves immutable |
| Same layout with a bounded **all-zero** trailing pad | Preview native rebuild; PVD/Joliet sizes are synchronized | Preview native rebuild |
| Bounded but **unequal** Primary/Joliet volume sizes | Preview rebuild only after each namespace tree and path table is validated against its own descriptor; output sizes normalized | Same validation |
| Standalone UDF Type-1 2048-byte images | Preview F5, F7, Delete files and **empty directories**, gated by strict bitmap/FE/FID/LVID validation | Limited native writer Preview |
| ISO/UDF Bridge without boot catalog | Not supported | Preview: single contiguous existing UDF or shared ISO/UDF file, exactly same length; both namespaces validated |
| Bootable ISO/UDF Bridge | **Preview:** UDF-only F5 add, Delete files/**empty UDF directories**, F7 and bounded resize when strict Type-1 bitmap/FE/FID/LVID validation passes and ISO/El Torito remain immutable | Existing contiguous UDF-only or shared-extent equal-size payload replacement; embedded/complex replacements route to the protected UDF writer |
| Validated conventional **MBR-isohybrid ISO-only** (no UDF) | Blocked: a partition-aware ISO remasterer is required | **Preview:** existing non-boot ISO-visible file, same size only; byte-exact MBR/El Torito/metadata preservation |
| Validated conventional MBR-isohybrid ISO/UDF Bridge | Metadata edits, adds, deletes and resize blocked | Preview: same-size replacement of a contiguous existing UDF-only file, after protected-region validation |
| GPT-hybrid or unknown nonzero system area | Not supported | Not supported; preserve boot/partition metadata |
| Rock Ridge, multi-extent and unrecognized variants | Not supported | Currently blocked if parser cannot guarantee all metadata is preserved |

These are *capability preconditions*, not blanket promises for any disc.
For individual archive failures, the WCX adapter records the native refusal
reason in `%LOCALAPPDATA%\ISO-WCX\Logs\edit-errors.log`.

## ISO-only MBR-isohybrid same-length F5 (Preview)

The ISO9660/Joliet writer now recognizes a conventional MBR-isohybrid
**without UDF** as a restricted payload-patching case. An ISO rebuild or
append is unsafe because it can change the extent map without updating
the image's MBR partitions. Only an existing ordinary ISO-visible file
may be replaced, with **exactly the same size**. F5 addition, Delete,
directory creation, resize, shared aliases and boot-image replacement
remain refused; unknown/GPT system areas are still unsupported.

The candidate target must have a unique, bounded file extent, no overlap
with any ISO directory/other file, path table, volume descriptor or
El Torito boot range. The entire original optical image is copied to a
private sibling transaction and only the intended payload extents are
patched. Both the staged ISO and its boot references are reparsed;
all bytes outside the changed file ranges (including the full 32-KiB
system area, MBR, vendor sectors, boot catalog and unused padding) are
compared against the original. Changed bytes are independently compared
with every F5 source stream before atomic commit. Cancellation and any
verification failure discard the private stage without modifying the
original. Windows' source-identity guard remains mandatory.

Synthetic ISO-only MBR and BIOS/EFI El Torito scenarios are covered,
including a previously rewritten ISO with path tables, readback,
same-size enforcement, multi-file batch rollback and refusal to touch
an emulated boot floppy. Real USB/BIOS/UEFI startup of vendor media
still requires a separate field test and is **not certified**.

## Shared source-mutation guard for ISO/WIM (Preview)

Atomic staging now snapshots the original Windows file's volume serial,
file ID and NTFS `ChangeTime` in addition to its size and ordinary
`LastWriteTime`. These fields are rechecked immediately before replacing
the original. This detects common same-size external rewrites with restored
timestamps, and a different archive substituted under the same filename.
On unsupported/non-NTFS volumes the identity query may safely refuse an
edit; it does not silently disable this check.

The guard does **not** promise immunity from a malicious writer changing
the file during the last two system calls: this is not an exclusive file
lock, full multi-GB SHA-256 or TxF transaction. An unchanged-image test,
cancelled edit, in-place mutation, same-timestamp path swap and private
workspace cleanup are covered on Windows x86/x64, without mounting real
WIMs. Diagnostics use the fixed category `archive_source_changed` without
sharing private image names.

## ISO/Joliet full F5 data verification (v0.21.2 Preview)

A staged plain ISO edit is checked not only for valid ISO/Joliet directory
metadata, but also against **every requested F5 source file**. The writer
resolves each requested filename from the independently parsed staged tree,
verifies its type/length, and compares the full on-disc file extent in
bounded 64 KiB chunks against the source. Missing files, changed source
lengths, wrong bytes, duplicate/case-colliding targets and a
delete-and-recreate-under-removed-directory batch fail before atomic commit.
Deletion and directory creation are also checked against the staged tree.
These checks are additive to the byte-for-byte El Torito and system-area
guards. The ISO/UDF Type-1 writer keeps its existing independent staged
UDF payload readback and boot-extent verifier.

## El Torito floppy and hard-disk emulation (v0.21.2 Preview)

The plain ISO9660/Joliet writer no longer rejects **El Torito media types
1/2/3/4** merely because the boot catalog declares disk emulation.
This is not permission to edit the boot image itself:

- Floppy emulation uses the **exact** 1.2, 1.44 or 2.88 MiB media
  size. The full disk may be an ISO-visible file **or a hidden extent**,
  provided it is completely inside the declared volume and does not
  overlap any ISO file/directory, path table, descriptor, UDF metadata
  or another boot resource. All floppy bytes remain immutable.
- Hard-disk emulation still requires an ISO-visible, 512-byte-aligned
  full file with a valid MBR and bounded non-GPT partition entries.
  **Hidden HDD images remain read-only:** their length is unknown.
- El Torito's `SectorCount` is only the initial 512-byte BIOS load. It
  does **not** bound the full emulated disk. Conflicting, out-of-range
  or otherwise ambiguous images are rejected before editing.
- Original boot descriptor, raw boot catalog, system area and the **entire
  bounded emulated floppy/HDD extent** are immutable. The staged ISO is
  re-parsed and checked against the original protected byte ranges before
  the final atomic replacement.
- Unrelated ISO-visible F5 add/replace, Delete and directory edits may
  work after all ISO/Joliet and boot preflight validations pass. The
  referenced boot image file itself cannot be replaced or removed.
- Rock Ridge, ISO/UDF Bridge metadata remaster, GPT/isohybrid layouts,
  unknown system area and unverifiable boot metadata remain guarded.

Tests include independently generated emulation catalogs from xorriso,
the native ISO editor's transactional rollback and Win32/x64 WCX runs.
**This is still a Draft Preview**: preserving byte-identical boot data
cannot certify bootability of a modified real machine image.

## Field-reported ISO F5 restrictions (RC1 B1412)

The sanitized field report showed two independent WCX 24 refusals:
`emulated El Torito boot image is not an ISO-visible file` and
`ISO primary and Joliet volume sizes disagree`. Neither is a Total
Commander dispatch failure. A hidden **floppy** can now be safely
bounded by its media type and protected byte-for-byte during an unrelated
ISO rebuild; a hidden **HDD** remains refused. Independently valid
Primary/Joliet volume sizes may differ, but each descriptor's directory
tree and path tables must fit **inside its own declared volume**. The
transaction copies the complete physical image, rebuilds both namespaces,
normalizes their volume-size fields, verifies F5 payloads and original
boot bytes, and only then atomically commits.

Synthetic plain ISO and native Win32/x64 WCX tests cover both accepted
cases and rejection of truncated views, overlapping hidden boot files,
out-of-range path tables and unbounded HDD emulation. Real BIOS/UEFI
bootability of modified vendor ISOs still requires separate validation.

## Empty UDF directory Delete (v0.21.2 Preview)

Total Commander Delete can now remove a **UDF-only, already empty**
directory. The native Type-1 writer preserves the original parent FID
record length and marks it deleted, clears its obsolete ICB pointer,
checks the directory has no live file/subdirectory descendants, and
reclaims its uniquely owned FE and ShortAD blocks in the Space Bitmap.
The LVID free-block and (where recorded) directory counts are updated
atomically. Freed sectors are securely zeroed **only in the staged
image** and are not reused during the same transaction; independent
UDF/ISO readback and the immutable El Torito guard run before commit.

Nonempty directories, the UDF root, a directory with ambiguous ICB
ownership, an unavailable bitmap/LVID, and an ISO-visible boot directory
remain read-only. Recursive deletion is **not** enabled. Refused and
cancelled operations must leave the input byte-identical and create
no persistent workspace. Regression tests cover nested and external
parents, mixed F5+Delete, and bootable ISO/UDF Bridge.

## ISO/UDF Bridge protected allocator (v0.21.2 Preview)

The UDF Type-1 bitmap can legitimately list a sector as free **for UDF**
while an ISO9660 file, Boot Catalog, BIOS/EFI image, path table or directory
still occupies those same physical bytes. Previously a new UDF-only file
could pick such a sector, copy and edit the full image, then fail the final
optical guard because ISO metadata changed.

For Bridge F5/F7 and safe UDF-only changes, the ISO parser now feeds the
Type-1 allocator immutable physical ranges (system area, descriptors,
complete ISO-visible file/directory extents, El Torito catalog/boot spans,
and UDF anchor/VDS ranges). The allocator maps them to partition blocks,
sorts and merges the intervals and skips those blocks for *both* contiguous
FE/directory allocation and fragmented multi-ShortAD file allocation.

This is an **in-memory exclusion only**: no ISO/UDF namespace, original UDF
free-space bitmap or LVID count is rewritten to claim those blocks as used.
Only genuinely allocated UDF blocks are subtracted from free counts.
An existing UDF-only file whose FE or data extent overlaps an ISO-protected
range is rejected before writing, even if the UDF path has a different name.
The original byte-for-byte staged ISO/El Torito verifier remains mandatory.

The allocator now supports a reproducible synthetic bridge where the first
UDF-free sector belongs to ISO; it skips that sector and successfully adds
a UDF-only file elsewhere. A second fixture with *only* ISO-owned free
sectors must fail without a staged edit or source modification. This does
**not** enable ISO-visible metadata updates, real-world WinPE boot
certification, unsupported bitmap structures or GPT/isohybrid remastering.

## AOMEI Partition Assistant 10.12.2 WinPE field-report profile (2026-11-04)

The user's **read-only Export Image Structure** report describes a
781,320,192-byte (381,504-sector) ISO/UDF Bridge with a PVD declaring
381,213 sectors, El Torito boot catalog at LBA 349, UDF NSR02,
anchors at LBAs 256, 381,248 and 381,503, and **UDF** as the selected
filesystem. The ISO9660 primary namespace contains only
`boot.images/boot.catalog`, `BIOS-0.img` and `EFI-1.img`; the actual
WinPE files, including `sources/boot.wim`, reside exclusively in UDF.
A pure-ISO editor therefore cannot F5 these files.

The native payload-only bridge editor now recognizes existing files that
exist **only in UDF**. It requires exactly one contiguous nonsparse extent,
the same replacement length, an unchanged ISO9660 namespace, and strict
non-overlap with every ISO namespace extent, bounded El Torito boot-image
range, ISO/UDF descriptor and UDF peer-file extent. It stages an isolated
ISO copy, verifies both namespace parsers and exact replacement bytes,
then commits atomically. In a bootable ISO with zero or ambiguous El Torito
sector counts, editing remains disabled until the complete boot image
extents can be independently established.

**Preview support now implemented:** F5 new UDF-only files, F7 new
empty directories, Delete regular UDF files and file-size-changing
replacements are routed into the native Type-1 FE/FID/bitmap/LVID writer
when its strict parser and allocation checks succeed. The ISO9660 namespace
remains immutable, and every staged transaction must pass a byte-for-byte
El Torito/system-area/ISO namespace guard before commit. Deleting directories
and changing ISO-visible paths remain unsupported.

The actual AOMEI ISO has not been checked for a writable UDF bitmap, Type-1
partition map, or boot-extent compatibility. **Export Image Structure**
now includes `UDF NATIVE EDIT PREFLIGHT (READ ONLY)`, which reports
whether the parser accepts its metadata and whether a writable allocator
bitmap exists. A real-image boot test is mandatory before Stable.

## Universal Optical Edit Guard (Preview)

`include/iso_wcx/optical_edit_guard.hpp` is a reusable native validator
for immutable byte ranges. Each filesystem writer registers its protected
areas, then the guard compares the staged output byte-for-byte with the
original before the atomic commit, with cancellation and rollback.

For a bootable **ISO/UDF Bridge**, the UDF Type-1 writer is now connected
to this guard via a mandatory staged-image callback. New UDF-only files,
logical file deletion, directory creation and bounded replacement/resizing
are attempted by the existing native FE/FID/bitmap/LVID engine. Before
commit, the callback additionally reparses ISO9660, checks unchanged
descriptors and tree extents, and compares the full protected BIOS/UEFI
boot image/catalog, system area, ISO namespace data, UDF anchors and
volume descriptor sequences. The *original ISO remains untouched* if
any preflight or stage check fails.

El Torito's `SectorCount` describes initial loaded 512-byte sectors, **not
necessarily the full boot image size**. The guard always protects full
ISO9660 file extents containing a referenced boot LBA. A zero count
can be accepted only if that full extent is independently resolved
from the ISO file namespace; otherwise the write fails closed. Rock
Ridge/SUSP records may be carried through unchanged on a UDF-only edit,
but the module never regenerates RRIP or edits an ISO-visible file.

This is a constrained capability, not blanket rewrite support.
ISO-visible additions/deletions, modifications of ISO-shared names,
unsupported UDF allocation maps (VAT/Sparable/Metadata), a missing or
inconsistent writable bitmap, unbounded boot-image extents, and GPT/unknown hybrid layouts remain
**read-only**. A conventional validated MBR-isohybrid Bridge has the strictly
bounded existing UDF-only equal-length payload exception described above.

The synthetic dual-namespace test `universal_iso_udf_boot_preserving_edit`
and the format-agnostic guard test run on CI. A boot test on a
disposable copy of the real AOMEI WinPE image is still necessary before
a Stable release.

## Native editor decision flow (no mount tools)

1. Detect the optical layout from content: primary descriptor, Joliet, UDF
   VRS/AVDP, El Torito boot catalog, system-area/partition structures and
   trailing bytes. **Never** infer writability from an `.iso` extension.
2. Validate all directory entries, extent bounds, namespace aliases and
   boot ranges. Refuse ambiguous or overlapping extents; reject reparse
   and unsafe source paths.
3. Route to an existing safe native writer. A new file or changed length in
   ISO/UDF Bridge requires a **dual-namespace writer**, not an ISO-only hack.
4. Stage output beside the original (same volume). Apply all changes to
   the stage and preserve reserved sectors/boot information.
5. Reopen staged output; independently validate namespace trees, exact
   requested file bytes, volume descriptors, directory records, allocation
   structures and boot signatures before atomic commit.
6. Report progress/cancellation through WCX. Keep the original unchanged on
   unsupported inputs, cancellation and verification failure.

## El Torito floppy and hard-disk emulation (experimental Preview)

The plain ISO9660/Joliet native rebuild now recognizes boot catalog media types
1 (1.2 MiB floppy), 2 (1.44 MiB floppy), 3 (2.88 MiB floppy), and 4
(hard-disk emulation), in addition to no-emulation type 0. Previously F5 was
immediately refused on any emulated boot entry.

An emulated boot LBA **must** be the first sector of a regular ISO9660 file
extent; a hidden, ambiguous or partially mapped disk is not writable. Floppy
file size must precisely match the declared media type. Hard-disk images must
be 512-byte aligned, carry a valid MBR signature and have bounded, consistent
MBR primary partition entries; protective GPT and malformed partitions are
not accepted. The boot catalog's 512-byte SectorCount records initial loader
sectors only, **never** the full floppy/HDD disk length.

Only edits to unrelated ISO files and directories are permitted. Boot image
replacement, deletion, or edits to a containing directory are still refused.
The append-only writer compares the original system area, boot descriptors,
boot catalog, **entire emulated boot image file extents** and initial loader
ranges byte-for-byte before commit. This works with an ISO9660/Joliet image
only; ISO/UDF Bridge, Rock Ridge and hybrid system areas keep their existing
stricter constraints. Reparse and boundedness failures leave the original
image unchanged.

Synthetic regressions cover positive unrelated-file F5 for all four media
types and negative tests for unlisted boot images, wrong floppy size, missing
HDD MBR, out-of-bounds MBR partitions, and attempts to modify boot images.
These prove guarded byte preservation, not real BIOS/UEFI bootability. The
feature must remain Preview pending CI and disposable-machine boot testing.

## Independently mastered El Torito regression

The Linux Preview CI now uses a **test-only** xorriso binary to create a
disposable ISO9660/Joliet image with separate BIOS and EFI El Torito entries.
The default Rock Ridge/SUSP variant must refuse editing without modifying
the original; xorriso's documented `--norock` fixture produces the separately
supported ISO/Joliet variant. This does not enable Rock Ridge editing.
`independent_xorriso_dual_boot_iso_edit` verifies native F5 add/replace,
Delete, cancellation and rejection of boot-file edits. It compares the
raw El Torito catalog, BIOS and EFI payloads before/after each operation,
and independently reads edited files through xorriso. This cannot establish
BIOS or UEFI bootability of customer WinPE media; that still needs a real
disposable-machine test.

## Remaining work for full bootable ISO/UDF editing

- Author or integrate a **native synchronized ISO9660/Joliet/UDF metadata
  writer**: path tables, UDF FE/EFE/FID, logical volumes, partition bitmap,
  integrity/CRC and shared file data extents. Verify both namespace
  representations of each edited file.
- Preserve El Torito BIOS and EFI boot images/catalog; independently parse
  all referenced boot extents, not just starting sectors. Verify boot
  descriptors and retained payload hashes.
- Handle hybrid MBR/GPT/isohybrid alignment and backup GPT references, and
  intentionally reject layouts that cannot be rebuilt without changing
  their boot-chain semantics.
- Support Rock Ridge only after its SUSP/RRIP entries and relocated
  directories are safely regenerated; no symlink/device extraction.
- Validate on real WinPE and installer images **in disposable copies**.
  Tests must cover F5, replace, delete, nested directories, UTF-8,
  cancellation, long paths, low disk space and preserved bootability.

## Diagnose a specific ISO

In Total Commander open **Files → Pack → Configure… → Export Image Structure**,
choose the ISO, and review its read-only metadata report. The report now
includes declared-vs-physical ISO volume size, bounded trailer classification,
UDF anchor candidates including declared-volume boundaries and a cautious
editing-route hint. It never contains payloads, but does contain file paths;
review sensitive paths before sharing.

A mismatch between declared ISO9660 size and physical file length is **not**
proof of safe padding: only a fully inspected, bounded all-zero trailer can
enter the native ISO rebuild path. Nonzero trailer or UDF/hybrid boot
metadata always keep the stricter safeguards.

Do **not** publish this preview as Stable until Windows x86/x64 and
real-image verification are green.
