# Optical CD/DVD lessons from 7-Zip (independent ISO-WCX implementation)

This note evaluates the official `ip7z/7zip` sources, especially:
- `CPP/7zip/Archive/Iso/IsoIn.cpp`, `IsoHandler.cpp`, `IsoItem.h`
- `CPP/7zip/Archive/Udf/UdfIn.cpp`, `UdfHandler.cpp`

## Useful patterns incorporated

1. **Separate filesystem parsing from CD container layout.** 2048-byte
   ISO sectors, raw 2352-byte Mode 1/Mode 2, 2336-byte Form 1 and CUE/NRG/MDS
   session offsets remain distinct from ISO9660/Rock Ridge/UDF directory
   parsing. The public WCX extraction adapter operates on validated extents.
2. **Treat RRIP metadata as on-disc metadata, not a host instruction.**
   PX mode fields are both-endian and are range-checked; read-only POSIX
   permission maps to Windows Read-only. TF modification timestamps decode
   to the WCX/DOS timestamp (whose representation has no timezone). Symbolic
   links and device nodes are not materialized as regular extracted files.
3. **Treat SUSP CE pointers as untrusted disk extents.** Cross-check both
   endian copies for every CE block, offset and size; retain hop limits and
   visited-pointer cycle rejection. Tests include corrupt mirrored fields.
4. **Keep corruption handling local.** Broken optional boot metadata must not
   hide readable ISO files; malformed core directory records must not turn
   into fabricated files. Preserve Joliet-to-ISO fallback only when the
   primary directory tree validates independently.
5. **UDF uses validated descriptor tags and allocation descriptors.**
   Existing ISO-WCX parsing checks tag checksum, descriptor CRC16, extent
   bounds and continuation traversal. Metadata/VAT/Sparable remains
   experimental until tested with independent real media.

## Remaining work: not advertised as supported

- Alcohol MDS **v2** / MDX, ISZ, DAA, CHD compressed formats.
- Full CD-Audio, CD-Text, CD Mode 2 Form 2 and merged multisession UDF.
- RRIP timestamps other than modification, POSIX xattrs/ACLs and symlink
  extraction (intentionally disabled).
- Full UDF 1.02–2.60 feature and optical drive compatibility coverage.

## License boundary

7-Zip's main archive sources are largely LGPL-2.1-or-later, with
module-specific exceptions. Nothing in this change copies, vendors or links
7-Zip source; it is an independently authored implementation inspired by
public filesystem layouts and validation principles. Changes should not
be called stable until Windows WCX x86/x64, Linux CI, sanitizer and real
image tests succeed. See `PROVENANCE.md`.
