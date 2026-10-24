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
supported. The staged WIM is reopened and visible regular files are
verified before the original archive path is replaced. Cancellation,
rejected input or processing failure before final commit does not change
the original archive. Space usage can be substantial.

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
