# ISO-WCX v0.18.0 — Stable Core + Native Floppy VFAT/IMZ Preview

Native C++20 Total Commander WCX plugin for Windows Win32/x64. The downloadable
ZIP installs two self-contained WCX binaries with Total Commander plugin
registration and English/Russian documentation. **No external unpackers, extra
parser DLLs, virtual disk drivers, mounts, or helper EXEs are required.**

## Highlights since v0.17.0

- **IMA/IMG/VFD/FLP floppy browsing:** native FAT12/16/32 superfloppy detection
  when the filesystem boot sector starts at byte zero, without requiring
  an MBR/GPT partition table. Existing partitioned HDD IMG/RAW support remains.
- **Transactional FAT12 write Preview:** add/replace/delete files; create nested
  directories and delete directory trees. Uses DOS 8.3 aliases with collision
  checks and long VFAT names encoded as strict UTF-8/UTF-16 (including Cyrillic,
  Hebrew and supplementary Unicode characters). Handles long-name directory
  slots spanning FAT directory-cluster boundaries. Changes are staged and
  independently verified before atomic replacement. Invalid FAT mirrors,
  damaged allocation chains, unsupported geometries and unsafe paths are
  rejected; edit backup copies of valuable images.
- **WinImage IMZ reading:** integrated single-file ZIP Stored/Deflate decoder
  uses statically linked zlib. Verifies ZIP headers, CRC32 and size bounds;
  rejects encrypted archives, multi-member archives, ZIP64 and malformed data.
  FAT12/16/32 files inside valid IMZ are browsable and extractable without a
  temporary decompressed disk image. IMZ writing is **not supported**.
- **BIF boot images:** valid FAT-backed BIF extracts browse as filesystems;
  opaque BIOS/El Torito boot data is exposed byte-for-byte as
  `boot-image.bin` and remains read-only.
- **Installer associations:** IMA, VFD, FLP, BIF and IMZ are registered along
  with existing ISO/UDF, IMG/RAW, VHD/VHDX, WIM/ESD/SWM and optical formats.

## Quality gates

- Visual Studio 2022 **Win32 and x64** builds, WCX DLL loading/extraction and
  comprehensive CTest regression suite.
- Linux C++20 CTest, Clang ASan/UBSan checks.
- Independent mkudffs/udfinfo/7-Zip UDF interoperability, full >4-GiB UDF
  mutation with SHA256 readback and UDF staging parity/rollback tests.
- Strict IMA/IMG/BIF/IMZ regression fixtures: Unicode VFAT, 8.3 alias
  collisions, nested directories, valid and corrupt ZIP/CRC32, truncated
  metadata and rollback behavior.
- SHA256-checked ZIP packaging: only `ISO-WCX.wcx` and
  `ISO-WCX.wcx64` binaries plus text documentation and plugin metadata.

The release is only published by the automated workflow after all required
validation jobs succeed.

## Install

Download **ISO-WCX-v0.18.0.zip** from this release and open it directly in
Total Commander. Accept the `pluginst.inf` installation prompt. Keep both WCX
files together. To verify integrity, use the accompanying `SHA256SUMS.txt`.
See `INSTALL_EN.txt` or `INSTALL_RU.txt` for repairing old associations.

## Write safety and limitations

This is a Stable distribution of the verified core, **not** a declaration that
all filesystem writers are mature or fully feature-complete.

FAT12 writes require an independently validated standalone, exact-length raw
image (up to 32 MiB); unsupported FAT metadata and unsafe paths are rejected.
**FAT16/32, IMZ, opaque BIF, partitioned HDD filesystems, ESD and SWM remain
read-only.** Real-world floppy/boot-image compatibility still benefits from
manual validation beyond the synthetic CI corpus.

UDF Type-1 editing remains an experimental, bounded implementation for
compatible UDF-only physical 2048-byte partitions. It supports transactional
addition/deletion and allocation only for layouts validated by the writer.
UDF metadata/VAT/sparable partitions, arbitrary ISO/UDF Bridge rebuilding and
general bootable media rewriting are not supported. Existing WIM, CHD, MDX,
NTFS/ext and ISO/Joliet limitations are documented in README.md and PREVIEW.md.

This release does not modify or replace previous Stable tags or assets.
