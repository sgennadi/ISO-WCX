# ISO-WCX

A modern **read-only Total Commander WCX** disc-image plugin based on C++20 and CMake.

> **Status: v0.3.0 preview, not a production release.** Windows x86 and x64 binaries compile and pass the automated WCX load/list/extract smoke tests. Compatibility with real-world discs and the Total Commander user interface still needs further validation.

## Download the preview

Open [GitHub Actions](https://github.com/sgennadi/ISO-WCX/actions/workflows/wcx-preview.yml), choose a successful run, and download the **ISO-WCX-Installable-Preview** artifact. The artifact contains `ISO-WCX-preview.zip`, with both binaries and `pluginst.inf`. Open the inner ZIP in Total Commander to start its plugin installer.

## Current support

- ISO 9660 with 2048-byte sectors, including multi-extent file data.
- Joliet (Unicode filenames).
- Rock Ridge RRIP alternative names via `NM`, SUSP `SP`/`ER` detection and bounded `CE` continuation areas. Symbolic links are deliberately omitted.
- Basic UDF Type-1 partition mapping, File Entry / Extended File Entry, short and long extents, `AED` allocation extent continuations, and OSTA CS0 filenames.
- Read-only archive navigation, testing, and extraction through WCX Unicode and ANSI API.
- Streamed file reads with 64-bit sizes, bounded parsing and initial malformed-image checks.
- Windows x86 `ISO-WCX.wcx` and x64 `ISO-WCX.wcx64` with GitHub Actions builds and tests.

**Not yet implemented:** Rock Ridge CL relocated directory trees, symlink extraction and POSIX attributes/timestamps; El Torito virtual folders; multisession/session offsets; 2352-byte sector BIN; UDF VAT/Metadata/Sparable partitions; compressed ISO container formats. Do not assume every UDF 1.02–2.60 image works. Do not assume every UDF 1.02–2.60 image works.

## Build

Windows with Visual Studio 2022:

```sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Use `-A Win32` for the 32-bit plugin. For Linux, the standalone `disc_inspect` utility and parser tests build with regular CMake commands; the WCX DLL is Windows-only.

See [PREVIEW.md](PREVIEW.md) for detailed build and install instructions and [PROVENANCE.md](PROVENANCE.md) for code provenance and licensing considerations. Original ISO.WCX 1.7.6 sources have **not** been republished. [Track additional format support in issue #1](https://github.com/sgennadi/ISO-WCX/issues/1).
