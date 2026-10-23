# ISO-WCX

**ISO-WCX v0.6.0 Preview** — modern C++20, read-only [Total Commander](https://www.ghisler.com/) WCX disc-image plugin.

## Preview installation

1. Open [WCX preview CI builds](https://github.com/sgennadi/ISO-WCX/actions/workflows/wcx-preview.yml) and select a successful workflow run.
2. Download its **ISO-WCX-Installable-Preview** artifact, unzip that outer artifact, then open `ISO-WCX-preview.zip` in Total Commander.
3. Total Commander installs `ISO-WCX.wcx64` (64-bit) or `ISO-WCX.wcx` (32-bit) using `pluginst.inf`.

For details, compatibility notes and source build commands, see [PREVIEW.md](PREVIEW.md).

## Formats (preview)

| Format/layout | Coverage |
| --- | --- |
| ISO 9660 / Joliet | File listing, extraction, Unicode names, multi-extent files |
| Rock Ridge | RRIP NM/CE and relocated CL directories, no symlink extraction |
| Raw BIN/IMG | CD Mode 1 / Mode 2 Form 1 (2352-byte), XA Form 1 (2336-byte); automatic probing limited to single-track images |
| CUE/BIN | BINARY data tracks (`MODE1/2048`, `MODE1/2352`, `MODE2/2352`, `MODE2/2336`), INDEX 00/01, multiple FILE entries, audio-track skipping; latest recognizable data track |
| 150-sector prefix | Relative and some absolute-LBA cases (not full multisession parsing) |
| UDF Type 1 | File Entry/Extended File Entry, OSTA CS0 and AED extents |
| UDF Metadata / VAT / Sparable | Experimental, synthetic-fixture coverage; not comprehensive UDF 1.02–2.60 |
| Nero NRG | Uncompressed NERO / NER5, DAOI/DAOX and ETNF/ETN2 data tracks; reads the latest detectable data track |
| Alcohol MDS/MDF | MDS version 1 track/session descriptors and sidecar MDF (2048/2336/2352 data sectors); synthetic-test coverage |
| Compressed or encrypted proprietary variants | Not implemented (NRG subchannels, MDS v2, MDX, ISZ, CSO/ZSO, DAA) |

## Development

Build with CMake 3.21+, a C++20 compiler and Python 3 for tests:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --parallel
    ctest --test-dir build --output-on-failure

Windows uses Visual Studio 2022 via CMake (`-G "Visual Studio 17 2022" -A x64` or `-A Win32`). CI produces both binary architectures and tests the WCX DLL APIs.

This is an independent modern reimplementation; original ISO.WCX 1.7.6 source code is **not** included. See [PROVENANCE.md](PROVENANCE.md) and [tracking issue #1](https://github.com/sgennadi/ISO-WCX/issues/1). CUE references are restricted to image files inside the CUE directory tree (including resolved symlinks). Audio decoding and full multisession TOC interpretation are not supported. NRG/MDS support is limited to verified uncompressed data-track layouts and has not been validated against a representative real-disc sample collection. Associations for `.nrg` and `.mds` are registered by the ZIP installer, while `.bin` and `.mdf` remain unassociated. Preview images were synthetically constructed; tests in the Total Commander GUI and against a diverse real-disc collection remain outstanding.
