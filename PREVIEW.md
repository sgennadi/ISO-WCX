# ISO-WCX v0.13.0 C++20 preview

Modern, read-only Total Commander WCX plugin. The codebase is clean-room and does not republish the legacy ISO.WCX 1.7.6 sources. Built with CMake and Visual Studio 2022 (Windows x86/x64).

## If Total Commander says `File iso.wcx not found!`

This indicates a **stale old association** such as `...\\plugins\\wcx\\ISO\\iso.wcx`; it does **not** indicate that `ISO-WCX.wcx` or its adjacent 64-bit `ISO-WCX.wcx64` is missing. The automatic installer cannot remove existing old plugin associations. In Configuration > Options > Packer > Configure packer extension WCXs, select `iso`, delete **only** the obsolete missing ISO.WCX entry, then click **New type...** and choose the installed `ISO-WCX.wcx`. Restart Total Commander and retry with Ctrl+PgDn. Do not type `192,` in the filename field or delete unrelated working plugins. See [INSTALL_RU.txt](INSTALL_RU.txt) and [INSTALL_EN.txt](INSTALL_EN.txt).

## Supported in the preview

- CUE/BIN track selection: opens `.cue` directly, parses multiple FILE/TRACK and INDEX 00/01 records, skips audio and unsupported track types, and chooses the last ISO/UDF-bearing data track. Supports 2048, 2336, Mode 1/2352 and Mode 2 Form 1/2352 layouts; extracts files through the same read-only WCX API.
- ISO9660 and Joliet, including Unicode names, ISO multi-extent/interleaved files, and primary-volume fallback for damaged Joliet.
- Rock Ridge RRIP NM/CE names, CL relocated directory references, PX non-writable/read-only mapping, TF modification timestamps and mirrored CE/PX number validation; symbolic links and POSIX special files are ignored rather than followed.
- 2048-byte logical images, raw 2352-byte CD Mode 1 and Mode 2 Form 1, and 2336-byte XA Form 1 frames (no Form 2).
- Some images with 150-sector lead-in (relative and absolute session LBA conventions), but **not full multisession TOC/CUE parsing**.
- UDF Type-1 physical partition mapping, File Entry/Extended File Entry, embedded/short/long allocation descriptors, AED continuations, OSTA CS0 UTF-16 names (surrogate pairs), and reserve VDS recovery.
- **Experimental** UDF Metadata, Virtual (VAT) and Sparable partition-map reading, including mirror metadata fallback and packet redirects.
- Read-only WCX operations (list/test/extract), streamed file data, 64-bit file sizes, bounded parsing and safety checks.

## Limits and cautions

- This is a **preview** tested primarily with deterministic synthetic image fixtures. Real DVD/BD/UDF compatibility is not yet guaranteed, especially rewritable/packet-written and damaged media.
- RAW Mode 2 Form 2, CD-Audio extraction, CD-Text and full CloneCD multisession are not supported. CloneCD raw `.sub` is preserved and position-type Q frames are CRC-checked; damaged Q data is not treated as an ISO failure. NRG and MDS select the latest recognized data track, but this is not full multisession filesystem merging.
- UDF VAT historical chains, every version of Metadata/Sparable, UDF pseudo-overwrite behavior and fragmented descriptor edge cases are not yet exhaustively covered.
- Rock Ridge PX read-only permissions and TF modification timestamps are implemented in the development branch, with CE mirrored-endian validation. Full POSIX permissions, symlink extraction and creation/access times are not exposed by the WCX API.
- Unsupported image containers remain ISZ, DAA, DMG and encrypted/compressed NRG/MDF variations. Indexed CSO/ZSO are supported. Only uncompressed NRG and MDS/MDF track subsets are implemented.
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

The plugin itself now opens partitioned raw HDD IMG/RAW, fixed VHD and clean fixed/dynamic base VHDX images through its own C++20 partition and FAT code. Read-only FAT12/16/32 files appear under partition1/, partition2/, etc. Extraction uses the same staging, rollback and progress controls as ISO/UDF. The installer registers IMG/RAW/VHD/VHDX; non-FAT partitions are skipped. VHD differencing/dynamic and VHDX parent/dirty-log images remain unsupported. No external programs, drivers or disk-parser DLLs are needed.

## MDS v2 / MDX inspection research (not an archive handler)

The developer-only `mds_descriptor_inspect` test utility detects MDS version 1,
packed/encrypted MDS major version 2, and MDX monolithic metadata envelopes,
with strict file-offset and overflow checks. It does **not** decrypt,
decompress or extract data from MDS v2 or MDX. A recognized MDX will
produce a specific error rather than being silently misread as ISO. The
installer does not associate .mdx. Read
[the Aaru issue #256 compatibility notes](docs/mds-v2-mdx-research.md).

## NRG and MDS/MDF preview support

- Nero `.nrg`: NERO (32-bit) and NER5 (64-bit) trailer, bounded chunk list, DAOI/DAOX (disc-at-once) and ETNF/ETN2 (track-at-once), selected latest ISO9660/UDF data track.
- Alcohol `.mds`/`.mdf`: validated MDS v1 session/track/filename/index blocks; uncompressed Mode 1, Mode 2 Form 1 and cooked 2048/2336 data tracks. `.mdf` is a sidecar, not a general Microsoft SQL MDF reader.
- Raw-image probing is still available for `.img`/`.bin`; installer associations include `.iso`, `.cue`, `.nrg`, `.mds` but **not** `.bin` or `.mdf`, avoiding conflicts.
- Synthetic NRG/MDS regression tests and malformed-metadata mutation tests are run by CTest on Linux and Windows. Real Nero/Alcohol images and actual Total Commander interaction still require manual compatibility testing.

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
- No R-W CD-Text, Q ADR=2/3 EAN/ISRC, proprietary protection semantics or CD-Audio in this update.
- The report scans in constant memory with a one-million-sector limit; new WCX artifacts require successful Windows CI before release.
