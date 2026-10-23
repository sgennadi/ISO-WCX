# Native optical container readers in ISO-WCX v0.14 Preview

These are read-only, random-access block decoders compiled into the same
Total Commander WCX binary as the ISO9660, Rock Ridge, Joliet and UDF parser.
No external DLL, mount, virtual drive, unpacker or intermediate ISO is used.

| Format | Currently implemented | Explicitly rejected |
| --- | --- | --- |
| ISZ v1 | 2048-byte sectors; one file; 3-byte XOR pointer index; sparse zero, raw, zlib and bzip2 chunks | Encryption/passwords, non-first segment, multiple segments, other geometry |
| DAA 1.00 | 76-byte header with CRC32, 3-byte size table, raw-DEFLATE compressed hunks | DAA 1.10, encryption, other chunk types and multipart headers |
| CHD v5 | Statically linked libchdr compressed maps, zlib/LZMA/Zstd/FLAC/CD codecs; cooked 2048-byte ISO/UDF plus single-track 2448-byte Mode 1/2 Form 1 CD data (experimental) | Audio, mixed/multi-track layouts, parents, unrecognized sector and metadata formats |

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

## Compressed CHD integration

CHD v5 decoding uses the BSD-3-Clause `libchdr` implementation pinned at
`47d794151b2b64f6aba60304e6480f4f5a2bc24b` and built entirely into the
WCX DLL. Its miniz, Zstandard, LZMA and dr_flac dependencies are also static.
The adapter deliberately exposes only ISO/UDF-bearing CD/DVD data tracks,
with strict CHD map/CRC and logical-geometry validation.

MAME `chdman` is used **only inside development CI** to produce independent
zlib/LZMA DVD ISO/UDF and CD images. The WCX preview workflow also passes
real chdman-generated compressed DVD LZMA and CD CD-LZMA fixtures to both
Windows Win32 and x64 runners, where the exported WCX APIs are used to
list and extract `HELLO.TXT` and verify its bytes. CI fixture generation
is not a runtime tool, and test EXEs are never packaged. No end user installs or runs it. The
WCX installer contains no libchdr DLL, mounted disk, helper or ISO staging file.

Compressed CD support is currently experimental until verified against a wider
collection of authentic multi-session and vendor CHD images.
