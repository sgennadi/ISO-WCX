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
