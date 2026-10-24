# MDS v2 / MDX independent metadata decoder (experimental)

Sources reviewed on 2026-10-23:

- [Aaru issue #256](https://github.com/aaru-dps/Aaru/issues/256) ("Support MDSv2/MDX images"), open feature request. The discussion describes DAEMON Tools format changes, encrypted/obfuscated metadata, MDX's single-file container and caveats for password-protected/TAGES media.
- [Marisa-Chan/mdsx](https://github.com/Marisa-Chan/mdsx), especially src/mds.h, src/mds.c, src/decode.c, README and testdata. The author reports decoding MDSv2 and MDX, including non-password and compressed cases, but describes the tool as a research base, not a production MDSv1/ISO converter.
- [Aaru Alcohol120 code](https://github.com/aaru-dps/Aaru/blob/devel/Aaru.Images/Alcohol120/Constants.cs): MAXIMUM_SUPPORTED_VERSION = 1. Aaru does **not** currently implement MDSv2/MDX.
- The project also compared [libMirage](https://cdemu.sourceforge.io/pkg_libmirage.php) and the official 7-Zip ISO/UDF handlers for optical-sector layout.

## What is implemented in the experimental ISO-WCX preview

The native implementation has **two independent layers**, statically linked into
the Total Commander WCX plugin:

1. **Bounded descriptor authentication and decryption**
   (`src/mds_v2.cpp`). The MDS v2/MDX probe checks the encrypted header and
   embedded MDX extents. A 512-byte salt/header is decrypted with the format's
   password-less RIPEMD-160 PBKDF2 (2000 iterations) and AES-256 CBC variant,
   CRC-checked, then its AES-protected zlib descriptor is inflated (maximum
   16 MiB of descriptor, irrespective of a multi-gigabyte MDX file size).
   Developer command: `mds_descriptor_inspect IMAGE.mds|IMAGE.mdx --decode`.
2. **Bounded, random-access read-only optical track extraction**
   (`src/mds_v2_tracks.cpp`). The decrypted session/track/footer geometry
   selects the last track with a validated ISO 9660 PVD or UDF anchor. Plain,
   stored, repeated-byte and raw-DEFLATE compression groups are accessed
   on demand, including supported multiple data fragments. Validated
   Mode 1 / Mode 2 Form 1 user-data sectors are sent to the **same
   `Disc`/`PackedOptical` path** used by the WCX list/test/extract API.
   No intermediate ISO, external helper executable or new runtime DLL.

Existing MDS v1 support continues to enumerate session tracks; the v2
decoder is only used when the header identifies an MDS major version 2.
An MDX container is decoded directly from the original embedded data source.

**Tested interoperability:** the `mdsx-fixtures.yml` workflow downloads
content-addressed pinned fixtures at a fixed Marisa-Chan/mdsx revision,
verifies each Git blob hash, decrypts eight real metadata samples, and
compares actual extracted file bytes from plain and compressed MDSv2/MDF
and monolithic MDX to the independently parsed `mdxtest.iso` source.
The Windows Win32 and x64 jobs additionally load the actual WCX plugin,
walk `ReadHeaderExW` and extract the same files with `ProcessFileW`.
These are compatibility samples, not proof of all vendor variants.

**Explicit limits:** password-protected AES-LRW **track-data** encryption
is deliberately refused, even though password-less AES metadata decoding
works. Unverified proprietary track modes, CD-Audio extraction, complex
multisession merging, writing to MDF/MDX and raw track-data formats outside
the recognized Mode 1 / Mode 2 Form 1 subset remain unsupported.

The AES-256 block primitive comes from pinned public-domain tiny-AES-c;
the complete Unlicense is in `THIRD_PARTY_NOTICES.txt`. RIPEMD-160 and
PBKDF2 are independently implemented here. No TrueCrypt code was imported.

## Remaining compatibility work before a stable MDSv2/MDX release

1. **Provenance and licensing:** Although mdsx has a root **MIT** LICENSE,
   parts of its implementation originated in TrueCrypt 5.1a and carry their
   own **TrueCrypt License 2.4** notices; some AES routines also carry
   separate attribution terms. Do not copy its common/ or crypto/ directories
   directly into this WCX DLL under an assumption of MIT-only licensing.
   Any actual import requires a per-file review and notices.
2. **AES-LRW data-track support and uncommon layouts.** The reader now
   provides a separate bounded random-access stream for unencrypted plain
   and compressed groups. Password-protected LRW data, mode variants,
   damaged compression tables and additional subchannel encodings need
   further implementation and independent test vectors.
3. **Require a representative image corpus:** Non-password MDSv2, compressed
   and uncompressed MDS/MDF, monolithic MDX, multiple sessions/tracks,
   Mode 1/Mode 2 Form 1, mixed-mode discs, subchannels, a known-password
   test case, intentionally corrupt descriptors and unsupported TAGES cases.
   Use independently generated/test-authorized images and known-good
   SHA-256 digests rather than treating unit fixture names as coverage.
4. **Windows WCX compatibility:** Retain both Win32/Win64 ABI regression
   tests and transactional extraction. The preview installer associates
   `.mdx`; GUI-level acceptance and a representative real-disc corpus are
   still required before adding MDFv2/MDX to a Stable release.

The mdsx project itself describes its parser as research-grade; an open
Aaru issue does not imply full compatibility. ISO-WCX now has a native
**experimental MDSv2/MDX filesystem reader**, not a general-purpose full
DAEMON Tools-compatible drive emulator.
