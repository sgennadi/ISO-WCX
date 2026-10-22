# ISO-WCX

Modern C++20 foundation for a read-only Total Commander WCX disc-image plugin.

> **Status: early development.** This repository does not yet contain a working WCX plugin, and does not currently produce a `.wcx` or `.wcx64` file.

## Goals

- Modern C++20 codebase, CMake and Windows x86/x64 builds.
- ISO 9660, Joliet, Rock Ridge and UDF, followed by additional image formats.
- Strict bounded parsing, Unicode, large files, safe extraction and robust malformed-image handling.
- Continuous integration, regression tests and reproducible releases.

## Current contents

A small independent endian-safe binary-reader foundation and tests are included. The existing legacy ISO.WCX implementation and experimental UDF prototype are **not** published here. The migration requires separate validation of original code redistribution rights and regression testing.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Roadmap

1. Verify legacy-source provenance and license.
2. Build independently testable ISO9660/Joliet/Rock Ridge and UDF reader libraries.
3. Integrate parsers behind the unchanged Total Commander WCX API.
4. Produce and test 32-bit `.wcx` and 64-bit `.wcx64` on Windows.
5. Add complex UDF partitions, fuzzing and release packaging.

This is **not** a fork claiming ownership of the original ISO.WCX code. See [PROVENANCE.md](PROVENANCE.md).
