# ISO-WCX v0.21.0 Stable Core + Export Image Structure and bounded writer Preview

Modern Total Commander WCX plugin with a Stable installation channel and explicitly restricted ISO/WIM/UDF editing. The codebase is clean-room and does not republish the legacy ISO.WCX 1.7.6 sources. Built with CMake and Visual Studio 2026, MSVC C++26 draft (Windows Win32/x64).

The UDF Type-1 transactional writer was introduced in v0.17.0 and remains **experimental** in v0.21.0, with strict format preconditions and atomic rollback. Stable Core refers to verified package installation and bounded core capabilities, not universal filesystem writing.

v0.21.0 includes the read-only in-process **Export Image Structure**
button and internal WIM metadata validation groundwork. It does **not**
make creating a folder inside a large WIM instantaneous: WIMGAPI
Apply/Capture is still used. The hybrid-boot ISO/Joliet unlink experiment
in PR #65 is **not** part of this Stable build.

For the full format-by-format safety matrix and next steps for bootable ISO/UDF images, see [native ISO editing strategy](docs/iso-edit-strategy.md).

## RC4 — UDF anchor parity with the reader (2026-11-13)

Fixed a namespace-integrity bug in hybrid ISO/UDF editing: the in-process
UDF reader accepts an independently validated **AVDP at sector 512** as
a fallback, but the ISO editor previously probed only 256, end-256 and
end-1 (plus declared-volume-end positions). A valid UDF filesystem without
an NSR02/NSR03 VRS and with only the sector-512 anchor could therefore be
mistaken for plain ISO9660 before an F5 edit.

RC4 now probes sector 512, protects its UDF descriptor sequence, and checks
all other standard and declared-volume-end anchors before permitting an
MBR-isohybrid payload update. No layout rebuild is allowed on an image
with an unreadable UDF filesystem.

Regressions independently cover:
- ISO/UDF with **only** a valid sector-512 anchor, no sector-256 anchor
  and no VRS, using native Windows WCX PackFilesW; the exact UDF extent
  changes while ISO, El Torito and MBR metadata remain byte-identical
- MBR-isohybrid variant of that image
- A malformed/incomplete UDF with a valid sector-512 AVDP that must
  **refuse** plain ISO additions without touching the source
- No orphaned `.iso-wcx-edit-*` temporary directories after F5/refusal

This change is not a stable release and still needs Windows Win32/x64 CI
and a disposable real-image boot test.

## RC4 — Bounded MBR-isohybrid ISO/UDF file replacement (2026-11-12)

The original ISO editor refused **every** nonzero optical system area
(first 32 KiB), including some MBR-isohybrid WinPE images. RC4 now accepts
a *validated conventional MBR system area* only for a **single-extent,
equal-length, UDF-addressed existing-file payload replacement** in an
ISO/UDF bridge. The edit copies the image to an isolated transaction,
changes only the validated UDF extent above the protected ISO boot/VRS
region, independently reopens ISO and UDF, verifies readback, then commits
atomically. Unchanged MBR/El Torito/ISO metadata are preserved.

Safety limits are intentional:
- Requires valid 0x55AA MBR signature, at least one bounded partition
  entry, valid boot flags and all partitions within the image.
- Protective MBR/GPT (0xEE or an EFI PART header) is **rejected**.
- Non-MBR/unknown system areas, plain ISO9660 hybrid metadata rebuilding,
  adding/deleting files, changing file lengths and moving partitions are
  **rejected**. No GPT, isohybrid remastering or image size change occurs.
- Real-host behavior and bootability of customer media are **not yet
  validated**; use disposable copies and confirm BIOS/UEFI boot yourself.
- Win32/x64 WCX tests cover the allowed F5 payload replacement, MBR and
  boot region byte preservation, blocked add/delete/GPT, and rollback.

This reduces needless F5 refusals without advertising universal hybrid ISO
editing. Stable v0.21.0 remains unchanged.

## RC4 — Configure: associations and Stable/Preview updates (2026-11-08)

**Configure ISO-WCX** includes **Check & Fix WCX Associations...**. It
previews changes and asks for confirmation. Then close every Total Commander
window; the plug-in starts a native worker via Windows `rundll32` that
waits for TC to exit and never modifies a loaded TC configuration.
The worker creates an atomic INI backup and applies only these safe rules:

- Missing `iso` becomes `159,ISO-WCX.wcx`.
- Existing ISO-WCX-owned `wim`, `iso` and `udf` use capability `159`.
- Existing ISO-WCX-owned read-only `esd` and `swm` use `192`.
- Other WCX plug-ins are preserved. Duplicates, redirects and unexpected
  INI changes abort safely; the x64 cache checksum is invalidated.
- The original ANSI/UTF-8/UTF-16 INI encoding and other rows are preserved.
- If .iso is still assigned to the separate legacy ISO\iso.wcx plugin,
  Configure offers a **second, explicit Yes/No consent** to replace only
  that legacy .iso entry. Choosing No keeps it untouched. Arbitrary
  third-party .iso handlers remain protected even when this option is used.
- No PowerShell script or auxiliary executable is required.

The Update checker now displays **Latest Stable** and **Latest Preview/RC**.
Users can choose `Stable` (recommended) or `Preview / RC` and opt out of
automatic checks. Stable downloads use the official GitHub Release ZIP and
SHA-256 checksum manifest. Preview queries completed successful Actions
builds with an unexpired combined x86/x64 installer and compares RC and build
numbers; the button opens GitHub for **manual** Preview ZIP installation.
Actions artifact download may require login. No unverified background RC
installation occurs. These are RC4 Preview changes, not Stable 0.21.0.

## RC4 — WIM scratch folder cleanup (2026-11-08, Issue #73)

Operator telemetry from RC3 Build 1288 confirms a successful WIM F5
`pack=0` and Delete `delete=0`: Total Commander now calls the WCX writer.
The new field problem is an orphaned `expanded` working directory after
a successful WIM edit, not a WCX dispatch failure.

RC4 closes WIMGAPI's loaded and captured image handles **before** cleanup
and explicitly deletes each `expanded` tree before committing the
verified WIM. On `ACCESS_DENIED` from applied Windows ACLs it grants
temporary deletion access to the active imaging identity and SYSTEM
**only for the isolated workspace entry**, without following reparse
points; it never changes ACLs of the original WIM or production files.
If cleanup still fails, the WIM edit does **not** report F5 success.
Cancel/failure cleanup is best-effort and must never overwrite the
original WIM. Windows WCX smoke tests assert no orphaned edit workspace
after PackFilesW, DeleteFilesW and cancellation.

The original WIMGAPI Apply/Capture and byte-by-byte changed-file
verification remain in place. The optional RC3 offline
`ISO-WCX-F5-Repair.ps1` is still included, because existing Total
Commander installations can retain stale WCX association masks.
No extra installer, runtime service, or helper EXE is needed.

**Verification gate:** Windows VS2026 Win32/x64 CTest, full GitHub
Actions, and real host testing on a disposable `boot.wim` or
`install.wim` including `expanded` cleanup. This preview is not
a Stable release.

## RC3 — WIM F5 host dispatch repair (2026-11-06)

The operator confirmed that RC2 `GetPackerCaps=222` and the live
`wincmd.ini` used `wim=222`, but **F5 did not enter `PackFilesW`**;
deleting a WIM folder succeeded. This is a TC host-dispatch symptom,
not proof that WIMGAPI failed. Separate real Total Commander 11.58.1
tests from PR #68 previously showed `158` rejected before the callback
and `159` successfully invoked the writer.

**RC3 restores `GetPackerCaps=159`**, specifically for a host-tested
existing-image F5 path, and deliberately removes `BY_CONTENT=64`.
This adds the `NEW=1` capability that Total Commander 11.58.1
used for dispatch, but `PackFilesW` still rejects creating an
actual new ISO/WIM archive. Real user-host confirmation is pending.

The *already cached* `wim=222` / `iso=222` / `udf=222`
registrations need to change to `159`, even after replacing the DLL.
The optional `ISO-WCX-F5-Repair.ps1` in the same ZIP:
1. Runs in **read-only dry-run mode** by default.
2. When explicitly run with `-Apply` after closing **all** TC
   windows, changes only these three ISO-WCX associations, preserves
   other WCXs and all INI encoding/line endings, creates a backup,
   and invalidates the derived `[PackerPlugins64]` checksum.
3. Refuses redirected or ambiguous INI layouts rather than guessing.
   If TC uses `/I=other.ini`, pass that INI via `-IniPath`.
4. A restart of Total Commander is required.

The repair is **not executed by pluginst.inf**; Total Commander does not
provide a safe documented post-install script hook for WCX. No EXE,
third-party DLL, background service or automatic INI mutation is added.
For an alternate manual solution, remove/re-add the existing WIM WCX
association after the updated DLLs are installed and TC is restarted.

Report the result of F5 using a disposable `boot.wim\\image1`,
then separately repeat for `install.wim` and ISO/UDF.
No production WIM should be edited until the issue is field-resolved.

## RC2: investigate F5/Delete regression after installation (2026-11-06)

The installed plugin advertises `GetPackerCaps=222` and the Total
Commander registration can legitimately contain `158`, `159` or
`222` for supported writable formats. **158 and 159 both allow
modification and deletion**, whereas the legacy optional test-helper
was incorrectly writing `192`, which has neither capability.
The test-only helper now corrects stale `192` registrations even
when the WCX path already matches, preserving valid `158`/`159`.

The RC2 production plugin has a **read-only Check WCX associations / F5**
button in Settings. It creates a local report at
`%LOCALAPPDATA%\\ISO-WCX\\Reports\\ISO-WCX-TC-Associations.txt`,
covering `wim`, `iso`, `udf`, `esd`, `swm`, effective INI
redirects, registered 32-bit WCX and companion 64-bit WCX paths, and
the derived `PackerPlugins64` extension cache. No INI editing occurs
while Total Commander is open; the installed package remains one ZIP
containing x86/x64 WCX libraries only.

See [registration diagnosis and exact masks](docs/tc-registration-audit.md).
A CI build and a real user WIM reproduction are required before claiming
the original F5 regression is resolved.

## RC build identity (current PR #69)

The **published Stable release remains v0.21.0**. Every Windows Preview
artifact produced by `.github/workflows/wcx-preview.yml` now identifies
itself as **v0.21.1 RC4 (Build N)**, where `N` is the unique GitHub
Actions workflow run number. The plugin Settings title, installed-version
label and Windows DLL Version Information share this identity, and the
auto-installable Total Commander `pluginst.inf` describes the same RC
and build. Its numeric `version=0.21.1` field preserves standard TC
installer compatibility. The ZIP is named
`ISO-WCX-v0.21.1-RC4-B<N>.zip` and includes **only** the two
architecture-specific WCX binaries and the normal installation notes.

CMake defaults to the Stable identity for local/release builds;
passing `-DISO_WCX_RC_NUMBER=4 -DISO_WCX_BUILD_NUMBER=<N>` produces an
RC-labelled build without editing Stable CMake/release metadata. Later
RCs increment the RC number deliberately, while repeated CI builds
of the same RC receive a different build number. The updater
continues checking official published Stable releases and also offers
Stable v0.21.1 to RC2 users when it is published.

## DISM GUI-derived WIM improvements (integrated into native WCX)

ISO-WCX reuses the **workflow ideas**, not the DISM GUI application or a
`dism.exe` child process. The plugin remains a self-contained WCX binary.

- **Get-WimInfo-style index summary:** explicit **Export Image Structure**
  now reads the per-index WIM XML through Windows WIMGAPI/MSXML and displays
  `imageN`, boot index, edition, description, architecture, Windows version
  and size metadata when those fields are available.
- **No surprise mount:** ordinary F5 WIM edits continue using isolated
  Apply/Edit/Capture stages, with proper cancellation, index-level status,
  original-file preservation and post-write verification.
- **One-index optimization:** single-index verification does not enumerate
  the untouched indexes again. A special staged-copy path avoids exporting
  other indexes when only the **last** `imageN` changes.
- No `DISM-GUI.exe`, `dism.exe`, external DLL, mounted WIM folder, or
  background service is required. Other image indexes remain unaffected
  unless explicitly selected.

Real multi-index `install.wim` performance and write tests on Windows
are mandatory before calling this Stable. See
[ISO editing strategy](docs/iso-edit-strategy.md) for optical write limits.

## Edit-path improvements under test (2026-11-04)

- **WIM (Windows, unsplit only):** when the only changes are within the
  final `imageN/` of a multi-index WIM, ISO-WCX now stages an exact copy
  of the original WIM, removes only its last index in that private copy,
  captures only the changed last index, and verifies its written files.
  Other indexes remain in place and are not re-exported or fully enumerated.
  This still requires one Apply/Capture of the changed image and storage
  for the staged WIM and extracted image. Editing an earlier index, changing
  several indexes, or deleting an index retains the original safer
  export-and-capture implementation so index ordering is not altered.
- **ISO:** images with matching ISO/Joliet volume descriptors and a bounded
  all-zero sector-aligned trailer may be safely rewritten. Nonzero trailers,
  hidden UDF anchors, hybrid boot/system areas and Rock Ridge remain blocked
  unless a separately validated writer handles the layout.
- **Diagnostics:** Windows edit failures are appended to
  `%LOCALAPPDATA%\\ISO-WCX\\Logs\\edit-errors.log`. This preserves the
  native rejection reason, instead of only showing a generic Total Commander
  error code. No file contents are logged.

These changes are preview-only until Windows/Total Commander tests pass.
Make a backup of important installation media before testing.

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

Download the **ISO-WCX-Installable-Preview** artifact from successful GitHub Actions runs. Unzip the outer GitHub artifact archive, then open `ISO-WCX-preview.zip` directly in Total Commander; `pluginst.inf` prompts installation. Alternatively configure `.iso`, `.img` or `.bin` manually under **Configuration > Options > Packer > Configure packer extension WCXs**. Only use `.bin` images in the supported single-track layouts above.

CI builds both Windows WCX architectures, runs parser and extraction tests, and packages only ISO-WCX.wcx, ISO-WCX.wcx64, pluginst.inf and human-readable instructions. No Setup.exe, third-party disk DLLs, PowerShell or CMD scripts are installed.

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
