# MDS v2 / MDX reverse-engineering notes (metadata-only stage)

Sources reviewed on 2026-10-23:

- [Aaru issue #256](https://github.com/aaru-dps/Aaru/issues/256) ("Support MDSv2/MDX images"), open feature request. The discussion describes DAEMON Tools format changes, encrypted/obfuscated metadata, MDX's single-file container and caveats for password-protected/TAGES media.
- [Marisa-Chan/mdsx](https://github.com/Marisa-Chan/mdsx), especially src/mds.h, src/mds.c, src/decode.c, README and testdata. The author reports decoding MDSv2 and MDX, including non-password and compressed cases, but describes the tool as a research base, not a production MDSv1/ISO converter.
- [Aaru Alcohol120 code](https://github.com/aaru-dps/Aaru/blob/devel/Aaru.Images/Alcohol120/Constants.cs): MAXIMUM_SUPPORTED_VERSION = 1. Aaru does **not** currently implement MDSv2/MDX.
- The project also compared [libMirage](https://cdemu.sourceforge.io/pkg_libmirage.php) and the official 7-Zip ISO/UDF handlers for optical-sector layout.

## What is implemented in ISO-WCX today

An independent C++20 metadata probe is built into iso_disc and as a
standalone mds_descriptor_inspect tool:

    mds_descriptor_inspect IMAGE.mds
    mds_descriptor_inspect IMAGE.mdx

The tool reads at most **96 bytes** of the descriptor initially, rejects
symlink/non-file inputs, validates the "MEDIA DESCRIPTOR" signature and
recognizes original Alcohol MDS v1, packed/encrypted DAEMON Tools MDS major v2
and MDX's embedded-file sentinel. For v2/MDX it checks the encrypted 512-byte
header lies within the file and validates the packed metadata region's
byte bounds, including 64-bit arithmetic overflow protections.

**Recognition is NOT decryption or extraction.** The MDS v1 uncompressed
track parser is unchanged. In Total Commander, MDS v2 and MDX are explicitly
rejected with a specific error instead of being silently interpreted as an
ISO file or incorrectly parsed as legacy MDS v1. The ZIP installer does NOT
register .mdx yet; no unsupported association is silently installed.

The probe intentionally does not:
- infer track counts from encrypted metadata;
- open, follow, mount or edit companion MDF files;
- derive keys, ask for passwords, brute-force passwords or import old crypto;
- decompress encrypted metadata or compressed track groups;
- expose CD-Audio, protected-track modes, indexes or encrypted sectors;
- claim that synthetically constructed headers prove vendor interoperability.

Unit regressions cover valid synthetic format envelopes, wrong signatures,
truncation, malicious offsets, integer overflows and both forms of MDX marker.
A separate WCX/Disc regression confirms a recognized MDX is safely rejected.

## Before implementing a real decoder

1. **Provenance and licensing:** Although mdsx has a root **MIT** LICENSE,
   parts of its implementation originated in TrueCrypt 5.1a and carry their
   own **TrueCrypt License 2.4** notices; some AES routines also carry
   separate attribution terms. Do not copy its common/ or crypto/ directories
   directly into this WCX DLL under an assumption of MIT-only licensing.
   Any actual import requires a per-file review and notices.
2. **Keep format detection separate from decryption.** Build an optional,
   separately tested decoder backend for validated encrypted headers,
   compressed track block indexes, and 2048/2324/2336/2352/2448
   data-sector layout. Never parse encrypted metadata as plaintext.
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
compatibility. This stage delivers a safe recognition boundary, not a
production MDSv2/MDX filesystem reader.
