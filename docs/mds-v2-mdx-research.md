# MDS v2 / MDX independent metadata decoder (experimental)

Sources reviewed on 2026-10-23:

- [Aaru issue #256](https://github.com/aaru-dps/Aaru/issues/256) ("Support MDSv2/MDX images"), open feature request. The discussion describes DAEMON Tools format changes, encrypted/obfuscated metadata, MDX's single-file container and caveats for password-protected/TAGES media.
- [Marisa-Chan/mdsx](https://github.com/Marisa-Chan/mdsx), especially src/mds.h, src/mds.c, src/decode.c, README and testdata. The author reports decoding MDSv2 and MDX, including non-password and compressed cases, but describes the tool as a research base, not a production MDSv1/ISO converter.
- [Aaru Alcohol120 code](https://github.com/aaru-dps/Aaru/blob/devel/Aaru.Images/Alcohol120/Constants.cs): MAXIMUM_SUPPORTED_VERSION = 1. Aaru does **not** currently implement MDSv2/MDX.
- The project also compared [libMirage](https://cdemu.sourceforge.io/pkg_libmirage.php) and the official 7-Zip ISO/UDF handlers for optical-sector layout.

## What is implemented in ISO-WCX today

There are two strictly separated operations:

1. `mds_descriptor_inspect IMAGE.mds|IMAGE.mdx` performs a 96-byte
   fast-recognition probe of original MDS v1, MDS v2 and embedded MDX
   sentinel/region pointers. It rejects malformed offsets before any
   decryption.
2. `mds_descriptor_inspect IMAGE.mds|IMAGE.mdx --decode` runs the native
   metadata decoder: password-free salt unshuffle, PBKDF2-HMAC-RIPEMD160
   (2000 iterations), AES-256 CBC with the format-specific whitening,
   authenticated-header CRC32 validation, and bounded zlib inflation into
   the reconstructed in-memory MDS v2 descriptor. The 16 MiB decoded
   metadata limit does not cap the MDX container file size.

The AES-256 block decryptor is compiled statically from pinned public-domain
tiny-AES-c (full Unlicense included in THIRD_PARTY_NOTICES.txt). The
RIPEMD-160/HMAC/PBKDF2 routines are an independent in-tree implementation.
No runtime OpenSSL DLL, chdman executable or mounting tool is needed.

The opt-in developer command **does not decode or expose CD/DVD track
sectors** and does not copy decrypted key data to disk. The Total Commander
WCX handler continues to reject MDS v2 and MDX as unsupported archive
formats until their data decoders pass genuine image and WCX ABI tests.
The ZIP installer does not register .mdx.

Unit regressions cover header boundaries, malformed pointers, the
RIPEMD-160 reference digest vectors and ciphertext tampering.
A dedicated CI workflow downloads eight pinned genuine .mds/.mdx
descriptors from Marisa-Chan/mdsx, checks their Git blob hashes,
successfully decrypts metadata and rejects an encrypted-header mutation.
This independently validates descriptor compatibility; it is **not**
yet a test of compressed/encrypted track-data extraction.

## Remaining work before a production data-track reader

1. **Provenance and licensing:** Although mdsx has a root **MIT** LICENSE,
   parts of its implementation originated in TrueCrypt 5.1a and carry their
   own **TrueCrypt License 2.4** notices; some AES routines also carry
   separate attribution terms. Do not copy its common/ or crypto/ directories
   directly into this WCX DLL under an assumption of MIT-only licensing.
   Any actual import requires a per-file review and notices.
2. **Keep metadata and track decoding separate.** The encrypted-header
   metadata stage is now implemented. Implement a separate bounded
   random-access stream for plain, compressed-group and optional LRW
   encrypted track data covering 2048/2324/2336/2352/2448-byte sectors.
   Never parse encrypted or unvalidated data as ordinary ISO data.
3. **Require a representative image corpus:** Non-password MDSv2, compressed
   and uncompressed MDS/MDF, monolithic MDX, multiple sessions/tracks,
   Mode 1/Mode 2 Form 1, mixed-mode discs, subchannels, a known-password
   test case, intentionally corrupt descriptors and unsupported TAGES cases.
   Use independently generated/test-authorized images and known-good
   SHA-256 digests rather than treating unit fixture names as coverage.
4. **Windows WCX compatibility:** Maintain both Win32/Win64 ABIs and
   transactional extraction. Do not register .mdx or advertise a new
   capability until read/extract tests pass through the plugin and real
   Total Commander GUI.

The mdsx README states its current implementation is research-grade, and
Aaru issue #256 remains open; neither project establishes complete vendor
compatibility. This stage delivers tested descriptor decryption but not a production
MDSv2/MDX filesystem reader.
