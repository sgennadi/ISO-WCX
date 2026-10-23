# ISO-WCX

**ISO-WCX v0.13.0 Preview** — a self-contained modern C++20, read-only
Total Commander WCX plugin for optical disc images and virtual HDD filesystems.

## Install into Total Commander

1. Open [WCX preview builds](https://github.com/sgennadi/ISO-WCX/actions/workflows/wcx-preview.yml).
2. Download the **ISO-WCX-Installable-Preview** ZIP artifact, unzip the artifact envelope if needed, and open **ISO-WCX-preview.zip** in Total Commander.
3. Total Commander uses `pluginst.inf` to install `ISO-WCX.wcx` for 32-bit and `ISO-WCX.wcx64` for 64-bit. Keep both files together in the plugin folder.
4. Restart Total Commander and open a supported image with Ctrl+PgDn.

**No separate setup EXE, parser DLL, unpacker, PowerShell script or mounting
service is needed.** Existing native helper CLIs are development/test programs,
not dependencies of the WCX installation.

If Total Commander still points to a removed old `...\ISO\iso.wcx`,
fix **Configuration → Options → Packer → Configure packer extension WCXs**:
remove only the obsolete `iso` association and add a new type pointing to
the installed `ISO-WCX.wcx`. Restart TC. The plugin itself cannot repair an
association that prevents its DLL from loading in the first place.

For detailed instructions see [PREVIEW.md](PREVIEW.md),
[INSTALL_EN.txt](INSTALL_EN.txt), and [INSTALL_RU.txt](INSTALL_RU.txt).

## Formats (preview)

| Format/layout | Coverage |
| --- | --- |
| ISO 9660 / Joliet | File listing and extraction, Unicode names, multi-extent and interleaved files; recover from broken Joliet directory using valid ISO9660 primary volume |
| Rock Ridge | RRIP NM/CE and relocated CL directories; PX read-only and TF modification timestamps; no symlink/device extraction |
| Raw BIN/IMG | CD Mode 1 / Mode 2 Form 1 (2352-byte), XA Form 1 (2336-byte); automatic probing limited to single-track images |
| CUE/BIN | BINARY data tracks (`MODE1/2048`, `MODE1/2352`, `MODE2/2352`, `MODE2/2336`), INDEX 00/01, multiple FILE entries, audio-track skipping; latest recognizable data track |
| CloneCD CCD/IMG | `.ccd` version 3 + matching `.img` (2352-byte sectors), single-session Mode 1 / Mode 2 Form 1 data tracks, including ECMA-130 scrambled sectors. Optional `.sub` sidecar exposed byte-for-byte under `subchannels/disc.sub`; CD-Audio and CD-Text decoding not implemented; CRC-valid Q track/index frames summarized in `subchannels/q-index.csv`. |
| 150-sector prefix | Relative and some absolute-LBA cases (not full multisession parsing) |
| UDF Type 1 | File Entry/Extended File Entry, OSTA CS0 (including UTF-16 surrogate-pair names), AED extents and reserve VDS failover |
| UDF Metadata / VAT / Sparable | Experimental, synthetic-fixture coverage; not comprehensive UDF 1.02–2.60 |
| Nero NRG | Uncompressed NERO / NER5, DAOI/DAOX and ETNF/ETN2 data tracks; reads the latest detectable data track |
| Alcohol MDS/MDF | MDS version 1 track/session descriptors and sidecar MDF (2048/2336/2352/2448 data sectors); synthetic-test coverage |
| El Torito | Exposes validated BIOS and EFI boot-load images and the boot catalog under `boot.images/`; EFI FAT BPB sizing when valid |
| Partitioned HDD IMG/RAW, fixed VHD, clean base VHDX | Built-in native MBR/GPT + FAT12/16/32 browsing and extraction under `partitionN/`; VHDX fixed/dynamic sparse payloads with 512/4096 logical sectors; no mount or external DLL |\n| Original Xbox XDVDFS | Read-only files, nested directories and timestamp metadata (original Xbox only, not Xbox 360 or Xbox One) |
| Multisession descriptor tracks | When multiple CUE/NRG/MDS tracks contain readable filesystems, expose separate `session1/`, `session2/` virtual folders. Not a full merged multisession filesystem. |
| CSO v1/v2 / ZSO | Indexed random-access ISO reads; raw DEFLATE / LZ4 and stored blocks, including CSO v2 mixed-codec images; no intermediate ISO |
| Other compressed/encrypted containers | Not implemented: ISZ, DAA, CHD, NRG encrypted/compressed, MDS v2, MDX; only CloneCD Q position decoding is implemented; other raw CD subchannels are not interpreted |

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
- **Validation:** independent synthetic tests for El Torito BIOS/EFI, XDVDFS nested trees, multisession extraction and malformed descriptors; Linux tests and sanitizers. MDS v2, MDX, ISZ, CHD and DAA are **not supported**; indexed CSO v1/v2 and ZSO are already implemented and tested synthetically.


## What changed in v0.10.0

- New independent bounded CSO v1, CSO v2 (DEFLATE/LZ4), and ZSO (LZ4) on-demand reader; one-block cache, no full ISO staging.
- Extend raw ISO and MDS to 2448-byte sectors (2352-byte CD frame + 96 subchannel bytes); subchannel metadata is deliberately ignored.
- Synthetic random-access and extraction tests for stored, DEFLATE, LZ4, shifted indexes, UDF and corrupted compressed blocks. Windows CI also loads compressed ISO images through the WCX API.
- Compared bchunk, mdf2iso and daa2iso for interoperability. Their GPL source code is **not** copied. DAA and MDS v2 are not yet supported.

## What changed in v0.11.0

- Added clean-room CloneCD `.ccd`/`.img` read-only loader based on the published descriptor layout. Track INDEX boundaries and .img sector alignment are validated. Audio, multidisc CCD sessions and CD-Text are not decoded. CloneCD scrambled Mode 1/2 and raw `.sub` + CRC-valid Q positions are supported in v0.12.
- A multi-data-track CCD is presented as `session1/`, `session2/` virtual folders, each retaining its own physical source when files are extracted.
- Added synthetic Mode 1/2, audio-prefix, multi-data-track, path and malformed-metadata regression tests on Linux and Windows. See [format comparison](docs/libmirage-format-comparison.md) for the libMirage references and licensing.

## What changed in v0.12.0

- **ECMA-119 extended attributes:** Honor directory-record extended-attribute block counts when reading regular files and directories. Reject conflicting big- and little-endian extent values.
- **CloneCD scrambled tracks:** Restore physical CD-ROM data on demand with the ECMA-130 Annex B sector-synchronized XOR sequence. This is **not** decryption of proprietary formats.
- **CloneCD `.sub`:** Preserve and extract an optional same-base raw P-W subchannel stream under `subchannels/disc.sub` after checking every 96-byte frame corresponds to one 2352-byte `.img` frame. A CRC-gated `subchannels/q-index.csv` report identifies valid Q track/index positions; CD-Text and copy-protection semantics are **not decoded**, and invalid Q frames never invalidate raw `.sub`.
- **Regression tests:** Synthetic ISO extended-attribute and endian corruption tests, scrambled CCD round trips and byte-perfect `.sub` extraction.

The roadmap still includes *MDS v2, MDX, ISZ, DAA, CHD, DMG, ECM, GZ/XZ and CD-Audio*. They are not implied by libMirage/CDEmu supporting them.

## What changed in v0.12.1

- Read CloneCD P-W Q position frames with both deinterleaved and interleaved decoders; validate the CD CRC16 checksum before interpreting track/index or MSF timestamps.
- Export a bounded, read-only `subchannels/q-index.csv` listing CRC-valid Q observations, while preserving `subchannels/disc.sub` byte-for-byte.
- Correct contradictory older CloneCD negative test fixtures and add C++20 Q-channel unit tests. CD-Text, encrypted formats and full protection handling remain unsupported.

### Experimental virtual hard-disk partition inspector

An **independent CLI**, `virtual_partition_inspect`, reads primary MBR,
linked extended MBR (EBR), and GPT partition tables from raw HDD `.img` and
fixed VHD images (512/4096-byte logical sectors).
It checks GPT CRC32 and partition overlaps and can recover a corrupted
primary GPT using an independently valid backup header and table. For example:

    virtual_partition_inspect C:\Images\disk.img
    virtual_partition_inspect C:\Images\disk-4k.img 4096

The native virtual-disk partition and FAT readers are now linked directly into WCX for supported images. A separate,
read-only `virtual_fat_inspect` CLI now lists and extracts FAT12/16/32
files from discovered raw IMG or fixed VHD partitions. NTFS/exFAT/ext
are not implemented. The optical ISO/BIN `.img` path is unchanged. See
[virtual disk architecture](docs/virtual-disks.md), informed by
[TalAloni/DiskAccessLibrary](https://github.com/TalAloni/DiskAccessLibrary)
(LGPL-3.0-or-later; no source copied or linked).


## Experimental C++26 and virtual HDD support

ISO-WCX ships its Windows x86/x64 WCX files using the proven **C++20** toolchain.
For developers, `-DISO_WCX_CXX_STANDARD=26` enables a separate, checked
experimental **C++26** compilation lane (GCC 14 and Clang 19 in GitHub Actions)
without changing the Total Commander ABI or release package.

The isolated `virtual_partition_inspect` tool can now parse MBR/EBR/GPT in
both raw HDD IMG and **fixed-size VHD v1** files. Fixed VHD validation includes
the EOF footer signature, one's-complement checksum, format version, disk type,
and exact logical size. No VHD data is written or mounted. The
**experimental native VHDX v1 reader** now handles clean fixed/dynamic base
images, including sparse BAT blocks and 512/4096-byte sectors, through
standalone partition and FAT tools. Differencing VHD/VHDX and VMDK remain
unsupported. The FAT-capable virtual disk readers are now linked into WCX;
real-image interoperability and Windows GUI extraction still need broader validation.
See [native VHDX design and test boundaries](docs/vhdx-native.md).

**No external disk DLLs:** The experimental libvhdi adapter and its CI
have been removed. ISO-WCX has its own native C++ raw IMG/fixed VHD reader.
We studied 7-Zip disk handler architecture but **did not copy its sources**.
Windows statically links zlib for compressed ISO support; no separate
zlib.dll or libvhdi.dll is shipped. See
[virtual disk architecture](docs/virtual-disks.md),
[7-Zip handler study](docs/7zip-disk-formats.md) and
[FAT12/16/32 read-only implementation](docs/fat-filesystems.md).

### FAT12/16/32 files inside WCX and standalone developer tests

`virtual_fat_inspect list disk.img 1` lists files in the first partition;
`virtual_fat_inspect cat disk.img 1 README.TXT` streams its bytes to stdout.
This supports verified nested directories, VFAT long filenames and
non-contiguous file cluster chains. No disk is mounted and no host path is
created from the image. FAT and virtual HDD support is now compiled directly into
the WCX DLL, alongside the existing optical ISO/UDF engine.

### Experimental MDS v2 / MDX metadata inspection

The new `mds_descriptor_inspect` utility safely distinguishes Alcohol MDS
v1, DAEMON Tools MDS v2, and embedded MDX. It reports the version and bounded
packed/encrypted metadata offsets without attempting to decrypt content.
The WCX plugin gives explicit unsupported-format errors for MDS v2 and MDX.
**This is not working MDS v2/MDX extraction, and no new .mdx association is installed.**
See the [Aaru issue #256 / mdsx research](docs/mds-v2-mdx-research.md)
for current limitations, TrueCrypt licensing notes, and the decoding roadmap.

### Consolidated optical metadata regression checks

RRIP PX (mode), TF (7/17-byte modification time), and mirrored CE pointers now have a dedicated integration + malformed-image regression in `tests/rock_ridge_metadata.cpp`. `tests/test_nrg_mds.py` no longer pretends that twice-running an MDS v1 fixture tests MDS v2; major version 2 is explicitly rejected. The supported MDS parser remains **v1 only** until independently validated v2 descriptors are implemented.

## What changed in v0.13.0 Preview

- Linked the independent native partition-table reader (MBR/EBR/GPT),
  VHD/VHDX block layer and FAT12/16/32 reader into the same WCX DLL as
  ISO9660/Joliet/Rock Ridge/UDF; no new runtime DLL or EXE.
- Added `partition1/`, `partition2/`, etc. for supported FAT volumes.
  Data is streamed through the existing extraction staging, rollback and
  progress handling, with full FAT chain validation.
- Raw HDD `.img` is distinguished by a validated partition map plus FAT
  directory tree. Optical images remain supported through the existing
  CD parser, and `.vhd`, `.vhdx`, `.raw` enter the HDD reader directly.
- Added end-to-end synthetic FAT12/16/32 IMG, fixed VHD, fixed/dynamic VHDX
  extraction and corrupt-chain rollback tests.
- The install ZIP now contains only WCX binaries, TC installation metadata
  and text documentation. No setup EXE or repair scripts are required.

**Unsupported:** NTFS, exFAT, ext2/3/4, VMDK, dynamic/differencing VHD,
differencing/dirty VHDX, MDS v2 and MDX extraction, DAA, ISZ and CHD.
Real-world media/Total Commander GUI validation is pending.
