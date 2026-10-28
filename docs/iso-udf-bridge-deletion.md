# ISO/UDF Bridge: restricted synchronized deletion (PR #67 Preview)

This is **not** a general ISO or UDF writer. The purpose is to avoid Total
Commander F8/Delete silently doing nothing when an image has both ISO9660
and UDF namespaces. The writer must never remove an ISO directory record
while leaving the matching UDF File Identifier Descriptor (FID) live.

## Safe subset

The preview deletes an **existing nonempty regular file**, when all of the
following hold:

- A raw 2048-byte/sector ISO image has a validated ISO9660 primary tree
  (and, when present, a matching Joliet tree), plus a strictly parsed
  single-partition UDF Type-1 tree.
- The file exists at the **same exact path** in ISO/Joliet and UDF.
- The file has exactly one shared physical data extent with the same
  length in both namespaces (nonempty UDF ShortAD; no sparse, embedded,
  aliased or multi-extent file).
- The source has no unsupported hybrid/system-area layout, Rock Ridge
  metadata, or El Torito boot catalogue.
- The request contains regular-file deletions only. Directory deletions,
  replacing a file and deleting one in the same operation, and newly
  created paths are refused.

The editor validates both original trees before creating a private sibling
copy of the ISO. It then compacts the removed ISO/Joliet directory records
**within the same original sectors**, preserving all later entries in
that sector and leaving directory lengths, path tables, volume descriptor
geometry, and file data locations unchanged. It sets the UDF FID deleted
bit, recalculates that descriptor's CRC and, for an embedded parent
directory, its parent FE descriptor CRC as well.

Before committing, the writer strictly reparses the UDF metadata,
independently parses the ISO/Joliet tree, confirms all requested files
disappeared from both and all remaining ISO paths/extents are unchanged.
The transaction then uses the normal atomic archive replacement and
source-identity checks. **No data blocks are reclaimed or zeroed**:
those blocks may still be referenced by ISO metadata, so freeing them
would be unsafe. This is a logical deletion, not a space-saving
recompression operation.

## Still intentionally unsupported

- **Bootable ISO/UDF Bridge**, hybrid MBR/GPT/system area, Rock Ridge and
  unknown filesystem extensions.
- Deleting a **directory**, even when empty; a matching recursive UDF
  directory writer and parent/FID reclamation are not implemented.
- Different ISO and UDF extents, multi-run/embedded/sparse file data,
  unreadable UDF CRC, hardlinks/aliases, different path spellings and
  unsupported UDF variants (VAT/metadata/sparable/multiple partitions).
- General file creation, arbitrary-size replacement, UDF repartitioning,
  full image repacking, and UDF block-space reclamation.

These conditions return a WCX error without modifying the original;
the interactive Total Commander host displays a specific reason.
The association value `222` in `wincmd.ini` is a normal WCX capability
bit mask, **not an error code**.

## How to verify in Total Commander

Install the matching **Win32 or x64 Preview** `ISO-WCX.wcx` /
`ISO-WCX.wcx64`, then fully restart Total Commander. Work with a **copy**
of your ISO until you have validated its behavior. Open the ISO through
the plugin, select a regular file and press F8. On a compatible ISO/UDF
Bridge, the file should disappear; reopen the image and confirm it is
absent. On an unsupported layout, the plugin refuses the edit with a
reason rather than falsely reporting success.

The CI gates include:
- `udf_bridge_same_length_edit` — synthetic shared-extent edits,
  synchronized ISO+UDF logical deletion, dual independent readback,
  preserved following file, unchanged unrelated sectors, rollback and
  deliberate rejection of unsafe fixtures.
- `wcx_delete_iso_udf_bridge_dual_namespace` (Windows Win32 and x64) —
  calls the actual `DeleteFilesW` export in a same-architecture process
  and verifies both namespaces independently.

These tests are not proof that every real-world bootable UDF hybrid
can be safely rewritten. Such layouts remain refused by design.
