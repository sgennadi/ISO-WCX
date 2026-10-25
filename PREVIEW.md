# ISO-WCX v0.17.0 Stable Core + Transactional UDF Writer Preview

Modern Total Commander WCX plugin with a Stable installation channel and explicitly restricted ISO/WIM/UDF editing. The codebase is clean-room and does not republish the legacy ISO.WCX 1.7.6 sources. Built with CMake and Visual Studio 2022 (Windows x86/x64).

The UDF Type-1 transactional writer was introduced in v0.17.0 and remains **experimental** in v0.18.0, with strict format preconditions and atomic rollback. Stable Core refers to verified package installation and bounded core capabilities, not universal filesystem writing.

## If Total Commander says `File iso.wcx not found!`

This indicates a **stale old association** such as `...\\plugins\\wcx\\ISO\\iso.wcx`; it does **not** indicate that `ISO-WCX.wcx` or its adjacent 64-bit `ISO-WCX.wcx64` is missing. The automatic installer cannot remove existing old plugin associations. In Configuration > Options > Packer > Configure packer extension WCXs, select `iso`, delete **only** the obsolete missing ISO.WCX entry, then click **New type...** and choose the installed `ISO-WCX.wcx`. Restart Total Commander and retry with Ctrl+PgDn. Do not type `192,` in the filename field or delete unrelated working plugins. See [INSTALL_RU.txt](INSTALL_RU.txt) and [INSTALL_EN.txt](INSTALL_EN.txt).

## Supported in the preview

- CUE/BIN track selection: opens `.cue` directly, parses multiple FILE/TRACK and INDEX 00/01 records, skips audio and unsupported track types, and chooses the last ISO/UDF-bearing data track. Supports 2048, 2336, Mode 1/2352 and Mode 2 Form 1/2352 layouts; extracts files through the same read-only WCX API.
- ISO9660 and Joliet, including Unicode names, ISO multi-extent/interleaved files, and primary-volume fallback for damaged Joliet.
- Rock Ridge RRIP NM/CE names, CL relocated directory references, PX non-writable/read-only mapping, TF modification timestamps and mirrored CE/PX number validation; symbolic links and POSIX special files are ignored rather than followed.
- 2048-byte logical images, raw 2352-byte CD Mode 1 and Mode 2 Form 1, and 2336-byte XA Form 1 frames (no Form 2).
- Some images with 150-sector lead-in (relative and absolute session LBA conventions), but **not full multisession TOC/CUE parsing**.
- UDF Type-1 physical mapping, FE/EFE, embedded/short/long allocation, AED continuations, CS0 Unicode and reserve VDS. **Write Preview for validated UDF-only physical 2048-byte Type-1:** create multiple regular files and nested empty directories atomically; mix creation, bounded replacement and logical deletion; reallocate compatible parent directories; reclaim verified deleted FE/ShortAD blocks for the following transaction; safely reuse cleared deleted FID slots without increasing directory length; support bounded multi-ShortAD files beyond 4 GiB. Bitmap/LVID and CRC are verified. A sibling transactional stage uses native CoW cloning on supported filesystems or sparse copy fallback, never direct writes to the original. No VAT, Metadata, Sparable, AED chain creation or ISO/UDF Bridge writer. See [UDF writer](docs/udf-type1-writer.md).
- **Experimental** UDF Metadata, Virtual (VAT) and Sparable partition-map reading, including mirror metadata fallback and packet redirects. UDF symbolic links (ICB type 12) and device/special ICBs are deliberately excluded from ordinary WCX extraction; they are never followed or materialized as regular files.
- WCX list/test/extract for supported images, streamed file data and 64-bit sizes. Restricted PackFiles/DeleteFiles support covers validated plain ISO9660/Joliet, UDF-only physical Type-1 images (experimental), and standard unsplit WIM on Windows.

## Limits and cautions

- This is a **preview** tested primarily with deterministic synthetic image fixtures. Real DVD/BD/UDF compatibility is not yet guaranteed, especially rewritable/packet-written and damaged media.
- RAW Mode 2 Form 2, CD-Audio extraction from CUE/NRG/CCD (CHD CDDA is supported), CD-Text and full CloneCD multisession are not supported. CloneCD raw `.sub` is preserved and position-type Q frames are CRC-checked; damaged Q data is not treated as an ISO failure. NRG and MDS select the latest recognized data track, but this is not full multisession filesystem merging.
- UDF VAT historical chains, every version of Metadata/Sparable, UDF pseudo-overwrite behavior and fragmented descriptor edge cases are not yet exhaustively covered.
- Rock Ridge PX read-only permissions and TF modification timestamps are implemented in the development branch, with CE mirrored-endian validation. Full POSIX permissions, symlink extraction and creation/access times are not exposed by the WCX API.
- ISZ v1 zero/stored/zlib/bzip2 and DAA v1.00 unencrypted raw DEFLATE are supported in the current native reader. CHD v5 supports compressed 2048-byte optical payloads and **experimental** mixed/multi-track 2448-byte CD images, selecting an ISO/UDF-bearing Mode 1/Mode 2 Form 1 data track after validating its filesystem anchor. MAME four-frame alignment is respected; **Red Book CDDA tracks are exposed as `CDDA/trackNN.wav`** on audio-only and mixed CHDs. The decoder generates RIFF PCM 16-bit stereo 44.1 kHz, or RF64 above the WAV 4-GiB limit, with 1-MiB bounded reads and no additional codec or helper. Broader encrypted/proprietary variants and DMG/encrypted NRG remain unsupported; MDF v2 unencrypted payloads with bounded compression-group decoding are supported in Preview. Indexed CSO/ZSO and uncompressed NRG/MDS v1 remain supported.
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

CI builds both Windows WCX architectures, runs parser and extraction tests, and packages only ISO-WCX.wcx, ISO-WCX.wcx64, pluginst.inf and human-readable instructions. No Setup.exe, third-party disk DLLs, PowerShell or CMD scripts are installed.

## Native HDD images inside WCX (v0.13 preview)

The plugin opens partitioned raw HDD IMG/RAW, fixed VHD and clean fixed/dynamic base VHDX through native C++20 MBR/GPT and filesystem readers. Read-only FAT12/16/32, exFAT and experimental ext2/3/4 and NTFS subsets appear under partition1/, partition2/, etc. NTFS covers basic MFT and $I30 directory indexes, Unicode names and ordinary resident/fragmented/sparse uncompressed files, with deliberate feature refusals. Extraction uses the same staging, rollback and progress controls as ISO/UDF. Unsupported/unknown partitions are skipped. Dynamic/differencing VHD, VHDX parent/dirty-log images, complex NTFS attribute-list/compression/encryption layouts and journal recovery are not supported. No external tools, drivers or disk-parser DLLs are needed.

## Experimental native MDS v2 / MDX archive handler

The developer-only `mds_descriptor_inspect` test utility detects MDS v1,
encrypted MDS v2, and MDX metadata envelopes with strict offset checks.
Using its optional `--decode` mode, it decrypts and inflates bounded MDS v2 / MDX metadata in-process (AES-256, RIPEMD160/PBKDF2 and zlib). The same metadata backend is now used by the WCX archive reader.
**Experimental extraction is enabled in Preview:** the WCX plugin opens ordinary and compressed MDS v2/MDF and single-file MDX images, selecting a validated ISO9660/UDF data track. Its in-process random-access decoder reads raw/stored/RLE/DEFLATE compression groups without creating a temporary ISO. Four pinned real MDS v2/MDX fixtures have passed filesystem content comparison against the reference ISO. The installer now associates `.mdx`. Password-protected AES-LRW data tracks, unsupported optical modes, image editing and full multisession merging remain unavailable. See [format limits and provenance](docs/mds-v2-mdx-research.md).

## NRG and MDS/MDF preview support

- Nero `.nrg`: NERO (32-bit) and NER5 (64-bit) trailer, bounded chunk list, DAOI/DAOX (disc-at-once) and ETNF/ETN2 (track-at-once), selected latest ISO9660/UDF data track.
- Alcohol `.mds`/`.mdf`: validated MDS v1 session/track/filename/index blocks with cooked 2048/2336 and raw Mode 1/Mode 2 Form 1 tracks. **Preview MDS v2/MDX:** encrypted metadata and on-demand unencrypted data-track decompression with bounded raw-DEFLATE, stored and repeated-byte groups. `.mdf` is a disk-image sidecar, not a Microsoft SQL database handler.
- Raw-image probing is still available for `.img`/`.bin`; preview installer associations include `.iso`, `.cue`, `.nrg`, `.mds` and `.mdx` but **not** `.bin` or `.mdf`, avoiding conflicts.
- Synthetic NRG/MDS and malformed-metadata regression tests run on Linux and Windows. An additional pinned real-image corpus independently verifies native MDS v2/MDX filesystem extraction and refuses password-protected tracks; broader Nero/Alcohol compatibility and real Total Commander GUI operation still require manual testing.

## v0.7.0 compatibility improvements and verification

- ISO interleaving: each File Unit Size is read, skipped Interleave Gap Size sectors are not copied, and extents are assembled in the original logical file order. Multi-extent files can include multiple interleaved sections.
- Invalid ISO interleaved directories and malformed unit/gap combinations are rejected.
- An invalid Joliet directory tree no longer hides a readable ISO9660 primary volume. The active format reports `ISO 9660 (Joliet fallback)`.
- UDF 16-bit OSTA CS0 names support legitimate UTF-16 surrogate pairs; truncated/unpaired surrogates are rejected.
- UDF reserve VDS fallback is tried when the primary VDS is damaged, using the same anchor and validating the file-set structures.
- The regression suite includes 12 deterministic v0.7 success/failure cases and an optional **independently mastered** ISO/Rock Ridge image generated by libarchive on Linux (test is explicitly skipped if libarchive is not available). The independent image tests nested paths, non-ASCII names, 1 MiB data, empty files and safe symlink omission.
- This is not evidence that all real DVD/BD images work. Compatibility of actual user images and live Total Commander UI remains to be measured before a stable release.

## v0.9.0: transactional extraction and Total Commander progress

- File extraction uses a random same-directory `.iso-wcx-*.part` temporary file. Cancellation, source truncation, malformed extents and output errors remove the temporary file. Existing output remains unchanged unless the new file fully commits. 
- Both callback types can be registered by Total Commander; the Unicode progress callback takes precedence, avoiding duplicate byte counts. The ANSI callback is used only when Unicode is absent.
- The regression suite includes native cancellation and rollback checks, and the Windows WCX smoke tests verify both callback modes and `E_EABORTED` (21).
- Linux Clang ASan/UBSan CI scans parser and extraction regression runs; preview installers remain unsigned.

**Concurrent file-system modifications:** This protection preserves existing destinations on ordinary failures, but it is not a substitute for an adversarially secure extraction sandbox. The destination-directory symlink check cannot prevent all external races.

## Historical release notes (older versions)

Support statements below describe their respective releases; see **v0.14** at the end for current additions.

## New in v0.9.0: boot, Xbox and sessions

- El Torito BIOS/EFI `boot.images` support from validated boot catalogs. EFI FAT-BPB-backed larger extraction is attempted only when its byte size is within the image. Boot-catalog entries do not guarantee the size of an entire no-emulation image; avoid treating incomplete boot images as full partition dumps.
- Original Xbox XDVDFS directory-tree listing, nested read-only extraction, and volume creation timestamps. Xbox 360/One proprietary formats are not implemented.
- Descriptor-proven CUE/NRG/MDS filesystems across multiple tracks appear as `sessionN` virtual directories. The plugin does not infer unknown sessions or merge their directories.
- ISO/UDF timestamps and file attributes are made available to the Windows WCX adapter.
- Unsupported: MDS v2, MDX, ISZ, DAX, CHD, DAA, UIF and compressed/encrypted NRG/MDF variants; CSO/ZSO are implemented.


## v0.10.0 compressed ISO and raw MDF support

- Read-only `.cso` v1 (including legacy version 0 headers), `.cso` v2 (mixed raw DEFLATE/LZ4) and `.zso` LZ4. A fixed-size block index is validated before any on-demand decompression; individual blocks are decoded only when the filesystem parser or extractor needs them.
- Zlib is linked statically on Windows using a pinned upstream source version, while Linux uses the system package. No runtime Python process, full-image unpack or additional DLL is needed by Total Commander.
- 2448-byte physical sectors are accepted for unswizzled raw MODE1/MODE2 Form 1 data, including supported MDS v1 subchannel layouts. Extra subchannel bytes are not exposed as ISO data.
- Does not support `.isz`, `.daa`, `.mdx`, `.chd`, proprietary encryption, mixed audio extraction or all vendor MDF sector patterns. The bchunk and mdf2iso projects are useful test references; their GPL source is not included.

## Native FAT12 VFAT + WinImage IMZ preview

- Raw IMA/IMG/VFD/FLP superfloppy images need no MBR; native FAT12/16/32 browse/extract.
- Valid FAT-backed BIF images browse as filesystems. Non-FAT El Torito BIF exports
  the original bytes as `boot-image.bin` and remains read-only.
- FAT12 writes now support Unicode VFAT long names with strict UTF-8/UTF-16,
  DOS 8.3 alias allocation/collision checks and contiguous LFN slot allocation,
  including directory-chain extensions. Atomic staging is verified before commit.
- WinImage IMZ uses one ZIP stored or raw-DEFLATE member with CRC32, bounded
  in-memory decompression (32 MiB image max), no additional DLL or helper.
  ZIP64, multiple files, encrypted ZIP and non-FAT images are refused.

## v0.11.0 CloneCD CCD/IMG preview

- Open a `.ccd` descriptor paired with a same-base `.img` file. Parses CloneCD version 3 `[Disc]` / `[TRACK N]` fields, including MODE and INDEX 0/1. Validated Mode 1 and Mode 2 Form 1 data tracks use the existing sector reader.
- If multiple data tracks contain readable ISO/UDF filesystems, they appear under virtual `sessionN/` folders. The `.sub` sidecar and standard scrambled Mode 1/2 sectors are now supported in v0.12. Audio, CD-Text, copy-protection interpretation, multiple CCD sessions and other CloneCD versions are not supported.
- Rejects descriptors with out-of-order/overlapping tracks, invalid physical lengths, missing sidecars, unsafe symbolic links and unsupported encoding / sector modes.
- New synthetic tests cover physical-sector boundaries and per-track extraction, but manual validation using real CloneCD dumps remains necessary.

## v0.12.0 ISO 9660 and CloneCD

- ISO 9660 directory record byte 1 (Extended Attribute Record Length) is honored before reading the actual extent. Mirrored extent offsets/lengths and volume sequence bytes are validated in both endian encodings.
- CloneCD `DataTracksScrambled=1` uses the standard CD-ROM scrambling XOR specified by ECMA-130 Annex B (sector bytes 12–2351 only). No proprietary media-key decryption, scrambled audio, or nonstandard sector modes are claimed.
- CloneCD `.sub` data is exposed as a byte-for-byte *raw* P-W sidecar under `subchannels/disc.sub`. An additional `subchannels/q-index.csv` lists CRC-valid Q position frames grouped by track/index; no CD-Text or copy-protection metadata is interpreted. Missing `.sub` is permitted; truncated or unsafe `.sub` sidecars are rejected.
- Full CloneCD multisession, audio extraction, and vendor-specific protection methods remain unsupported. Experimental synthetic tests do not replace vendor-disc testing.

## v0.12.1 CloneCD Q CRC/index report

- `subchannels/q-index.csv` summarizes only CRC-verified Q ADR=1 position frames. Malformed Q frames do not invalidate the disc or erase raw `.sub` data.
- Handles normal CloneCD deinterleaved P12/Q12, with CRC-gated fallback for packed/interleaved P-W variants.
- No R-W CD-Text, Q ADR=2/3 EAN/ISRC or proprietary protection semantics in the earlier CloneCD update; the newer experimental CHD reader extracts standard CDDA WAV.
- The report scans in constant memory with a one-million-sector limit; new WCX artifacts require successful Windows CI before release.

## v0.14: integrated ISZ, DAA and limited CHD support

All new format decoders operate inside `ISO-WCX.wcx/.wcx64`, with no
separate ISO conversion, external parser DLL, executable, mounting driver or
runtime archive utility. Supported variants: unencrypted, single-file ISZ v1
with zero/stored/zlib/bzip2 blocks; unencrypted single-file DAA 1.00 with raw
DEFLATE and checked header CRC32; CHD v5 now decodes compressed maps and
hunks using the statically linked BSD-licensed libchdr. ISO/UDF are mapped
from 2048-byte DVD data or validated single-track Mode 1/2 Form 1 CD frames.
All filesystem parsing is in the existing WCX engine.

The bzip2 1.0.8 source is fetched with a pinned SHA-256 **at build time** and
compiled statically into the WCX. This does not add a runtime DLL.
bzip2/libbzip2 (C) 1996-2010 Julian R Seward. Redistribution and use are
permitted under its permissive license; see
https://sourceware.org/bzip2/downloads.html for official source and license.
The format logic itself is independently authored; no GPL image decoder
code was copied.

**Not supported in this historical v0.14 implementation:** ISZ passwords/encryption,
DAA 1.10 and multipart DAA, CHD audio/multitrack/parent chains, and MDS v2/MDX
metadata decryption. The current development preview now supports split ISZ,
CHD multitrack/CDDA WAV and MDS v2/MDX data reads in specific tested layouts;
unsupported proprietary variants still fail closed.
Authentic vendor samples and GUI testing are still required for stable.

## WIM/ESD in v0.15 Preview

The plugin can browse `boot.wim`, `install.wim`, and WIM-structured `.esd`
directly in Total Commander. WIM indexes appear as `image1/`, `image2/`,
etc. It uses system WIMGAPI from System32. No mount, additional DLL,
third-party EXE or conversion to ISO is required. The OS WIM API may store
temporary image metadata in the normal Windows temporary directory.
See [WIM architecture](docs/wim-integration.md).

Complete split SWM sets can be read and extracted from any part; all parts
must be present in the same directory. ESD and SWM remain read-only.

Existing plain ISO9660/Joliet and non-solid, unsplit standard WIM support
restricted add/replace/delete through the WCX API. New ISO identifier
lengths of up to 221 bytes and relaxed Joliet length up to 110 UTF-16 units
(including UTF-16 surrogate pairs) are supported in editing, although such
long-name layouts are not portable to every optical image reader.
Existing BIOS and EFI El Torito catalog/image sectors are preserved, and
edits to overlapping file extents are refused. ISO/UDF Bridge images have
one narrow write mode: equal-length replacement of an existing regular file
whose ISO and UDF entries share a single physical extent. Bridge bootable
images, addition/deletion, resize, embedded/sparse/hard-linked extents and
UDF metadata rebuilding remain unsupported. Separate UDF-only Type-1 images
permit bounded new file/directory creation and contiguous ShortAD parent
directory expansion, in addition to existing-file replacement/FID deletion; no
files/directories or free-space reclamation. Hybrid system-area ISO,
Rock Ridge and multi-extent rewrites are refused. WIM edits require
an NTFS staging workspace and elevated Windows backup/restore/security
privileges; new indexes, SWM edits and ESD writes are unsupported.

Changes rebuild and verify a staged image before replacing the original.
WCX file-by-file extraction does not restore NTFS alternate streams or
reparse points. Total Commander GUI editing has not yet been manually
validated; edit copies of important installation media.
