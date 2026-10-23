# ISO-WCX C++20 native preview

This clean-room preview implements a read-only Total Commander packer plugin without importing original ISO.WCX 1.7.6 source code. It is intentionally separate pending a review of the original author's redistribution terms.

## Working preview functionality

- ISO9660 (logical 2048-byte sectors), Joliet Unicode, and contiguous or multi-extent file data.
- UDF Type-1 partition mapping with supported File Entry/Extended File Entry, inline or short/long extents, and OSTA CS0 filenames.
- Extraction, test, skip, folder enumeration, WCX Unicode and ANSI entrypoints.
- Large file (64-bit) sizes in `ReadHeaderExW` and streamed extraction.
- Basic malformed-image validation, UTF-8 paths, filtering unsafe Windows filenames, CRC16 for non-empty UDF descriptor CRC fields.
- Read-only on purpose.

## Explicit limitations

- UDF VAT / metadata / sparable partitions are not yet supported, nor continuation allocation extents.
- Rock Ridge, El Torito virtual folders, multisession offsets, 2352-byte sector BIN, compressed container formats are not implemented yet.
- The WCX adapter must be built and tested on Windows. Core smoke tests on Linux do not establish Windows compatibility.
- For untrusted ISO images, test on disposable filesystems: the extraction path validates symbolic links but cannot eliminate races with another process changing directories.
- This is a preview, not an official ISO.WCX release.

## Build and test

Windows x64 (Visual Studio 2022 build tools):

    cmake -S . -B build -G "Visual Studio 17 2022" -A x64
    cmake --build build --config Release
    ctest --test-dir build -C Release --output-on-failure

For 32-bit, replace `x64` with `Win32`. Output is `ISO-WCX.wcx64` or `ISO-WCX.wcx` under `build/Release`.

Linux CLI-only validation:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --parallel
    ctest --test-dir build --output-on-failure

Inspect a disc image:

    ./build/disc_inspect image.iso

Extract:

    ./build/disc_inspect image.iso --extract output-directory

## Install in Total Commander

In **Configuration > Options > Packer > Configure packer extension WCXs**, register the `.iso` extension against `ISO-WCX.wcx64` (Total Commander x64), or `.wcx` (32-bit). For an ISO/UDF image the same plugin performs format probing. More extensions can be registered only after corresponding image layouts are supported.

CI builds the two Windows architectures and runs a dynamic DLL export/list/extraction smoke test. Preview binaries are CI artifacts, not signed releases.
