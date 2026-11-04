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

### Field diagnosis: no second Associate dialog / no PackFilesW

The one native `defaultextension=wim` Associate prompt is expected.
If all 23 supported image extensions were already associated in earlier
previews and WIM flags are already `158`, the follow-up repair dialog is
**correctly suppressed**. A missing repair dialog can also mean that the
read-only profile discovery or registration inspection failed. Such failures
were previously swallowed when opening WIM; the next Preview records
`WCX association auto-check started`, `WCX association inspection`
(with counts for corrected, missing, duplicate, and foreign handlers),
`WCX association check complete`, any confirmation decision, and
`WCX association inspection failed` to the existing local TC log.

The read-only `tools/ISO-WCX-Diagnostic.ps1` now also reports
`WCX_ASSOC_SUMMARY owned_158=... owned_other=... missing=... foreign=...
duplicates=...` based on `[PackerPlugins]`. An older setup may
have zero `missing` entries, so no second dialog is required. Any
`WCX_ASSOC_BAD_MASK=wim` entry or a `WCX64_CACHE_MISSING=wim` warning
helps explain why TC might not dispatch an F5 edit callback.
A log consisting only of `OpenArchiveW entered/succeeded` does not
prove F5 was attempted; reproduce from a filesystem source panel into
a disposable WIM copy, with a small normal file selected as the source.

### TC F5 dispatched vs WCX rejected

The actual WIM F5 integration field report found that the WCX64 module
opened the image successfully and `[PackerPlugins]` contained `wim=158`;
the TC x64 `PackerPlugins64 exts` cache included `wim` too. Thus neither
a duplicate registration nor a missing MODIFY bit was demonstrated.

The interactive `edit-errors.log` now records the first live
`GetPackerCaps` call (`return=158`) and always logs the entrance of
`PackFilesW` or ANSI `PackFiles` before checking for null arguments.
A null archive/source returns `E_NOT_SUPPORTED(24)` **with an explicit
rejection event**, rather than returning 24 silently. No changes are
written to images or TC configuration by these diagnostics.

If `OpenArchiveW succeeded` is present but **no** `PackFilesW entered`
or `PackFiles ANSI entered` appears after a real F5, TC did not call
either WCX packing callback. Confirm that the source panel displays an
ordinary filesystem file, not a ZIP or other archive, and that the WIM
destination panel is `boot.wim\\image1\\` (inside the image index,
not the synthetic WIM root listing only `image1`). The user's screenshot
showing the synthetic WIM root is insufficient evidence of an attempted
copy inside index 1. Do not add `PK_CAPS_NEW` to work around this without
a format-specific new-archive implementation.

### Read-only F5 panel routing check (Total Commander 11.57+)

The user has verified `wim=158`, `PackerPlugins64 exts` contains WIM,
and `OpenArchiveW succeeded` runs with no `PackFilesW` callback.
An earlier screenshot showed only `[image1]` in the right panel,
but a later **read-only TC 11.58.1 live-panel query conclusively verified**
`TC_WIM_DESTINATION_DEPTH=inside-image-index` and the path
`boot.wim\\image1\\*.*` during the same reported F5 failure.
The standard file-to-WIM F5 operation must target `boot.wim\\image1\\`.
**Do not blame the user's panel selection or keep asking for the same
panel check after this verification.**
The source must be a selected file in an ordinary filesystem panel
(not from another archive), and **that source panel** must be active.

The existing `tools/ISO-WCX-Diagnostic.ps1` now queries Total Commander
11.57+ read-only window messages `WM_USER+50` via
`SendMessageTimeoutW`. It reports `TC_PANEL_SNAPSHOT` with active
panel, file/dir selection counts, filesystem/WCX plugin type and
right-side archive presence. The query never posts a command, edits
Total Commander, or changes the image. If the querying process is
not permitted to message an elevated Total Commander (UIPI), it
reports the failure rather than escalating or attempting bypass.
The later diagnostic reads the path-bar control captions using
non-mutating GetWindowTextW and identifies the WIM destination index
when a caption is available. Run the diagnostic at the same elevation
as TC immediately after F5. If the destination is confirmed inside
`image1` and no PackFiles callback appears, investigate the WCX host
dispatch or cached capabilities rather than repeating panel tests.

### Bidirectional F5 routing and live destination path

Total Commander 11.58.1 field reports verified both valid panel layouts:
filesystem LEFT to WCX RIGHT, and filesystem RIGHT to WCX LEFT.
The first version of the diagnostic incorrectly rejected the second,
which is now fixed in the read-only Get-TcF5Context helper.
Windows PowerShell CI covers both directions, archive-to-filesystem,
and zero-source-selection conditions.

The diagnostic also attempts to read the documented panel path control
handles 9/10 using GetWindowTextW. Where a visible path caption is
available, it reports whether the WIM destination lies inside image1
or at the synthetic root. Custom-drawn path labels can be inaccessible;
the script then reports UNKNOWN rather than inferring the path from
WCX archive presence. No commands are posted to TC.

### PowerShell relative ReportPath resolution fix

The user submitted -ReportPath ".\ISO-WCX-F5.txt" from
Downloads\Mount; an older diagnostic used .NET GetFullPath against the
underlying process working directory and silently wrote to $HOME instead.
The script now explicitly resolves non-rooted report paths against
PowerShell Get-Location.ProviderPath before normalizing the absolute
path. Existing absolute and UNC paths are unchanged. A Windows CI
regression runs the script after Push-Location and checks that the report
was actually created in that PowerShell folder.

### Do not use cm_CopyToArchive as a WIM F5 test

**Confirmed in a 2026-11-01 TC 11.58.1 field screenshot:** typing
`cm_CopyToArchive` opened the normal **Pack files (Alt+F5)** dialog with
**ZIP** selected and an output of `diags.zip`. The next overwrite prompt
was for `diags.txt` inside an existing ZIP, **not** for a file inside
`boot.wim`. The test was invalid for WIM and should be cancelled
instead of allowing the ZIP overwrite.

The standard F5 action is `cm_Copy`, **not** `cm_CopyToArchive`.
`cm_CopyToArchive` and `cm_PackFiles` always start the separate pack
dialog for a new archive; neither adds files to an already open WIM
in the opposite panel. Do not suggest either as a way to bypass
`Function not supported!`.

The user's ordinary F5 was already reproduced with source
`filesystem`, one source file selected, WIM destination `image1`,
`wim=158`, TC64 cache containing `wim`, and ISO-WCX.wcx64 loaded.
The root cause remains open until a real TC-hosted PackFiles callback
(or a specific host refusal mechanism) is identified. Do not change
masks to `159`, alter third-party associations, or create an output ZIP
to disguise the missing WIM edit.

### Diagnose deployed WCX64 exports before host routing

The user's real Total Commander test verifies:
- the source is an active filesystem panel with a selected ordinary file;
- the destination is a WCX panel whose path is inside boot.wim\image1\;
- all 23 image associations use 158, and the TC64 cache includes wim;
- the live WIM opened successfully, but F5 reports "Function not supported!"

The diagnostic now checks the **deployed binary's PE export table** using
LoadLibraryExW with DONT_RESOLVE_DLL_REFERENCES and GetProcAddress.
This does not execute plugin code, start any WIM action, or alter TC:
it reports WCX_EXPORT_PRESENT/MISSING for PackFilesW, PackFiles,
GetPackerCaps, DeleteFilesW and the core Unicode entrypoints.
A missing PackFilesW is a concrete ABI failure; a complete export set
means the refusal occurs somewhere else, not that F5 has been fixed.
A separate native CTest DLL-loading smoke now asserts actual PackFilesW
exports and invokes GetPackerCaps to verify its return value is 158.
Run the read-only field script after installing the latest Preview; it
automatically checks the .wcx64 path if PowerShell is 64-bit, or set
-WcxPath explicitly. Do not execute the WCX pack exports in diagnostic
code or promote Stable before a real hosted F5 succeeds.

### WIMGAPI read-side sharing fix for TC F5 preflight (Preview)

A concrete Windows API compatibility issue was found in the WCX WIM
reader: WIMCreateFile(..., GENERIC_READ, OPEN_EXISTING, 0, ...) omits
WIM_FLAG_SHARE_WRITE (0x40). Windows can then refuse a SECOND
GENERIC_WRITE open of the physical WIM while Total Commander still has
its archive panel open. TC may perform a writable-destination preflight
before dispatching PackFilesW, so this is a **plausible**, not yet
field-proven, explanation for its "Function not supported!" error.

The read-only WIMGAPI opener now specifies WIM_FLAG_SHARE_WRITE while
still using GENERIC_READ. This allows the host's concurrent write probe
without granting the WCX reader write permissions. The existing
commit/replace transaction still releases active reader handles, holds
per-archive locks, and invalidates stale open-panel WIM streams before
replacing the archive.

The real Windows WIM CTest opens a genuine disposable WIM through the
WCX OpenArchiveW entry point, then tests CreateFileW(GENERIC_WRITE) on
the same WIM while keeping that archive panel open. No bytes are written
by the probe. This assertion must pass on both Win32 and x64 before
the change can be tested in a real TC instance. Do NOT promote Stable
until the user's F5 trial confirms PackFilesW is reached.
