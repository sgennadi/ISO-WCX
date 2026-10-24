# Native optical container readers in ISO-WCX Preview (post-v0.16)

These are read-only, random-access block decoders compiled into the same
Total Commander WCX binary as the ISO9660, Rock Ridge, Joliet and UDF parser.
No external DLL, mount, virtual drive, unpacker or intermediate ISO is used.

| Format | Currently implemented | Explicitly rejected |
| --- | --- | --- |
| ISZ v1 | 2048-byte sectors, 48/64-byte headers; 2/3/4-byte XOR pointer indexes or implicit raw; sparse zero, raw, zlib and bzip2; 2/3-part synthetic split tests across `.i01` and `.part02.isz` parts with native random access | Encryption/passwords, opening a non-first part directly, unsupported geometry or unknown vendor variants |
| DAA 1.00 | 76-byte header with CRC32, 3-byte size table, raw-DEFLATE compressed hunks | DAA 1.10, encryption, other chunk types and multipart headers |
| CHD v5 | Statically linked libchdr compressed maps and zlib/LZMA/Zstd/FLAC/CD codecs; cooked 2048-byte ISO/UDF, validated mixed/multi-track 2448-byte Mode 1/2 Form 1 CD data, and virtual `CDDA/trackNN.wav` from audio-only or mixed CHDs (16-bit stereo PCM, RIFF/RF64). | CD-Text, CD+G graphics, parent CHDs, unsupported sector layouts, full multisession merging or editing. |

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
The adapter exposes ISO/UDF-bearing CD/DVD data tracks and CDDA audio tracks
as generated WAV/RF64 files, with strict CHD map/CRC, four-frame padding and
logical-geometry validation.

MAME `chdman` is used **only inside development CI** to produce independent
zlib/LZMA DVD ISO/UDF and mixed CD/CDDA images. Independent fixtures compare
all WAV PCM sample bytes and CD filesystem extraction. No end user installs or runs it. The
WCX installer contains no libchdr DLL, mounted disk, helper or ISO staging file.

Compressed CD support is currently experimental until verified against a wider
collection of authentic multi-session and vendor CHD images.

## Multi-segment ISZ preview (after v0.16 stable)

The native decoder now maps successive ISZ part-file payload regions onto
one 64-bit virtual byte stream, then decompresses only the requested chunks.
It never creates a full temporary ISO or loads the entire image into memory.
The first file must be `.isz`; companion parts use `.i01/.i02` or
`.part01.isz/.part02.isz` naming. All parts must remain in the same folder.
Header signature, volume serial, part number, segment extents, compressed-block
boundaries and pointer lengths are validated before data access. Linked part
files and missing or inconsistent parts are rejected.

The test suite generates synthetic 48-byte/64-byte ISZ headers, pointer
widths 2/3/4, no-table uncompressed payloads, 2- and 3-part images splitting
inside compressed chunks, ISO9660 and UDF, malformed parts and Windows WCX
native ABI extraction. These tests are compatibility evidence for those
specific layouts only. No commercial or vendor-image corpus validation is
claimed. Encrypted ISZ remains unsupported.
