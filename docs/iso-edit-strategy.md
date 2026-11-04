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
| ISO/UDF Bridge without boot catalog | Not supported | Limited shared-single-extent replacement only; both namespaces validated |
| Bootable ISO/UDF Bridge | Not supported | Currently blocked pending boot-range checks |
| Hybrid MBR/GPT or nonzero system area | Not supported | Currently blocked; never discard boot/partition metadata |
| Rock Ridge, multi-extent and unrecognized variants | Not supported | Currently blocked if parser cannot guarantee all metadata is preserved |

These are *capability preconditions*, not blanket promises for any disc.
For individual archive failures, the WCX adapter records the native refusal
reason in `%LOCALAPPDATA%\ISO-WCX\Logs\edit-errors.log`.

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
