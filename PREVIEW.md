# ISO-WCX v0.21.0 Stable Core + Export Image Structure and bounded writer Preview

Modern Total Commander WCX plugin with a Stable installation channel and explicitly restricted ISO/WIM/UDF editing. The codebase is clean-room and does not republish the legacy ISO.WCX 1.7.6 sources. Built with CMake and Visual Studio 2026, MSVC C++26 draft (Windows Win32/x64).

The UDF Type-1 transactional writer was introduced in v0.17.0 and remains **experimental** in v0.21.0, with strict format preconditions and atomic rollback. Stable Core refers to verified package installation and bounded core capabilities, not universal filesystem writing.

v0.21.0 includes the read-only in-process **Export Image Structure**
button and internal WIM metadata validation groundwork. It does **not**
make creating a folder inside a large WIM instantaneous: WIMGAPI
Apply/Capture is still used. The hybrid-boot ISO/Joliet unlink experiment
in PR #65 is **not** part of this Stable build.

## AMI UEFI NTFS firmware-read workaround (Preview)

Some AMI UEFI firmware ships a broken NTFS file reader (version `0x10000`).
For a file split over multiple NTFS cluster runs, a short UEFI read ending
exactly on a run boundary can park the firmware's cursor at an unrelated
cluster. Later UEFI reads may return **wrong bytes** while Windows and Linux
correctly read the same unchanged file. See
[Pete Batard's analysis](https://github.com/pbatard/AmiNtfsBug).

**ISO-WCX mitigation:** On Windows, for each ordinary file extracted onto
NTFS, `Disc::copy` creates a new, random, same-directory
`.iso-wcx-*.part` file with Win32 `CREATE_NEW`. It sets the final
logical size using `SetFilePointerEx` followed by `SetEndOfFile`
**before the first streamed write**, then opens that exact file with
`std::ios::in|std::ios::out|std::ios::binary` (no `trunc`).
This requests early NTFS cluster allocation instead of repeatedly growing
the file across many small write chunks. After closing and validating the
file, the existing atomic sibling rename commits it to the destination.

The original file, ISO and WIM are unchanged during this process; the
temporary file is removed on cancellation, incomplete source reads,
malformed extents, full disk or write failure. All non-NTFS destinations
and preallocation-rejecting drivers use the previous normal streamed
writer, without installing any external DLL, EXE or filesystem driver.

This mitigation **does not guarantee physical contiguity**. If the target
NTFS USB volume is already severely fragmented, NTFS may still allocate
multiple extents. There is no change to AMI firmware, Windows' NTFS driver,
ISO/WIM editing or the boot configuration of the USB device. A successfully
extracted file can still trigger the firmware defect, so a real UEFI boot
test on affected hardware is required. This is also **not** an NTFS
defragmenter or a bootable USB creation feature.

Controls for diagnostics/troubleshooting:

- `ISO_WCX_NTFS_PREALLOCATE=0`: disable reservation; default is enabled on
  supported NTFS outputs, and all other filesystems retain legacy streaming.
- `ISO_WCX_NTFS_EXTENTS=1`: optionally query read-only
  `FSCTL_GET_RETRIEVAL_POINTERS` after a completed extraction and report
  the NTFS extent count to Windows `OutputDebugStringW`. No warning is
  shown for unrelated files, and query failure never fails an extraction.

CI includes Windows Win32 and x64 preallocation tests (full file length
before first write, non-truncating buffered output, zero-filled tail,
CREATE_NEW collision safety, opt-out and empty files) and the existing
`copy_transaction_rollback` test, which now checks the live size of a
cancelled `.part` stage. Linux extraction behavior is unchanged.

## WIM scratch cleanup after F7/F8 (protected Windows NTFS tree)

A WCX WIM edit can stage `<WIM folder>/.iso-wcx-edit-<random>-<random>/expanded`.
This is a FULL **WIMApplyImage extraction**, **not** a mounted WIM. DISM's
`/Get-MountedWimInfo` is expected to show nothing for this case. The
`expanded` tree may look like a normal Windows installation, with
`Windows`, `Users`, `Program Files`, read-only/system attributes, hard
links, ACL-protected directories and NTFS junctions.

**Preview fix:** the old `Cleanup` destructor discarded Windows deletion
errors without reporting the retained workspace. WIM output is captured
with original metadata; only *after* capture, ISO-WCX now removes the
ephemeral expanded tree through a link-aware, privilege-scoped Windows
cleanup routine. Source image ACLs and saved WIM data are never changed.
Separate edited indexes get separate staging trees to prevent stale
cross-index copies. The private workspace gets a final cleanup after the
WIMGAPI handles have closed, and a deferred retry on WCX `CloseArchive`
if another process temporarily holds a backup open.

Open Total Commander WIM panels retain WIMGAPI read handles. ISO-WCX
releases its own handles only at the final verified atomic-replacement step,
**after** all cancellation and source-identity checks. A cancelled F7/F8
therefore leaves the original WIM and its open read-only panel usable. After
successful replacement, the old panel listing is intentionally stale and must
be reopened before testing or extracting files; the plugin refuses to read
stale entries instead of mixing old filenames with new content. A real WCX
integration test exercises a retained panel, cancellation before recapture,
late cancellation after verification, and successful commit + `CloseArchive`.

Only private transaction folder names generated by ISO-WCX are eligible.
A `mounted` directory is checked against native WIMGAPI mount state;
nonempty/registered/uncertain mounts are preserved and **never** recursively
deleted. Any final cleanup failure displays its blocked path and Windows
error in interactive Total Commander. Do not manually use recursive
`takeown`, `icacls`, `rmdir` or `DISM /Cleanup-Mountpoints` on arbitrary
Windows-like staging trees without checking all junctions and ownership.
Existing workspaces left by **older processes** are **not** automatically
cleaned or scanned, to avoid deleting a recovery copy.

This is Preview-only until Win32/x64 CI and actual Total Commander Windows
image testing pass. Genuine 7-GiB `install.wim` still requires field validation.

## ONE universal pluginst.inf ZIP (both TC architectures)

**Project requirement (do not split):** GitHub Actions publishes a single
`ISO-WCX-Installable-Preview` artifact containing exactly
`ISO-WCX-preview.zip` for either 32-bit or 64-bit Total Commander.
No separate x86/x64 installer ZIPs, no EXE, CMD or PowerShell installer.

The ZIP contains `pluginst.inf`, `ISO-WCX.wcx` (Win32),
`ISO-WCX.wcx64` (x64), and documentation. A single
`pluginst.inf` entry `file=ISO-WCX.wcx` tells Total Commander to load
`ISO-WCX.wcx64` for its 64-bit edition and `ISO-WCX.wcx` for the
32-bit edition. Although both modules are shipped and extracted by the
stock INF installer, **only the correctly matching one is loaded**.

After unpacking the outer Actions artifact, open the inner installer ZIP
using **Enter** in Total Commander and accept the native `pluginst.inf`
installation. It advertises 23 supported image extensions. TC can prompt
per extension; the INF cannot force silent bulk registration, remove older
binaries, or conditionally extract only one architecture. Existing
ZIP/RAR/7z associations, toolbars, custom commands and unrelated plugins
must stay unchanged. CI enforces the single-ZIP requirement and both
native PE machine types.

To prevent sluggish normal ZIP opens, the Preview plugin no longer advertises
`PK_CAPS_BY_CONTENT`. The cached/native WCX capability mask changes from
Stable `222` to Preview `158`, and both `CanYouHandleThisFile(W)` and
`OpenArchive(W)` immediately refuse unrelated extensions. The updater also
does not start solely because Total Commander queries WCX capabilities.
ZIP remains handled by Total Commander. Native Win32/x64 smoke tests
check that repeated `GetPackerCaps`, `CanYouHandleThisFileW` and
`OpenArchiveW` calls bypass a 64-MiB ZIP-signature fixture without
probing its contents. This microbenchmark is not a substitute for a
real Total Commander ZIP timing comparison.

For WIM files at least **64 MiB**, the separate native opening indicator
appears only after **1.2 seconds**. It shows the current WIM index,
discovered metadata entries and elapsed time. When WCX finishes scanning
the WIM, it shows an exact percentage and entry count as TC receives
`ReadHeader` results. During extraction, TC displays byte progress;
during WIM edits, WIMGAPI Apply/Capture, rebuild and validation stages
report progress to TC. Cancel is cooperative: an already-running
synchronous WIMGAPI call can delay acknowledgement. Real 7 GiB
`install.wim` GUI validation remains pending.

## If Total Commander says `File iso.wcx not found!`

This indicates a **stale old association** such as `...\\plugins\\wcx\\ISO\\iso.wcx`; it does **not** indicate that `ISO-WCX.wcx` or the matching edition's `ISO-WCX.wcx64` is missing. The automatic installer cannot remove existing old plugin associations. In Configuration > Options > Packer > Configure packer extension WCXs, select `iso`, delete **only** the obsolete missing ISO.WCX entry, then click **New type...** and choose the installed `ISO-WCX.wcx` (Win32) or `ISO-WCX.wcx64` (x64). If the **Delete** button in that association dialog does nothing, run the optional repository helper `ISO-WCX-RemoveAssociation.ps1` with `-IniPath "C:\\path\\wincmd.ini" -Extension iso` to preview, then close TC and add `-Apply` to remove **only that ISO-WCX association** with a backup. For an obsolete `iso.wcx` use the separate `ISO-WCX-Repair.ps1`. These tools preserve user toolbar buttons, unrelated extensions and INI encoding. They are **not needed** to run the plugin. Restart Total Commander and retry with Ctrl+PgDn. Do not type capability masks (`222,` in old Stable, `158,` in Preview) in the DLL filename field. See [INSTALL_RU.txt](INSTALL_RU.txt) and [INSTALL_EN.txt](INSTALL_EN.txt).

## Supported in the preview

- CUE/BIN track selection: opens `.cue` directly, parses multiple FILE/TRACK and INDEX 00/01 records, skips audio and unsupported track types, and chooses the last ISO/UDF-bearing data track. Supports 2048, 2336, Mode 1/2352 and Mode 2 Form 1/2352 layouts; extracts files through the same read-only WCX API.
- ISO9660 and Joliet, including Unicode names, ISO multi-extent/interleaved files, and primary-volume fallback for damaged Joliet.
- Rock Ridge RRIP NM/CE names, CL relocated directory references, PX non-writable/read-only mapping, TF modification timestamps and mirrored CE/PX number validation; symbolic links and POSIX special files are ignored rather than followed.
- 2048-byte logical images, raw 2352-byte CD Mode 1 and Mode 2 Form 1, and 2336-byte XA Form 1 frames (no Form 2).
- Some images with 150-sector lead-in (relative and absolute session LBA conventions), but **not full multisession TOC/CUE parsing**.
- UDF Type-1 physical mapping, FE/EFE, embedded/short/long allocation, AED continuations, CS0 Unicode and reserve VDS. **Write Preview for validated UDF-only physical 2048-byte Type-1:** create multiple regular files and nested empty directories atomically; mix creation, bounded replacement and logical deletion; reallocate compatible parent directories; reclaim verified deleted FE/ShortAD blocks for the following transaction; safely reuse cleared deleted FID slots without increasing directory length; support bounded multi-ShortAD files beyond 4 GiB. Bitmap/LVID and CRC are verified. A sibling transactional stage uses native CoW cloning on supported filesystems or sparse copy fallback, never direct writes to the original. No VAT, Metadata, Sparable, AED chain creation or **general** ISO/UDF Bridge writer; a narrow, independently tested synchronized Bridge file-delete mode is documented below. See [UDF writer](docs/udf-type1-writer.md).
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

Visual Studio 2026 experimental C++26 x64:

    cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DISO_WCX_CXX_STANDARD=26
    cmake --build build --config Release
    ctest --test-dir build -C Release --output-on-failure

For 32-bit replace `-A x64` with `-A Win32`. Builds produce `ISO-WCX.wcx64` / `ISO-WCX.wcx`.

Linux CLI tests:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --parallel
    ctest --test-dir build --output-on-failure

CLI tool: `disc_inspect image.iso` or `disc_inspect image.iso --extract OUTPUT`.

## Installing in Total Commander

Download the **ISO-WCX-Installable-Preview** artifact from a successful
[Preview workflow](https://github.com/sgennadi/ISO-WCX/actions/workflows/wcx-preview.yml).
Unpack the GitHub artifact wrapper, then open the enclosed
`ISO-WCX-preview.zip` directly in Total Commander with **Enter**.
Confirm the `pluginst.inf` installation. Both native plugin binaries
are included; TC chooses Win32 WCX or x64 WCX64 itself.

### WIM and ISO editing in Preview

| Image | Supported modifications | Limits |
| --- | --- | --- |
| Standard unsplit WIM | Create/replace/delete files and directories within existing indexes; delete an image index except the last | WIMGAPI Apply/Capture can take many minutes and require NTFS staging, free disk and privileges. Fast Mount/Commit for creation of folders only is **experimental and off by default**. |
| Plain ISO9660/Joliet | Constrained addition, replacement, deletion and byte-exact no-op for already existing folders | Rock Ridge and incompatible layouts remain read-only; not a general-purpose ISO remaster. |\n| Hybrid boot ISO9660/Joliet (Preview) | Delete existing regular files by unlinking directory records only | Keep system-area MBR/GPT, El Torito sectors and payload bytes unchanged. No directory deletion, additions, resizing, Rock Ridge or UDF Bridge. |
| UDF-only Type-1 | Selected file and directory creations, file replacement and logical deletion | Experimental, validated physical Type-1 metadata and sufficient space only. |
| ISO/UDF Bridge | Narrow synchronized file replacement/deletion when both namespaces remain valid | No general directory creation or hybrid boot editing. |
| ESD / SWM | Read and extract only | Editing is not supported. |

The restricted **hybrid-boot ISO/Joliet file unlink code from PR #65**
is now integrated into this **combined Preview** (PR #68). It remains
experimental, requires real BIOS/UEFI boot testing and never securely
erases unlinked payload bytes. See [boot-preserving unlink constraints](docs/iso-hybrid-unlink.md).
Test only on disposable copies of installation images. Folder deletion from Total
Commander may involve special trailing wildcard tokens; the Preview WCX
normalizes those only at the terminal directory selection to avoid unsafe
file changes. Large WIM enumeration has an asynchronous native progress
dialog, but performance and cancellation still need real-host testing.

## Native HDD images inside WCX (v0.13 preview)

The plugin opens partitioned raw HDD IMG/RAW, fixed/dynamic VHD and clean fixed/dynamic base VHDX through native C++20 MBR/GPT and filesystem readers. Read-only FAT12/16/32, exFAT and experimental ext2/3/4 and NTFS subsets appear under partition1/, partition2/, etc. NTFS covers basic MFT and $I30 directory indexes, Unicode names and ordinary resident/fragmented/sparse uncompressed files, with deliberate feature refusals. Extraction uses the same staging, rollback and progress controls as ISO/UDF. Unsupported/unknown partitions are skipped. Dynamic VHD **base images** now have a native sparse BAT/bitmap reader; in v0.19.0 selected FAT12/16/32 partitions have bounded **transactional write Preview** (max 1 GiB logical x64 / 512 MiB Win32, sufficient disk staging space); differencing VHD parents, VHDX parent/dirty-log images, complex NTFS attribute-list/compression/encryption layouts and journal recovery are not supported. No external tools, drivers or disk-parser DLLs are needed.

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

## Fixed VHDX FAT12/16/32 write (included as bounded Preview in v0.20.0)

A bounded C++20 editor now targets only complete 512-byte-sector fixed VHDX
base images. The entire BAT is checked for unique fully allocated payload
blocks and for overlaps with metadata/log/optional regions. FAT changes
are staged through logical RAW plus a private physical clone; changed sectors
are patched and dual headers receive fresh write GUIDs, sequence and CRC32C.
A native independent readback of **every logical byte**, then atomic rename,
protects the original. Win32 limit: 512 MiB logical/physical. x64 limit:
1 GiB logical / 2 GiB physical. Dynamic/sparse/4K-sector VHDX, parent
snapshots, logs requiring replay and non-FAT editing remain read-only.
This bounded Preview **is included in v0.20.0 Stable Core** after the release CI gate; it was absent from Stable v0.19.0.
QEMU interop checks are test-only, not WCX dependencies.

## FAT12/16/32 transactional raw-volume writer Preview (v0.19.0)

- Shared FAT12/16/32 in-process writer with UTF-8/UTF-16 VFAT, checked DOS 8.3
  aliases, file replacement/addition/removal and recursive folder changes.
- FAT16 fixed root uses 16-bit chain values; FAT32 writes 28-bit chains while
  preserving upper reserved entry bits. FAT32 root clusters are addressable
  above 65535, with correct high-word FAT directory entry pointers.
- Mirrored FAT copies are checked before writes and updated consistently.
  FAT32 FSInfo and backup FSInfo are verified and receive exact free/next
  cluster counts; the backup boot sector must match the primary.
- The staged image is a demand-paged OS mapping of a *separate complete
  same-directory disk copy*, not a source-sized RAM vector. Image safety caps:
  **64 GiB on x64, 512 MiB on Win32**. Requires sufficient disk free space.
- Exact-size standalone FAT raw superfloppies and FAT `partitionN/` volumes
  inside validated MBR/GPT RAW/IMG and **fixed VHD** are writable. Only one
  partition may be edited per transaction. Original MBR/GPT, unrelated
  partitions and the fixed VHD footer are preserved byte-for-byte.
- Dynamic base VHD FAT partition writes are available only through a distinct
  full logical-image + physical-clone Preview (1 GiB x64 / 512 MiB Win32).
  Differencing VHD and all unsupported VHDX modes (sparse/dynamic/4K/parent), nonmirrored FAT32, damaged FAT,
  encrypted/compressed disks and oversized images remain read-only. Changes
  stage, sync, independently re-parse and commit atomically.
- Independent tests verify Unicode names, root and directory allocation,
  FAT mirrors, high-word cluster indexes, FSInfo bookkeeping and unchanged
  originals after failed edits. The v0.19.0 Stable distribution includes this bounded Preview.

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
two narrow Preview write modes: equal-length replacement of an existing
shared-extent regular file; and **synchronized logical deletion** of an
existing nonempty regular file with an identical single physical extent in
ISO/Joliet and UDF Type-1 *without* bitmap/LVID counters. The latter
updates both directory namespaces transactionally, recalculates UDF CRC,
and verifies both trees before commit. Bootable/hybrid, bitmap-managed,
directory deletion, general creation/resize, multi-run, sparse or linked
Bridge edits are refused. See [Bridge deletion safety](docs/iso-udf-bridge-deletion.md). Separate UDF-only Type-1 images
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
