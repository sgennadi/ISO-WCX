# ISO-WCX v0.5.0 C++20 preview

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
- RAW Mode 2 Form 2, audio tracks, mixed-mode tracks, complete CUE/CCD/MDS metadata and full multisession TOC are not supported.
- UDF VAT historical chains, every version of Metadata/Sparable, UDF pseudo-overwrite behavior and fragmented descriptor edge cases are not yet exhaustively covered.
- Rock Ridge POSIX permissions, TF timestamps and symlink extraction are not implemented.
- Compression/container formats (ISZ, CSO, ZSO, DAA, DMG, NRG, MDF, etc.) are not supported yet.
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
