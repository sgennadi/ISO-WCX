# ISO Master and TotalImage feature review (2026-11-14)

These are upstream reference projects, not imported WCX libraries.

| Reference | License | Reuse decision |
| --- | --- | --- |
| ISO Master / bkisofs | GPLv2 | Feature/specification study only; no GPL code in the plugin |
| TotalImage | MIT; copyright (c) 2020-2026 TotalImage Team | Cross-check public layouts; independently authored C++ reader, no C# dependency |

## Already present in ISO-WCX

* ISO9660/Joliet read and bounded transactional native write,
  El Torito catalog/BIOS/EFI extraction and protected boot ranges
* Rock Ridge NM/CE/CL/PX/TF read-only parsing
* Nero NRG, Alcohol MDF/MDS, IMZ, FAT12/16/32, VHD/VHDX, UDF
* Staged changes with readback, progress, cancellation, rollback and
  atomic replacement. TotalImage's transaction idea is already covered
  by our stronger transaction and protected-extent verification.

## Implemented from independent format specifications

* Anex86 FDI/HDI: a 32-byte little-endian header with type, total
  header size, data length and sector/track/head/cylinder geometry,
  usually followed by a 4096-byte padded header. **No unique magic**.
  Reject malformed or truncated geometry; never fall back to treating
  .fdi/.hdi as a raw disk after a validation failure.
* T98-Next NHD: `T98HDDIMAGE.R0\0` signature, 512-byte header,
  geometry at documented offsets, exact bounded data span.
* Data is exposed as logical sectors through the existing native
  VirtualDiskReader, with no mounting and no external converter.
* .fdi: validated FAT superfloppy read/extract only. A 1024-byte
  FAT sector size is accepted only when the BPB agrees.
* .hdi/.nhd: supported **MBR/GPT** and native volume parsers only.
  The PC-98 native CHS partition table is not yet supported.
* Native writing is intentionally disabled for these three formats
  until the FAT mapper can preserve and verify their external headers.

Format references: TotalImage's MIT-licensed `TotalImage.IO/Containers`
classes and the independent public descriptions
https://www.pc98.org/project/doc/hdi.html and
https://www.pc98.org/project/doc/nhd.html.

## Next improvements, in priority order

1. Finish host-level F5/Delete confirmation in WIM and ISO/UDF Bridge,
   and complete safe empty-directory UDF deletion in PR #84
2. For Rock Ridge write support, independently model SUSP/RRIP
   PX/NM/TF/CE/CL and directory relocations before allowing a rewrite
3. Add a PC-98 native partition map (sector/head/cylinder with bounds
   and overlap checks) for real HDI/NHD files
4. Add experimental 1024-byte PC-98 FAT12/16 images with independently
   mastered test fixtures and cross-platform extraction validation
5. Only afterwards consider obscure historical disk geometries or
   extra image containers

## Release constraints

* Both new readers are read-only; do not advertise F5/Delete.
* Do not modify stable DLLs before Linux/MSVC Win32/x64 tests pass.
* A generated FAT fixture passing does not prove interoperability
  with real 86Box, T98-Next or physical PC-98 disks.
* Single combined installation ZIP for TC x86/x64, no EXE or extra DLL.
