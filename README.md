# ISO-WCX

**ISO-WCX v0.12.0 Preview** — modern C++20, read-only [Total Commander](https://www.ghisler.com/) WCX disc-image plugin.

## Preview installation

1. Open [WCX preview CI builds](https://github.com/sgennadi/ISO-WCX/actions/workflows/wcx-preview.yml) and select a successful workflow run.
2. Download its **ISO-WCX-Installable-Preview** artifact, unzip that outer artifact, then open `ISO-WCX-preview.zip` in Total Commander.
3. Total Commander installs `ISO-WCX.wcx64` (64-bit) or `ISO-WCX.wcx` (32-bit) using `pluginst.inf`.

For details, compatibility notes and source build commands, see [PREVIEW.md](PREVIEW.md).

## Formats (preview)

| Format/layout | Coverage |
| --- | --- |
| ISO 9660 / Joliet | File listing and extraction, Unicode names, multi-extent and interleaved files; recover from broken Joliet directory using valid ISO9660 primary volume |
| Rock Ridge | RRIP NM/CE and relocated CL directories, no symlink extraction |
| Raw BIN/IMG | CD Mode 1 / Mode 2 Form 1 (2352-byte), XA Form 1 (2336-byte); automatic probing limited to single-track images |
| CUE/BIN | BINARY data tracks (`MODE1/2048`, `MODE1/2352`, `MODE2/2352`, `MODE2/2336`), INDEX 00/01, multiple FILE entries, audio-track skipping; latest recognizable data track |
| CloneCD CCD/IMG | `.ccd` version 3 + matching `.img` (2352-byte sectors), single-session Mode 1 / Mode 2 Form 1 data tracks, including ECMA-130 scrambled sectors. Optional `.sub` sidecar exposed byte-for-byte under `subchannels/disc.sub`; CD-Audio and detailed P-W/Q decoding not implemented. |
| 150-sector prefix | Relative and some absolute-LBA cases (not full multisession parsing) |
| UDF Type 1 | File Entry/Extended File Entry, OSTA CS0 (including UTF-16 surrogate-pair names), AED extents and reserve VDS failover |
| UDF Metadata / VAT / Sparable | Experimental, synthetic-fixture coverage; not comprehensive UDF 1.02–2.60 |
| Nero NRG | Uncompressed NERO / NER5, DAOI/DAOX and ETNF/ETN2 data tracks; reads the latest detectable data track |
| Alcohol MDS/MDF | MDS version 1 track/session descriptors and sidecar MDF (2048/2336/2352/2448 data sectors); synthetic-test coverage |
| El Torito | Exposes validated BIOS and EFI boot-load images and the boot catalog under `boot.images/`; EFI FAT BPB sizing when valid |
| Original Xbox XDVDFS | Read-only files, nested directories and timestamp metadata (original Xbox only, not Xbox 360 or Xbox One) |
| Multisession descriptor tracks | When multiple CUE/NRG/MDS tracks contain readable filesystems, expose separate `session1/`, `session2/` virtual folders. Not a full merged multisession filesystem. |
| CSO v1/v2 / ZSO | Indexed random-access ISO reads; raw DEFLATE / LZ4 and stored blocks, including CSO v2 mixed-codec images; no intermediate ISO |
| Other compressed/encrypted containers | Not implemented: ISZ, DAA, CHD, NRG encrypted/compressed, MDS v2, MDX; raw CD subchannels are ignored |

## Development

Build with CMake 3.21+, a C++20 compiler and Python 3 for tests:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --parallel
    ctest --test-dir build --output-on-failure

Windows uses Visual Studio 2022 via CMake (`-G "Visual Studio 17 2022" -A x64` or `-A Win32`). CI produces both binary architectures and tests the WCX DLL APIs.

This is an independent modern reimplementation; original ISO.WCX 1.7.6 source code is **not** included. See [PROVENANCE.md](PROVENANCE.md) and [tracking issue #1](https://github.com/sgennadi/ISO-WCX/issues/1). CUE references are restricted to image files inside the CUE directory tree (including resolved symlinks). Audio decoding and full multisession TOC interpretation are not supported. NRG/MDS support is limited to verified uncompressed data-track layouts and has not been validated against a representative real-disc sample collection. Associations for `.nrg`, `.mds`, `.cso` and `.zso` are registered by the ZIP installer, while `.bin` and `.mdf` remain unassociated. Preview images were synthetically constructed; tests in the Total Commander GUI and against a diverse real-disc collection remain outstanding.

## What changed in v0.7.0

- ISO9660 interleaved file units and gaps are now extracted in correct byte order, including multi-extent/interleaved combinations. Interleaved directories are rejected rather than silently read incorrectly.
- When an optional Joliet directory is corrupt, the reader can recover through the intact ISO9660 primary volume.
- UDF OSTA CS0 16-bit names support valid supplementary Unicode code points represented by UTF-16 surrogate pairs. Invalid pairs are rejected.
- UDF mounting tries the **reserve Volume Descriptor Sequence** when the main VDS is corrupt, before trying other UDF anchors or ISO9660 fallback.
- 12 added deterministic success/failure cases and an **independently mastered** ISO image built using libarchive on supported Linux systems. That fixture tests nested directories, Unicode, 1 MiB streamed extraction and symlink rejection.

**Real optical disc validation is still outstanding**: an independently mastered fixture exercises a real ISO writer, but it is not equivalent to testing a representative collection of commercial/recordable DVD or Blu-ray media, damaged sessions and every UDF version.

## What changed in v0.8.0

- **Rollback-safe extraction:** write file data to a randomized staging file in the same destination directory, close successfully and rename it over the destination only after completing every extent. A cancellation, incomplete source read or write failure removes the staging file and leaves an existing destination intact.
- **WCX Unicode progress:** when Total Commander installs both ANSI and Unicode progress handlers, the plugin reports progress through the Unicode handler only, rather than double-counting bytes. The ANSI callback remains supported as a fallback.
- **Regression coverage:** a native public-API test covers cancellation, overwriting, source truncation, successful commit, test-only reads and no orphaned staging files. The Windows WCX loader test additionally validates `SetProcessDataProcW`, ANSI fallback, byte deltas and `E_EABORTED`.
- **CI hardening:** Linux Clang AddressSanitizer/UndefinedBehaviorSanitizer testing is now a regular GitHub Actions job, alongside Windows x86/x64 build and DLL smoke tests.

As before, the WCX adapter is read-only and **Preview** quality. We have not demonstrated compatibility with every optical-media image format, commercial disc or live Total Commander installation. Destination path symlinks are checked, but extracting into a directory modified concurrently by an adversary remains outside the current threat model.

## What changed in v0.9.0

- **El Torito:** inspect the ISO boot record and validated boot catalog; expose read-only BIOS and EFI boot images in the virtual `boot.images` directory. Advertised no-emulation SectorCount is a boot-load size, not proof of the entire image length; EFI FAT BPB is used only after validation. Invalid catalog checksums or out-of-bounds images are rejected.
- **Original Xbox XDVDFS:** parse the volume descriptor, sibling directory tree and nested folders with cycle checks, bounded metadata and secure filenames. This does not support Xbox 360 XGD discs.
- **Descriptor-driven multisession views:** enumerate more than one filesystem-bearing CUE, NRG or MDS track under `session1/`, `session2/` etc. Track indices are preserved for extraction, including earlier tracks. Does not replace a full TOC-based multisession implementation for raw `.iso`/`.bin` files.
- **Metadata:** ISO 9660 and UDF timestamps and file attributes are passed through the WCX API. Xbox XDVDFS entries inherit the volume creation date when per-file timestamps are unavailable.
- **Security and rollback:** preserves the v0.8 staging-file extraction and single Unicode progress callback behavior.
- **Validation:** independent synthetic tests for El Torito BIOS/EFI, XDVDFS nested trees, multisession extraction and malformed descriptors; Linux tests and sanitizers. MDS v2, MDX and compressed ISZ/CSO/ZSO/DAA are still **not supported**.


## What changed in v0.10.0

- New independent bounded CSO v1, CSO v2 (DEFLATE/LZ4), and ZSO (LZ4) on-demand reader; one-block cache, no full ISO staging.
- Extend raw ISO and MDS to 2448-byte sectors (2352-byte CD frame + 96 subchannel bytes); subchannel metadata is deliberately ignored.
- Synthetic random-access and extraction tests for stored, DEFLATE, LZ4, shifted indexes, UDF and corrupted compressed blocks. Windows CI also loads compressed ISO images through the WCX API.
- Compared bchunk, mdf2iso and daa2iso for interoperability. Their GPL source code is **not** copied. DAA and MDS v2 are not yet supported.

## What changed in v0.11.0

- Added clean-room CloneCD `.ccd`/`.img` read-only loader based on the published descriptor layout. Track INDEX boundaries and .img sector alignment are validated. Audio, scrambled sectors, subchannels and multidisc CCD sessions are deliberately not decoded.
- A multi-data-track CCD is presented as `session1/`, `session2/` virtual folders, each retaining its own physical source when files are extracted.
- Added synthetic Mode 1/2, audio-prefix, multi-data-track, path and malformed-metadata regression tests on Linux and Windows. See [format comparison](docs/libmirage-format-comparison.md) for the libMirage references and licensing.
