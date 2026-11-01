# Native WIM / ESD / SWM reader and editor (v0.15 Preview)

**One ISO-WCX binary; no helper EXE or separately distributed DLL.**
On Windows, ISO-WCX loads the operating-system `wimgapi.dll` from
System32 in-process. WIM images appear as `image1/`, `image2/` and so
on. Compatible ESD and complete SWM sets are browsable too.

## Reading and extracting

Native WIMGAPI enumerates each index and streams regular files into the
existing transactional WCX extraction stage. The host OS supplies the
available XPRESS/LZX/LZMS decoders; unsupported ESD compression is rejected.
The OS may use the Windows temporary directory for internal WIM metadata.

For split SWM, open any member with all parts in the same directory.
The resolver checks WIM GUID and part number, scans at most 10,000 local
entries and accepts at most 4,096 parts. Missing, duplicate, incomplete
or inconsistent sets are rejected. Exact filenames are registered using
WIMSetReferenceFile; no joining or mounting occurs.

WCX per-file extraction skips unsafe reparse entries and does not restore
NTFS alternate streams, security descriptors or reparse-point targets.

## Restricted transactional WIM editing

Only existing standard, unsplit, non-solid and writable WIM images may be
modified. Under `imageN/`, WCX PackFiles/DeleteFiles can add, replace or
remove files and directories. Deleting entire indexes is supported except
for removal of the last index. Creating a new image index is unsupported.
ESD and split SWM remain read-only, as do solid, delta and other excluded
WIM layouts.

Editing requires the Windows backup, restore and security privileges and
an NTFS staging volume. ISO-WCX enables necessary privileges on a temporary
thread impersonation token rather than changing the process token.
Affected images are applied into a sibling workspace using WIMGAPI,
edited there and recaptured; unaffected indexes are exported directly.
Image descriptive XML and boot-index selection are preserved where
supported. WIM creation and folder edits are **not constant-time**: the entire affected
index must still be extracted and recaptured. A large install.wim can
legitimately require several minutes even for one empty folder. During
Apply/Capture, WIMGAPI percent updates are mapped to Total Commander
WCX's progress bar (with the current image/stage label and cancellation).
Unchanged indexes are exported rather than reapplied.

The staged WIM is reopened; all image-index metadata and edited directory
entries are checked, and **changed files** are read back byte-for-byte.
Unchanged compressed file contents are not redundantly decompressed and
re-read after an already-expensive Apply/Capture. Deletions, unsafe paths,
empty folders and cancellation are covered by WCX regression tests.
The original image is only replaced after the checks and flush succeed.
A cancelled or rejected edit does not modify the original WIM; temporary
disk usage can be substantial. This is still a Preview implementation.

## Windows 2025 WIMGAPI output compatibility

Native x86 and x64 interop checks on Windows Server 2025 detected a
16-byte extension written by `WIMFindFirstImageFile` beyond the public
`WIM_FIND_DATA` layout. Without reserved storage, a Win32 caller could
experience heap corruption. ISO-WCX reserves **exactly 16 additional bytes**
after the standard Windows ABI fields and keeps a separate canary-checked
boundary around every enumeration buffer. Both the initial and subsequent
`WIMFindNextImageFile` results are checked. The extension is not interpreted
as untrusted filesystem metadata; the plugin still validates names, paths,
sizes, and entries before exposing files to Total Commander.

This compatibility buffer changes neither the WIM format nor the installed
WCX ABI, and adds no external runtime dependency.

## Dependencies and validation

No DISM or wimlib-imagex executable, added libwim DLL, mounted image,
driver or helper process is needed. The project reviewed
https://github.com/ebiggers/wimlib but did not copy or distribute its
GPL/LGPL source.

Windows x86/x64 WCX smoke tests create a multi-index LZX WIM, verify
byte-for-byte extraction, check WIM-compatible ESD, split a real multi-part
SWM, exercise renamed/missing/duplicate parts, and test WIM editing and
index deletion through WCX exports. Automated fixtures are not equivalent
to manual Total Commander GUI validation or a broad corpus of real
installation images: **this feature remains Preview.**

## Diagnose a missing boot.wim image index, disabled TC F5 or retained workspace

A read-only PowerShell 5.1/7 diagnostic is provided as
[`tools/ISO-WCX-Diagnostic.ps1`](../tools/ISO-WCX-Diagnostic.ps1).
Run on the affected Windows computer after closing and reopening every
Total Commander instance when replacing the WCX plugin:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\ISO-WCX-Diagnostic.ps1 -WimPath "D:\sources\boot.wim" -CheckDism -ReportPath "$env:TEMP\ISO-WCX-Diagnostic.txt"
```

The script compares the raw WIM header image count against read-only DISM
output; prints the active TC packer associations and WCX capability flags
(158 is expected for F5); reports the ISO-WCX module loaded by each
running Total Commander process; and inventories only strictly named
`.iso-wcx-edit-*` directories beside that exact WIM. It does **not**
change the INI, edit the WIM, unload a mount, or delete any workspace.
An empty `dism /Get-MountedWimInfo` cannot prove that a native WIMGAPI
mount is absent. A retained `mounted` directory or
`original-archive.backup` must be preserved until native mount registration,
source-WIM identity and handle ownership have been checked. Retried cleanup
from a prior TC process is not persisted across restarts.

If the on-disk WIM declares two images, the WCX should show both
`image1` and `image2`; a disagreement between WIMGAPI's image count
and the on-disk header now fails explicitly instead of silently listing
fewer image roots. This does not assume every boot.wim contains two images.

### Interactive F5/F7 failures and retained WIM cleanup diagnostics

The current Preview records local, bounded, best-effort error and operation
events only when invoked inside Total Commander. The log is stored at
`%LOCALAPPDATA%\\ISO-WCX\\Logs\\edit-errors.log`; it is never uploaded
or sent through the network. An entry named `PackFilesW entered` means the
TC host actually called the Unicode WCX edit callback. If pressing F5 does
not produce this event after reopening TC, inspect TC's WCX association and
capability cache rather than assuming the WIM writer failed. Nonzero
`PackFilesW finished` results and corresponding edit error messages help
locate a rejected path or NTFS/Windows API error. Cleanup failures include
the exact blocked entry and Win32 error; successful deferred retries are
also recorded. No CTest/CLI host writes this interactive log.

Run the read-only `tools/ISO-WCX-Diagnostic.ps1` and send its report after
reproducing the failed F5 to capture the recent local entries. Even when a
workspace contains only `expanded` and no `mounted` or
`original-archive.backup`, do not blindly recurse-delete it: Windows
Apply/Capture may restore protected ACLs and hardlinks. A TC restart cannot
recover a process-local deferred cleanup queue from the previous process.

### Explicit TC x64 cache repair when F5 is unavailable despite flags 158

Use **Packers > ISO-WCX > Configure > Repair WCX associations**.
For a correctly registered `iso/wim=158,...ISO-WCX.wcx`, an explicit
repair now offers to clear the checksum in TC's `PackerPlugins64` cache.
This is opt-in, backs up the affected INI, and leaves third-party packer
associations and toolbar buttons unchanged. The previous implementation
returned `Nothing changed` when `PackerPlugins` flags were already
correct, even if the separate x64 cache was stale. Auto-detection while
opening a WIM still does NOT prompt for a cache reset on every TC launch.
After confirming, **close every Total Commander process** and start TC
again before testing F5. This addresses one plausible cache issue but
does not prove it caused the user's F5 failure; inspect the new local
`edit-errors.log` from the subsequent F5 attempt.

### TC 11.x F5 "Function not supported" diagnosis (Preview)

A failed F5 in an already-open WIM panel does not by itself prove the
WIM writer was called. The optional interactive log at
`%LOCALAPPDATA%\\ISO-WCX\\Logs\\edit-errors.log` now records
`OpenArchiveW entered` and `OpenArchiveW succeeded` when TC opens WIM,
ISO or UDF, as well as existing `PackFilesW entered` and
`PackFilesW finished` entries. If the WIM open is present and no new
PackFiles event appears after F5, inspect TC's x64 associations and
cache rather than changing the WIM bytes. The read-only script
`tools/ISO-WCX-Diagnostic.ps1` checks whether `wim` occurs in
`[PackerPlugins64] exts` and whether a checksum is present.
`ConfigurePacker -> Repair WCX associations` performs a confirmed,
backup-protected cache invalidation when needed; restarting all TC
windows is essential. An F5 return code 24 means E_NOT_SUPPORTED and
the preceding `F5/F7 edit error` explains any intentional WCX refusal.
Test first with an ordinary small text file on a disposable WIM copy,
not with symlinks, protected paths or an entire AppData tree.
