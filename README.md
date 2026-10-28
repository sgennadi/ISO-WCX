# ISO-WCX

**ISO-WCX v0.21.0 Stable Core + Export Image Structure, WIM diagnostics and VS2026 C++26 draft** — a native Total Commander WCX
plugin for optical ISO/UDF, WIM/ESD/SWM and virtual HDD images.
Core browsing/extraction and packaging are verified in Windows x86/x64 and
Linux CI. **Advanced readers and write modes explicitly marked experimental
remain limited; this is not full NTFS/ext4/UDF compatibility.**

## Install into Total Commander

1. Open the [latest Stable release](https://github.com/sgennadi/ISO-WCX/releases/latest).
2. Download **ISO-WCX-v0.21.0.zip** and open it in Total Commander.
3. Let `pluginst.inf` install `ISO-WCX.wcx` (32-bit) and `ISO-WCX.wcx64` (64-bit).
4. Restart Total Commander and open a supported image with Ctrl+PgDn.

Development builds remain available through the [Preview workflow](https://github.com/sgennadi/ISO-WCX/actions/workflows/wcx-preview.yml).

**No separate setup EXE, parser DLL, unpacker, PowerShell script or mounting
service is needed.** Existing native helper CLIs are development/test programs,
not dependencies of the WCX installation.

If Total Commander still points to a removed old `...\ISO\iso.wcx`,
fix **Configuration → Options → Packer → Configure packer extension WCXs**:
remove only the obsolete `iso` association and add a new type pointing to
the installed `ISO-WCX.wcx`. Restart TC. The plugin itself cannot repair an
association that prevents its DLL from loading in the first place.

For installation see [INSTALL_EN.txt](INSTALL_EN.txt) and
[INSTALL_RU.txt](INSTALL_RU.txt); [PREVIEW.md](PREVIEW.md) describes
experimental support. See [v0.21.0 release notes](RELEASE_NOTES.md).

## Fixed VHDX FAT write (bounded v0.20.0 Preview)

Stable v0.20.0 includes verified FAT12/16/32 `partitionN/` changes in
**fully allocated fixed VHDX** images with 512-byte logical sectors only.
It uses private logical RAW editing, sector-precise physical VHDX staging,
CRC32C-updated dual headers, complete independent logical readback, and atomic
replace. Limits: 1 GiB logical and 2 GiB physical on x64; 512 MiB each on
Win32. Dynamic/sparse VHDX, 4K-sector VHDX, differencing/dirty-log images and
NTFS/exFAT/ext writes remain strictly read-only. No additional runtime DLLs,
helper executables or mounts. See [VHDX implementation](docs/vhdx-native.md).

## Modern Windows build toolchain (development Preview)

The **v0.21.0 Stable and development Preview** are compiled with **Visual Studio 2026 (v18),
MSVC v145, and experimental C++26 (`/std:c++latest`)** for Win32 and
x64. Both WCX architectures must pass all CTest and Total Commander ABI
smoke checks; GitHub also runs a VS2026 C++20 compatibility baseline.
Linux GCC 14 and Clang 19 C++26 tests remain independent. Microsoft's
latest-draft mode enables implemented C++26 features, not a claim that
the entire unfinished standard is supported. See
[toolchain policy](docs/toolchains.md).

Published **Stable v0.19.0 remains unchanged** on its original tested
VS2022/C++20 toolchain. Stable v0.20.0 instead uses VS2026/C++26 after
the full release-quality checks pass.

## Export Image Structure (Stable v0.21.0 and newer)

**Availability:** This feature is included in Stable v0.21.0 and newer
development builds. The previously published v0.20.0 ZIP does **not**
have the export button.

In Total Commander choose **Files > Pack**, select an extension configured
for ISO-WCX and click **Configure... > Export Image Structure...**.
For Joliet discs the report contains **both the displayed Joliet tree and
the independent ISO9660 primary tree**. ISO/UDF Bridge discs include
separate UDF and ISO9660 (and Joliet, when present) namespaces, plus
El Torito boot information and a specific warning about unsupported deletion.
The report includes file names and extents, not file contents; review paths
for sensitive information before sharing.
Choose the source ISO/UDF/WIM/VHD/VHDX or other supported image, then choose
where to save the UTF-8 `-ISO-WCX-Structure.txt` report. The report contains
the format reported by the **same in-process parser** as Total Commander,
the complete supported directory tree (filename, file/dir, byte size,
attributes, extents, offsets), and read-only ISO metadata when applicable:
2048-byte volume descriptors, PVD label, Joliet, El Torito boot catalog,
UDF recognition/anchor candidates and the first 32 KiB system-area flag.

The report exports **metadata only, never file payloads**. The source is
opened read-only and is not mounted, changed, repacked or copied. A private
temporary report is flushed and atomically replaced at the selected TXT
destination; cancelled dialogs do not create files. Works **without**
`disc_inspect.exe`, additional runtimes or separate utilities. The new
button is available after installing a build that contains this change;
published older Stable builds remain unchanged. If Configure... is missing,
reassociate the WCX plugin so Total Commander refreshes its capability flags.

## Experimental fast WIM folder creation (development Preview after v0.21.0)

The new WIMGAPI **mount-and-commit** path can avoid applying and recapturing
the complete WIM for a batch of empty directories in one image index.
It is **opt-in only**, either from the new checkbox in
**ISO-WCX → Configure** (Preview builds) or via
`ISO_WCX_WIM_FAST_MOUNT=1` in Total Commander's process environment.
The environment variable `0` forces the safe legacy path. The source is cloned transactionally,
the mounted index is committed only to the private copy, and the result
is independently enumerated and compared before source replacement.
The staging clone is still proportional to the WIM file size; the mount
driver and administrative access are required.

**Published Stable v0.21.0 does not include this Preview feature**. Until
privileged Windows mount, failure cleanup, ACL/ADS and real image tests
complete, the normal Apply/Capture edit remains the safe default.
See [WIM mount preview](docs/wim-mount-fast-preview.md).

## WIM read-only metadata validation (v0.21.0)

v0.21.0 adds strict read-only development-side WIM metadata preflight,
including bounds, UTF-16 names, security references, directory records
and alternate stream checks. It **does not** replace the native WIMGAPI
Apply/Capture WIM editing path and does not promise instant folder
creation in a large `install.wim`. This fast-writer groundwork is not
a visible WCX command. See [WIM metadata research](docs/wim-metadata-tree.md).

**Hybrid-boot ISO/Joliet deletion (PR #65) is still Preview-only**, not
in the v0.21.0 Stable release. Real bootable ISO verification is needed
before the safety gate can be relaxed.

## Built-in update checker (introduced in v0.19.0)

ISO-WCX v0.19.0 includes an in-process updater.
It requires **no standalone updater EXE, downloaded DLL or administrator rights**
to check release information. The update package itself is installed only by
Total Commander after user confirmation.

- **Manual:** In Total Commander, open **Files → Pack**, choose a packer
  extension associated with ISO-WCX, then **Configure... → Check now**.
- **Automatic:** Once per day, on plugin initialization, a background WinHTTP
  request checks the **latest Stable release** on our GitHub repository.
  A new version is shown only once; browsing/extraction never wait for the network.
- **Disable automatic checks:** Choose **Do not check for updates automatically**
  (Russian: **Не проверять обновления автоматически**). Stored per user at
  `HKCU\\Software\\ISO-WCX\\Updates\\AutoCheck` with `0` meaning off.
  The **Check now** button still works when automatic checking is disabled.
- **Download:** The updater obtains the official ZIP and `SHA256SUMS.txt`
  using HTTPS/WinHTTP and checks SHA-256 with built-in Windows BCrypt.
  It stages the ZIP in a random temporary directory and offers to open it in
  Total Commander. It **never overwrites a loaded WCX DLL**; finish the
  install and restart Total Commander to activate the updated plugin.

**Upgrading from v0.18.0:** Total Commander can cache packer capability flags.
After installing a build with updater support, reinstall the WCX association
or update its cached flags if the **Configure...** button is missing, then
restart TC. This updater is not present in the original v0.18.0 Stable build.

## Format coverage (experimental subsets identified separately)

| Format/layout | Coverage |
| --- | --- |
| WIM / ESD / SWM on Windows | Browse/extract multiple `imageN/` indexes with system WIMGAPI (XPRESS/LZX/LZMS as supported by Windows), including complete split SWM sets validated by GUID and sequence. **Edit:** standard unsplit WIM only, using Windows WIMGAPI and an NTFS staging workspace with elevated privileges; ESD/SWM read-only. No external utility or extra DLL. |
| ISO 9660 / Joliet | Browse/extract Unicode names, multi-extent and interleaved files; fallback from corrupt Joliet. **Edit:** add/replace/delete files and folders in validated plain 2048-byte ISO9660/Joliet images; experimental long primary ISO identifiers up to 221 bytes and relaxed Joliet identifiers up to 110 UTF-16 units, including surrogate pairs. Deterministic collision-checked short aliases and El Torito boot-extent safeguards. Rejects system-area hybrid and Rock Ridge/multi-extent rebuilding. **ISO/UDF Bridge (PR #67 Preview):** equal-size replacement of an existing nonbootable shared-extent file; and **restricted synchronized deletion** of regular, nonempty, nonboot files with the same single physical data extent in ISO/Joliet and UDF Type-1 **without a bitmap/LVID allocator**. Deletion compacts ISO/Joliet directory records in place and marks the corresponding UDF FID deleted with corrected CRC in one private transaction. Reparse both namespaces before atomic commit. No general add/mkdir, resized replacement, directory deletion, Rock Ridge/hybrid system-area edits, bootable Bridge edits or UDF allocation recovery. See [Bridge deletion constraints](docs/iso-udf-bridge-deletion.md). |
| Rock Ridge | RRIP NM/CE and relocated CL directories; PX read-only and TF modification timestamps; no symlink/device extraction |
| Raw BIN/IMG | CD Mode 1 / Mode 2 Form 1 (2352-byte), XA Form 1 (2336-byte); automatic probing limited to single-track images |
| CUE/BIN | BINARY data tracks (`MODE1/2048`, `MODE1/2352`, `MODE2/2352`, `MODE2/2336`), INDEX 00/01, multiple FILE entries, audio-track skipping; latest recognizable data track |
| CloneCD CCD/IMG | `.ccd` version 3 + matching `.img` (2352-byte sectors), single-session Mode 1 / Mode 2 Form 1 data tracks, including ECMA-130 scrambled sectors. Optional `.sub` sidecar exposed byte-for-byte under `subchannels/disc.sub`; CD-Audio and CD-Text decoding not implemented; CRC-valid Q track/index frames summarized in `subchannels/q-index.csv`. |
| 150-sector prefix | Relative and some absolute-LBA cases (not full multisession parsing) |
| UDF Type 1 | Browse FE/EFE, OSTA CS0 Unicode, AED continuations and reserve VDS failover. Read-only whole-disk UDF 2.01 with 512–32768-byte power-of-two blocks. **Write Preview (validated, nonhybrid UDF-only 2048-byte physical Type-1):** atomic multi-file and nested directory creation; mixed create/replace/logical-delete; bounded FE/ShortAD resize; multiple extents and files over 4 GiB; safe exclusive-ownership deleted FE/ShortAD reclamation and next-transaction block reuse; reuse of verified deleted FID slots to prevent directory growth. Atomic staged verification with optional CoW reflink/block clone and sparse fallback. **Not** a general UDF writer: general ISO/UDF Bridge rewriting (except the narrow synchronized file delete above), VAT/metadata/sparable writes, symlinks, hard links and AED chain creation remain unsupported. See [UDF writer](docs/udf-type1-writer.md) and [portability](docs/udf-cross-platform.md). |
| UDF Metadata / VAT / Sparable | Experimental read-only partition-map support with synthetic fixtures, not comprehensive UDF 1.02–2.60. The UDF reader exposes regular files and directories; symlinks, block devices and other special ICB types are not materialized as ordinary files. |
| Nero NRG | Uncompressed NERO / NER5, DAOI/DAOX and ETNF/ETN2 data tracks; reads the latest detectable data track |
| Alcohol MDS/MDF | MDS v1 session/track descriptors with sidecar MDF (2048/2336/2352/2448 data sectors). **Preview: MDS v2 / monolithic MDX**, native AES-decrypted/zlib-inflated descriptor, plain and compressed (stored/RLE/raw-DEFLATE groups) unencrypted data-track random access, extraction through WCX with real MDF/MDX ISO filesystem fixtures. Password-protected AES-LRW sectors, other optical variants and writing remain unsupported. |
| El Torito | Exposes validated BIOS and EFI boot-load images and the boot catalog under `boot.images/`; EFI FAT BPB sizing when valid |
| Raw floppy IMA / IMG / VFD / FLP | **Preview:** FAT12/16/32 superfloppy detection without MBR/GPT; native list/extract. Transactional **FAT12/16/32 write Preview** using demand-paged, disk-backed atomic staging (up to **64 GiB x64** / **512 MiB Win32**) for exact-size raw superfloppies and validated `partitionN/` FAT volumes inside **RAW/IMG and fixed VHD**: native UTF-8/UTF-16 **VFAT long names** (Cyrillic, Hebrew, emoji), collision-aware DOS 8.3 aliases, add/replace/delete and folder operations. FAT16 uses fixed root and mirrored 16-bit FAT chains; FAT32 uses dynamic root, mirrored 28-bit allocation chains with preserved reserved nibble, checked FSInfo/backup boot and updated free-cluster hints. Refuses corrupt mirrors, FSInfo/backup mismatch, cycles, unsafe filenames, ambiguous partition selections, mounted/live source images and oversized disks; differencing VHD and most VHDX stay read-only; fully allocated fixed VHDX with 512-byte logical sectors permits bounded FAT writes (1 GiB logical/2 GiB physical x64 or 512 MiB each Win32); base dynamic VHD FAT partition writes have a separate 1 GiB x64 / 512 MiB Win32 Preview limit. Atomic staging and readback before replacing the original; no helper utilities or parser DLLs. **These writers are included as bounded Preview features in v0.19.0.** No external programs or DLLs. |
| WinImage IMZ compressed floppy | **Preview:** in-process single-member ZIP stored/raw DEFLATE, CRC32 and ZIP directory validation, output limited to 32 MiB; FAT12/16/32 browse/extract without a temporary image. Rejects encryption, multiple members, ZIP64, malformed streams and excessive expansion. Read-only. |
| BIF El Torito boot images | **Preview:** valid raw FAT images browseable like IMA; opaque no-emulation BIOS boot images expose original bytes as `boot-image.bin`. Opaque BIF stays read-only. |
| Partitioned HDD IMG/RAW, fixed/dynamic VHD, clean base VHDX | Native MBR/GPT filesystem browsing under `partitionN/`: FAT12/16/32, exFAT and **experimental** ext2/3/4 and NTFS. NTFS supports basic MFT/`$I30` directory parsing, resident/nonresident and sparse uncompressed files; rejects attribute lists, compressed/encrypted files and unsupported index layouts. Read-only except for Preview FAT `partitionN/` writes in raw IMG/RAW and **fixed** VHD (exact source clone and atomic replace); **dynamic VHD** is a native, checksummed, sparse BAT/bitmap reader with **bounded FAT12/16/32 transactional write Preview** for validated base images (1 GiB x64 / 512 MiB Win32). No mount/driver/external parser DLL. VHDX supports clean fixed/dynamic base image reads and bounded FAT writes only in fully mapped fixed 512-byte-sector VHDX. |
| Original Xbox XDVDFS | Read-only files, nested directories and timestamp metadata (original Xbox only, not Xbox 360 or Xbox One) |
| Multisession descriptor tracks | When multiple CUE/NRG/MDS tracks contain readable filesystems, expose separate `session1/`, `session2/` virtual folders. Not a full merged multisession filesystem. |
| ISZ v1 (single-file and split; experimental) | Built-in 2048-byte optical streams; 48/64-byte headers, 2/3/4-byte chunk indexes or implicit stored data, zero/raw/zlib/bzip2; split `.isz` + `.i01/.i02` or `.part01.isz` + `.part02.isz` without intermediate ISO. Encryption, non-first segment as input, other sector geometry and uncommon vendor variants remain unsupported. |
| DAA v1.00 | Built-in single-file raw DEFLATE chunks with header CRC32 and bounds checking; v1.10, encryption and multipart images rejected |
| CHD v5 (optical) | Native static libchdr compressed maps and zlib/LZMA/Zstd/FLAC/CD codecs; 2048-byte cooked ISO/UDF and **experimental multi-track** Mode 1/Mode 2 Form 1 CD frames (2448 bytes). MAME 4-frame track padding is mapped, the last ISO/UDF-bearing data track is selected after filesystem validation, and **audio-only/mixed CHDs expose `CDDA/trackNN.wav`** (16-bit stereo PCM at 44.1 kHz, RIFF or RF64 for large tracks). No CD-Text, CD+G graphics, parent-chain resolution, CHD writing or full multisession merging; `.chd` association remains manual in Preview |
| CSO v1/v2 / ZSO | Indexed random-access ISO reads; raw DEFLATE / LZ4 and stored blocks, including CSO v2 mixed-codec images; no intermediate ISO |
| Other compressed/encrypted containers | Not implemented: encrypted ISZ, multipart DAA, DAA v1.10, CD-Audio extraction from non-CHD CUE/CCD/NRG containers, CHD parent chains, encrypted/compressed NRG, MDS v2/MDX with password-protected AES-LRW data tracks, unsupported vendor-specific MDF compression and full optical multisession merging; CloneCD Q decoding is limited to CRC-valid positions |

## Development

Build with CMake 3.21+, a C++20 compiler and Python 3 for tests:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --parallel
    ctest --test-dir build --output-on-failure

Windows Stable v0.20.0 uses Visual Studio 2026 with CMake
(`-G "Visual Studio 18 2026" -A x64 -DISO_WCX_CXX_STANDARD=26`,
or `-A Win32`). CI produces both binary architectures and tests WCX DLL APIs.

For a minimal Windows release build containing **only the WCX DLL**, use
`-DBUILD_TESTING=OFF -DISO_WCX_BUILD_DEV_TOOLS=OFF` and
`cmake --build build --config Release --target ISO-WCX`.
The standalone diagnostic/test EXEs are internal development targets, not
WCX runtime dependencies or parts of the installable ZIP.

This is an independent modern reimplementation; original ISO.WCX 1.7.6 source code is **not** included. See [PROVENANCE.md](PROVENANCE.md) and [tracking issue #1](https://github.com/sgennadi/ISO-WCX/issues/1). CUE references are restricted to image files inside the CUE directory tree (including resolved symlinks). Full multisession TOC interpretation and CD-Audio outside the experimental CHD reader are not supported. NRG and MDS v1 support remains limited to verified uncompressed data tracks; native MDS v2/MDX plain and compressed reads pass a pinned independent real-image corpus, but broader discs and Total Commander GUI operation still need manual validation. Associations for `.nrg`, `.mds`, `.cso`, `.zso`, `.isz` and `.daa` are registered by the ZIP installer, while `.chd`, `.bin` and `.mdf` remain unassociated. Preview images were synthetically constructed; tests in the Total Commander GUI and against a diverse real-disc collection remain outstanding.

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

In v0.8 the adapter was read-only; current v0.15 supports restricted ISO/WIM
editing and remains **Preview** quality. We have not demonstrated compatibility with every optical-media image format, commercial disc or live Total Commander installation. Destination path symlinks are checked, but extracting into a directory modified concurrently by an adversary remains outside the current threat model.

## Historical changelog

The following release notes describe the feature coverage **at each older version**. For current capabilities, consult the format table above and the v0.14 notes below.

## What changed in v0.9.0

- **El Torito:** inspect the ISO boot record and validated boot catalog; expose read-only BIOS and EFI boot images in the virtual `boot.images` directory. Advertised no-emulation SectorCount is a boot-load size, not proof of the entire image length; EFI FAT BPB is used only after validation. Invalid catalog checksums or out-of-bounds images are rejected.
- **Original Xbox XDVDFS:** parse the volume descriptor, sibling directory tree and nested folders with cycle checks, bounded metadata and secure filenames. This does not support Xbox 360 XGD discs.
- **Descriptor-driven multisession views:** enumerate more than one filesystem-bearing CUE, NRG or MDS track under `session1/`, `session2/` etc. Track indices are preserved for extraction, including earlier tracks. Does not replace a full TOC-based multisession implementation for raw `.iso`/`.bin` files.
- **Metadata:** ISO 9660 and UDF timestamps and file attributes are passed through the WCX API. Xbox XDVDFS entries inherit the volume creation date when per-file timestamps are unavailable.
- **Security and rollback:** preserves the v0.8 staging-file extraction and single Unicode progress callback behavior.
- **Validation:** independent synthetic tests for El Torito BIOS/EFI, XDVDFS nested trees, multisession extraction and malformed descriptors; Linux tests and sanitizers. MDS v2, MDX, ISZ, CHD and DAA were **not supported at that point in the project history**; indexed CSO v1/v2 and ZSO are already implemented and tested synthetically.


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

The remaining roadmap includes *MDS v2/MDX AES-LRW encrypted track data and unusual variants, broader ISZ/DAA/CHD support, DMG, ECM, GZ/XZ, CDDA from CUE/NRG/CCD and CD-Text*. Partial ISZ/DAA/CHD support arrived in v0.14; formats supported by other projects are not automatically supported here.

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
files from discovered raw IMG or fixed VHD partitions. This paragraph
records the older v0.12 state; exFAT and limited ext2/3/4 and NTFS readers
arrived in subsequent preview development. The optical ISO/BIN `.img` path is unchanged. See
[virtual disk architecture](docs/virtual-disks.md), informed by
[TalAloni/DiskAccessLibrary](https://github.com/TalAloni/DiskAccessLibrary)
(LGPL-3.0-or-later; no source copied or linked).


## Experimental C++26 and virtual HDD support

Starting with **Stable v0.20.0**, Windows x86/x64 WCX binaries use
**VS2026 C++26 draft mode**, preserving the Total Commander WCX ABI.
Both architectures are also built/tested separately in C++20 compatibility
mode. Linux GCC 14 and Clang 19 C++26 remain checked by GitHub Actions.

The isolated `virtual_partition_inspect` tool can now parse MBR/EBR/GPT in
both raw HDD IMG and **fixed-size VHD v1** files. Fixed VHD validation includes
the EOF footer signature, one's-complement checksum, format version, disk type,
and exact logical size. VHD *filesystem* writes are confined to an explicitly selected validated FAT `partitionN/` within a fixed VHD via atomic clone-and-replace; the 512-byte footer and all other partition bytes must remain intact. No VHD is mounted. The
**experimental native VHDX v1 reader** now handles clean fixed/dynamic base
images, including sparse BAT blocks and 512/4096-byte sectors, through
standalone partition and FAT tools. Differencing VHD/VHDX and VMDK remain
unsupported. The FAT-capable virtual disk readers are now linked into WCX;
real-image interoperability and Windows GUI extraction still need broader validation.
See [native VHDX design and test boundaries](docs/vhdx-native.md).

**New Preview: dynamic VHD v1 reader and bounded FAT write.** The native reader checks both
`conectix` footers, the `cxsparse` header, BAT physical extents and
MSB-first per-sector bitmaps. Unallocated sectors are zero-filled and
MBR/GPT filesystems are presented under `partitionN/`. The same
FAT12/16/32, exFAT and limited NTFS/ext filesystem readers are reused.
**Write Preview (v0.19.0):** verified non-differencing **dynamic**
VHD v1 FAT12/16/32 `partitionN/` edits stage a full logical raw disk, call the
same native FAT editor, then apply only changed 512-byte sectors to a private
physical VHD clone. BAT and sector bitmaps are extended when needed. The
staged dynamic image is re-opened and compared against the edited logical
disk **byte-for-byte** before original-path replacement. Safety cap:
**1 GiB logical x64 / 512 MiB Win32** and enough free space for multiple
temporary disk copies. Read/write changes require no mounts or helper DLLs.
Differencing/AVHD parents, dirty VHDX, dynamic VHD corruption and non-FAT
filesystem writes remain unsupported. Dynamic VHD writes are a **bounded Preview included in v0.19.0 Stable Core**;
they are not general-purpose virtual-disk writes.

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

The developer-only `mds_descriptor_inspect` utility recognizes Alcohol
MDS v1, DAEMON Tools MDS v2 and embedded MDX. With `--decode`, the
experimental native AES-256/PBKDF2/RIPEMD160/zlib backend decrypts and
inflates *metadata only* (tested on pinned real encrypted descriptors).
The WCX plugin still rejects MDS v2 and MDX data-track extraction.
**This is not working MDS v2/MDX extraction, and no new .mdx association is installed.**
See the [Aaru issue #256 / mdsx research](docs/mds-v2-mdx-research.md)
for current limitations, TrueCrypt licensing notes, and the decoding roadmap.

### Consolidated optical metadata regression checks

RRIP PX (mode), TF (7/17-byte modification time), and mirrored CE pointers now have a dedicated integration + malformed-image regression in `tests/rock_ridge_metadata.cpp`. `tests/test_nrg_mds.py` no longer pretends that twice-running an MDS v1 fixture tests MDS v2; major version 2 is explicitly rejected by the production WCX track reader. Metadata decryption is independently validated but MDS v2 sector extraction remains unsupported.

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

**Unsupported at the time of v0.13:** NTFS, exFAT, ext2/3/4, VMDK, dynamic/differencing VHD,
differencing/dirty VHDX, MDS v2 and MDX extraction, encrypted/segmented ISZ,
DAA v1.10 or encrypted DAA, audio/multitrack CHD and CHD parent chains.
Real-world media/Total Commander GUI validation is pending.

## What changed in v0.14.0 Preview

- A native `PackedOptical` block reader is compiled directly into
  `ISO-WCX.wcx` and `ISO-WCX.wcx64`; no external extractor, driver,
  intermediate ISO or optional DLL is required.
- ISZ v1 single-file data: zero, stored, zlib and **statically built bzip2**
  chunks with bounded index, decompression and read cache.
- Unencrypted DAA v1.00 single-file images: validated header CRC32, three-byte
  block length index, raw RFC1951 DEFLATE streams.
- CHD v5 compressed optical streams using statically built libchdr and its
  in-tree zlib, LZMA, Zstandard, FLAC and CD codecs. Read-only cooked DVD/ISO
  and experimental single-track Mode 1 / Mode 2 Form 1 CD data; unsupported
  audio, multi-track and parent layouts are rejected. No runtime library.
- Linux and Windows x86/x64 tests exercise synthetic ISO/UDF data, byte
  extraction and corrupt-container rejection. Windows tests use the actual
  WCX export functions, not an external extractor.
- No automatic `.chd` association yet: unsupported CHD parent chains, sector
  layouts and other image classes still make blanket CHD registration misleading.
  MDS v2/MDX and protected ISZ/DAA remain **unsupported**.

See [bounded optical container design](docs/packed-optical.md).

## What changed in v0.15.0 Preview

- Browse WIM or Windows-compatible ESD inside Total Commander using the
  system WIMGAPI already installed with Windows.
- Exposes each edition/index as `image1/`, `image2/`, etc., with nested
  folders, file sizes and DOS timestamps/attributes.
- Streams individual files through the existing WCX atomic extraction and
  cancellation path. No DISM, wimlib-imagex.exe, mount, external DLL or driver.
- Split SWM sets: open any part with all `.swm` files in the same folder.
  Parts are matched by GUID and part number, including renamed files.
- Plain ISO9660/Joliet editing adds/replaces/removes files and folders
  transactionally; unsupported hybrid/Rock Ridge layouts fail without
  modifying the input. A narrow ISO/UDF Bridge mode permits only identical-
  length replacement of nonboot files whose physical data extent is shared
  across both namespaces; metadata-changing Bridge edits are refused.
  Existing supported boot-image sectors are preserved in plain ISO editing.
- Standard unsplit WIM editing can add/replace/remove files and directories,
  and remove existing image indexes except the final one. It uses native
  WIMGAPI apply/capture with an NTFS workspace and elevated backup/restore/
  security privileges. The result is validated before replacing the input.
- ESD/SWM, virtual disks, complex UDF and compressed optical formats remain
  read-only. The experimental UDF-only Type-1 allocator supports bounded new
  files/empty directories, contiguous directory expansion and existing-file
  replacement, but **deleted file blocks are not reclaimed**. Superseded
  directory extents may be reclaimed after ownership checks. ISO/UDF Bridge
  remains limited to shared-extent equal-length replacement; hybrid UDF
  metadata rebuilding is not implemented.
  Individual WCX file extraction does not restore alternate data streams or
  reparse points. Unsupported ESD compression remains excluded.
- See [WIM design notes](docs/wim-integration.md).


## Preview filesystem expansion (post v0.15)

Native exFAT 1.0 browsing/extraction was added in PR #27. The next
experimental ext2/3/4 module supports direct/indirect legacy block maps,
ext4 extent trees, sparse files and nested directories in partitioned
IMG/VHD/VHDX. It deliberately refuses unsupported features such as journal
replay, inline data and encrypted inodes. This is a **partial, read-only**
ext implementation, not full modern ext4 compatibility. Experimental NTFS
read-only support (PR #29) parses basic MFT records, `$I30` indexes,
UTF-16 filenames, resident and fragmented/sparse nonresident files.
It rejects attribute-list continuations, encrypted/compressed data,
reparse reconstruction and journal recovery. See [exFAT design](docs/exfat-integration.md),
[ext reader design](docs/ext-integration.md) and [NTFS preview](docs/ntfs-integration.md).

## Native UDF Type-1 writer (experimental)

The in-process writer edits compatible standalone 2048-byte UDF Type-1 images
without third-party DLLs or executables. It can replace/grow/shrink existing
regular files within their recorded FE/EFE or one ShortAD allocation and
logically delete file FIDs (without freeing deleted file blocks). The Preview
allocator can create **one new regular file bounded by image/partition space
and FE ShortAD capacity** or **one empty directory** per atomic transaction,
updating the bitmap, LVID and unique IDs. For nonembedded files the writer
supports several fragmented ShortAD runs and streamed verification without
allocating a second extracted copy. Writes beyond 4 GiB remain experimental
and require broader independent large-media coverage.

Parent directories may automatically move from inline FE data to contiguous
ShortAD blocks and grow across multiple sectors (8 MiB directory-data limit).
The writer keeps FIDs tightly packed even across sector boundaries; validates
the allocated run and reclaims superseded directory blocks only when
ownership is unambiguous. The original image is not replaced until a staged
copy is structurally reparsed and extracted bytes verified.

Hybrid ISO/UDF Bridge mutation, FAT/NTFS in virtual drives, VAT/Metadata/
Sparable writers, sparse/unrecorded allocation descriptors, AED continuation
writing, deleted-file block reclamation and unsupported layouts remain
read-only or are explicitly refused. See
[UDF writer scope](docs/udf-type1-writer.md) and
[portable UDF recognition](docs/udf-cross-platform.md).
