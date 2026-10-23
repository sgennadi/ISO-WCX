# Native optical container readers in ISO-WCX v0.14 Preview

These are read-only, random-access block decoders compiled into the same
Total Commander WCX binary as the ISO9660, Rock Ridge, Joliet and UDF parser.
No external DLL, mount, virtual drive, unpacker or intermediate ISO is used.

| Format | Currently implemented | Explicitly rejected |
| --- | --- | --- |
| ISZ v1 | 2048-byte sectors; one file; 3-byte XOR pointer index; sparse zero, raw, zlib and bzip2 chunks | Encryption/passwords, non-first segment, multiple segments, other geometry |
| DAA 1.00 | 76-byte header with CRC32, 3-byte size table, raw-DEFLATE compressed hunks | DAA 1.10, encryption, other chunk types and multipart headers |
| CHD v5 | Uncompressed 4-byte hunk map, 2048-byte optical units, sparse hunks | Any compressed codec or entropy map, 2352/2448 CD frames, parent CHDs, audio and multi-track formats |

All formats have bounded logical size, bounded chunk sizes and counts, checked
64-bit offsets before seeking, a single decoded-block cache and the existing
transactional WCX extraction/cancellation flow. Unsupported variants return a
clear format error rather than being treated as uncompressed ISO data.

Testing uses independently generated synthetic ISO/UDF fixtures and mutations
for damaged signatures, maps, truncated chunks, CRC or unexpected codecs.
This is **not** equivalent to broad vendor-image interoperability.

Upstream factual references (no image-handler code imported):
- https://github.com/oserres/isz-tool
- https://github.com/rsenn/daa2iso
- https://github.com/rtissera/libchdr

The pinned bzip2/libbzip2 1.0.8 compression implementation is permissively
licensed and statically compiled into the WCX binaries at build time.
