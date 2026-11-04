# ISO editing through Total Commander (design and current status)

**Goal:** F5 add/replace, Delete, and F7 create-directory from inside Total
Commander for supported optical images. No separate mount program, command
script, external WCX editor or runtime DLL. Never silently corrupt
BIOS/UEFI bootability, ISO9660/Joliet, UDF, Rock Ridge or hybrid
USB/CD metadata.

## Current safe capabilities

| Optical layout | Add/delete/resize | Same-length replacement |
| --- | --- | --- |
| Validated nonhybrid ISO9660 / Joliet, optional El Torito | Native transactional rebuild, subject to boot-extent and name checks | Native transactional rebuild |
| Same layout with a bounded **all-zero** trailing pad | Preview native rebuild; PVD/Joliet sizes are synchronized | Preview native rebuild |
| Standalone UDF Type-1 2048-byte images | Limited native writer Preview, gated by UDF format validations | Limited native writer Preview |
| ISO/UDF Bridge without boot catalog | Not supported | Preview: single contiguous existing UDF or shared ISO/UDF file, exactly same length; both namespaces validated |
| Bootable ISO/UDF Bridge | **Preview:** UDF-only F5 add, delete, F7 and bounded resize when strict native Type-1 bitmap/FE/FID/LVID validation passes and the ISO namespace is immutable | Existing contiguous UDF-only or shared-extent equal-size payload replacement; embedded/complex replacements route to the protected UDF writer |
| Hybrid MBR/GPT or nonzero system area | Not supported | Currently blocked; never discard boot/partition metadata |
| Rock Ridge, multi-extent and unrecognized variants | Not supported | Currently blocked if parser cannot guarantee all metadata is preserved |

These are *capability preconditions*, not blanket promises for any disc.
For individual archive failures, the WCX adapter records the native refusal
reason in `%LOCALAPPDATA%\ISO-WCX\Logs\edit-errors.log`.

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

**Not yet available:** F5 to create a new file, Delete, folder changes,
or changes to file length in this bootable ISO/UDF Bridge. Those require
a coordinated UDF allocation/FID/FE writer with boot-preserving validation,
not merely a raw payload patch. This capability must be tested on a
disposable copy of the actual AOMEI ISO before general availability.

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
inconsistent writable bitmap, unbounded boot-image extents, and hybrid
MBR/GPT remain **read-only**.

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
