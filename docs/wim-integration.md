# Integrated WIM, ESD and SWM reader (v0.15 Preview)

**One WCX binary, no external helpers.** On Windows the plugin loads the
system component `wimgapi.dll` from System32. Each Windows image index is
represented under `image1/`, `image2/` and so on. WIMCreateFile,
WIMSetTemporaryPath, WIMLoadImage, WIMFindFirstImageFile,
WIMFindNextImageFile, WIMCreateImageFile, WIMReadImageFile and WIMCloseHandle
are called in-process.

The plugin does not mount or modify WIM files. The Windows library may
stage metadata in the OS temporary directory internally. Individual file
content is streamed to ISO-WCX's existing transactional extraction stage,
which supports cancellation without damaging existing output.

Supported: single-file standard WIM and compatible ESD compression that
the host Windows WIMGAPI can decode, plus complete split SWM sets. Not supported:
independent WIM browsing on non-Windows OS, alternate data streams,
reparse point recreation, extraction of NTFS security descriptors and
unusual vendor-encrypted containers.

WIM filenames are checked for Unicode safety, path traversal, Windows
reserved device names and dangerous separators. Reparse entries are
skipped. Listing size and nesting depth are bounded.

The project reviewed https://github.com/ebiggers/wimlib .
wimlib supports XPRESS, LZX and LZMS and has LGPL/GPL licensing; its
documented Windows build uses MinGW rather than MSVC. We do not import
its source or ship libwim-15.dll/wimlib-imagex.exe: the built-in Windows
WIMGAPI satisfies the portable Windows WCX runtime requirement without
extra redistributables.

The Windows CI test `wcx_wim_smoke.cpp` creates a small two-index LZX WIM
with directory entries, opens it through 32/64-bit WCX exports and verifies
selected files byte-for-byte. It checks WIM-compatible ESD naming and
malformed WIM rejection. It also splits a WIM with WIMSplitFile, verifies
files from multiple parts byte-for-byte, opens later/renamed parts, and rejects
missing and duplicate parts. Test binaries and generated images are not shipped.


SWM discovery scans only the selected file's directory (at most 10,000 entries,
4,096 parts), matching the WIM GUID and part numbers, not filename suffixes.
All parts must be present as regular non-symlink `.swm` files. The first part
is opened and exact paths to other parts are registered with WIMSetReferenceFile
(WIM_REFERENCE_APPEND) before WIMLoadImage. No joining or mounting is performed.
The cross-platform wim_split_parts test validates malformed set handling.
