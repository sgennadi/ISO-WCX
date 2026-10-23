# ISO-WCX v0.6.0 C++20 preview

Modern, read-only Total Commander WCX plugin. The codebase is clean-room and does not republish the legacy ISO.WCX 1.7.6 sources. Built with CMake and Visual Studio 2022 (Windows x86/x64).

## Supported in the preview

- CUE/BIN track selection: opens `.cue` directly, parses multiple FILE/TRACK and INDEX 00/01 records, skips audio and unsupported track types, and chooses the last ISO/UDF-bearing data track. Supports 2048, 2336, Mode 1/2352 and Mode 2 Form 1/2352 layouts; extracts files through the same read-only WCX API.
- ISO9660 and Joliet, including Unicode names and ISO multi-extent files.
- Rock Ridge RRIP NM/CE names, CL relocated directory references; symbolic links are ignored rather than followed.
- 2048-byte logical images, raw 2352-byte CD Mode 1 and Mode 2 Form 1, and 2336-byte XA Form 1 frames (no Form 2).
- Some images with 150-sector lead-in (relative and absolute session LBA conventions), but **not full multisession TOC/CUE parsing**.
- UDF Type-1 physical partition mapping, File Entry/Extended File Entry, embedded/short/long allocation descriptors, AED continuations, OSTA CS0 names.
- **Experimental** UDF Metadata, Virtual (VAT) and Sparable partition-map reading, including mirror metadata fallback and packet redirects.
- Read-only WCX operations (list/test/extract), streamed file data, 64-bit file sizes, bounded parsing and safety checks.

## Limits and cautions

- This is a **preview** tested primarily with deterministic synthetic image fixtures. Real DVD/BD/UDF compatibility is not yet guaranteed, especially rewritable/packet-written and damaged media.
- RAW Mode 2 Form 2, audio extraction, subchannel processing, full TOC/CCD and nonstandard CUE/MDS layouts are not supported. NRG and MDS select the latest recognized data track, but this is not full multisession filesystem merging.
- UDF VAT historical chains, every version of Metadata/Sparable, UDF pseudo-overwrite behavior and fragmented descriptor edge cases are not yet exhaustively covered.
- Rock Ridge POSIX permissions, TF timestamps and symlink extraction are not implemented.
- Unsupported image containers remain ISZ, CSO, ZSO, DAA, DMG and encrypted/compressed NRG/MDF variations. Only uncompressed NRG and MDS/MDF track subsets are implemented.
- Avoid extracting untrusted images into important directories; path checks cannot fully mitigate concurrent symlink races.
- Unsigned binary; this is not an official release of the legacy ISO.WCX plugin.

## Build and verify

Visual Studio 2022 x64:

    cmake -S . -B build -G "Visual Studio 17 2022" -A x64
    cmake --build build --config Release
    ctest --test-dir build -C Release --output-on-failure

For 32-bit replace `-A x64` with `-A Win32`. Builds produce `ISO-WCX.wcx64` / `ISO-WCX.wcx`.

Linux CLI tests:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --parallel
    ctest --test-dir build --output-on-failure

CLI tool: `disc_inspect image.iso` or `disc_inspect image.iso --extract OUTPUT`.

## Installing in Total Commander

Download the **ISO-WCX-Installable-Preview** artifact from successful GitHub Actions runs. Unzip the outer GitHub artifact archive, then open `ISO-WCX-preview.zip` directly in Total Commander; `pluginst.inf` prompts installation. Alternatively configure `.iso`, `.img` or `.bin` manually under **Configuration > Options > Packer > Configure packer extension WCXs**. Only use `.bin` images in the supported single-track layouts above.

CI compiles Windows x64 and Win32 binaries, runs CTest including WCX export/list/extract smoke checks, and packages an installable ZIP when both architectures pass.

## NRG and MDS/MDF preview support

- Nero `.nrg`: NERO (32-bit) and NER5 (64-bit) trailer, bounded chunk list, DAOI/DAOX (disc-at-once) and ETNF/ETN2 (track-at-once), selected latest ISO9660/UDF data track.
- Alcohol `.mds`/`.mdf`: validated MDS v1 session/track/filename/index blocks; uncompressed Mode 1, Mode 2 Form 1 and cooked 2048/2336 data tracks. `.mdf` is a sidecar, not a general Microsoft SQL MDF reader.
- Raw-image probing is still available for `.img`/`.bin`; installer associations include `.iso`, `.cue`, `.nrg`, `.mds` but **not** `.bin` or `.mdf`, avoiding conflicts.
- Synthetic NRG/MDS regression tests and malformed-metadata mutation tests are run by CTest on Linux and Windows. Real Nero/Alcohol images and actual Total Commander interaction still require manual compatibility testing.
