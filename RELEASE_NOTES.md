# ISO-WCX v0.21.0 — Export Image Structure and WIM metadata safety

Total Commander in-process WCX plugin for Win32/x64, built with Visual
Studio 2026 (MSVC 19.50+ and experimental C++26 draft flags). The two plugin
binaries are `ISO-WCX.wcx` and `ISO-WCX.wcx64`. No helper EXE, parser
DLL, mount service or third-party runtime program is required.

## New since Stable v0.20.0

### Built-in Export Image Structure (PR #62)

Choose **Files > Pack > [ISO-WCX association] > Configure... >
Export Image Structure...** and select the source image and a TXT output.
The plugin exports **metadata only**, in UTF-8: detected container,
directory/file tree, sizes, attributes, extent offsets and counts. For
raw ISO/UDF it also reports ISO9660 descriptors, PVD label, Joliet,
El Torito, candidate UDF anchors, the first 32-KiB system-area flag,
and alternate ISO9660/Joliet directory views where relevant.

The report uses the same in-process reader as Total Commander and requires
no `disc_inspect.exe`. The image is opened for reading; no payloads are
extracted and no image data is modified. Export the report only to a
destination you control, and review potentially sensitive filenames before
sharing the report.

### WIM validation groundwork (PR #63 and #64)

A bounded **read-only** WIM metadata preflight and internal tree decoder
validate selected uncompressed resource-table layouts, directory records,
UTF-16 names, metadata streams, offsets and security references. Malformed
and cyclic fixtures have regression tests. The decoder is development/test
infrastructure, **not a new WIM writer or UI feature**. Most real Windows
WIMs use compressed metadata and require future work.

Creating/deleting folders in ordinary unsplit WIMs **still invokes Windows
WIMGAPI Apply/Capture**, even though v0.20.0 already improved stage
progress and selective post-edit readback. Creating an empty folder in a
multi-gigabyte WIM is therefore **not instantaneous**. ESD and SWM editing
are not enabled. No extra WIM DLLs or external extractors are shipped.

## What is deliberately NOT included

- Experimental boot-preserving ISO/Joliet file deletion (PR #65) is not
  included in v0.21.0. It remains in a separate Preview pending validation
  with real bootable ISO images; the release refuses unsupported hybrid
  directory changes to avoid silently breaking boot sectors or UDF metadata.
- General ISO/UDF Bridge deletion, arbitrary Boot ISO remastering, generic
  NTFS/ext4 writes, dynamically allocated VHDX editing, and metadata-only
  WIM creation or deletion are not supported.
- Existing bounded FAT/UDF/VHD/VHDX writer Preview modes retain their
  documented limitations. Back up important images before any edit.

## Build verification and distribution

All release steps require successful Visual Studio 2026 x86/x64 C++26
draft builds and CTest, independent C++20 compatibility checks, Linux
GCC14/Clang19 C++26, Linux C++20, sanitizer tests, WIM/ISO/UDF and
virtual-disk tests, independent QEMU/UDF/7-Zip interoperability and
installer ZIP/SHA256 verification. A check of matching version strings
guards CMake, the Total Commander installer, release metadata and
documentation against partial version bumps.

Only a fully passing **main** workflow may create the immutable
`v0.21.0` release and tag. Existing v0.20.0 packages stay unchanged.

## Install

After publication, get `ISO-WCX-v0.21.0.zip` and `SHA256SUMS.txt` from:
https://github.com/sgennadi/ISO-WCX/releases/tag/v0.21.0

Open the ZIP in Total Commander, confirm its WCX installation request,
then restart Total Commander. The integrated updater checks only
published Stable releases and may offer the verified ZIP; it never
overwrites a loaded DLL.
