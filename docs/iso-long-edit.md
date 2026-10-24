# ISO9660/Joliet long-name editing and El Torito preservation (Preview)

The native WCX transactional ISO editor supports ordinary 2048-byte
ISO9660/Joliet images without system-area hybrid data or Rock Ridge/UDF
bridges. It rebuilds both ISO directory trees and endian-specific ISO path
tables, verifies the staged result and replaces the input only on success.

## Long names

- Primary ISO identifiers: up to **221 bytes**, including ` ;1` when a
  file-version suffix is used. This is a relaxed ISO Long(221) mode, not a
  promise of compatibility with all ISO9660 readers.
- Joliet: up to **110 UTF-16 code units** including the file-version suffix
  (Long(110)); a non-BMP symbol occupies two code units. Valid UTF-8 is
  decoded into UTF-16 and re-encoded for names, including surrogate pairs.
  Relaxed Long(110) is not universally accepted by ISO/Joliet implementations.
- Names unrepresentable in primary ISO get a deterministic, collision-checked
  `WCX_XXXXXXXXXXXXXXXX` ASCII alias with optional `.BIN` suffix. File bytes
  are shared between ISO and Joliet directory views.
- Invalid UTF-8, invalid surrogate pairs, unsupported path components,
  filename overflow and ambiguous namespace mappings are rejected without
  modifying the original ISO.

## Boot images

Existing supported El Torito catalogs and BIOS/UEFI boot images are copied
byte-for-byte when new file data and directory structures are appended. The
editor refuses replacements or deletions of any file whose original extent
overlaps a referenced boot image or boot catalog sector (not just files whose
first LBA matches). It does **not** create new boot entries or modify complex
unsupported catalogs.

## ISO/UDF Bridge boundary

**UDF and ISO/UDF Bridge writes are not implemented in this release.**
The current ISO writer deliberately rejects UDF Volume Recognition Sequence
markers `BEA01`, `NSR02` and `NSR03` in the scanned descriptor area. A
future UDF writer must update UDF anchors, VDS/partition metadata, FE/EFE,
FID trees, mapping tables, allocation state and independently validate both
ISO and UDF views. Updating only ISO descriptors would be unsafe and is
not offered. Existing supported UDF images remain read-only.

## Validation

CTest generates synthetic ISO Long(221), Joliet Long(110), non-BMP Unicode,
Unicode folder-name collisions, dual El Torito BIOS/EFI boot sectors,
filesystem rejection and transactional rollback tests. These are preview
fixtures; manually verify representative real media before relying on edits.
