# Experimental WIM fast folder creation — mount-and-commit

**Status:** Development Preview, opt-in only. Not present in published
Stable v0.21.0. It must pass actual privileged Windows mount/commit,
cancel and cleanup testing before a release enables it by default.

## Why WIM mkdir was taking minutes

The old editor must WIMApplyImage() the whole selected index onto NTFS,
create the folder and WIMCaptureImage() it again. For a large Windows
install.wim this is slow even when a directory is empty.

## New in-process path

When the process environment includes **ISO_WCX_WIM_FAST_MOUNT=1**, and
the edit batch consists ONLY of up to 64 directory creations beneath
ONE existing imageN index, ISO-WCX tries the Windows WIMGAPI mount path:

1. Validate the WIM and take a read-only metadata listing. An existing
   directory is a no-op; an existing file conflicts.
2. Request administrator imaging privileges, NTFS staging and a fresh
   same-volume transaction workspace. Copy the original WIM there using
   CopyFileExW, preserving the input bytes. Copy progress and cancellation
   are reported through existing TC callbacks.
3. Open **only the cloned WIM** via WIMCreateFile with
   WIM_GENERIC_READ | WIM_GENERIC_WRITE | WIM_GENERIC_MOUNT, then
   WIMLoadImage(index) and WIMMountImageHandle with the optional FAST
   mount flag. The mount directory is a fresh empty NTFS directory in
   the sibling workspace.
4. Check for reparse points, make requested subdirectories, commit the
   mounted index with WIMCommitImageHandle, and unmount using
   WIMUnmountImageHandle. ESD/SWM/solid/read-only/delta WIM and index
   deletion are not eligible.
5. Close WIMGAPI handles. Independently reopen the cloned WIM using
   the existing in-process enumeration backend and compare **all**
   directory/file names and file attributes, timestamps and sizes.
   Only missing directory records explicitly requested by the edit
   plan may be new. Commit the staged file atomically only after
   verification of original size/mtime stability.

## Safeguards and limitations

- **No separate EXE, wimlib, DLL or helper process**. WIMGAPI is a system
  Windows component, but its *mount driver* and administrator rights may
  be unavailable on some machines.
- The staging copy still takes time proportional to WIM size. The
  improvement is avoiding full Apply/Capture for a folder-only edit.
- The fast mount **does not apply to file additions/replacements,
  deletion, multiple edited indexes or ESD/SWM**. Those remain on
  existing safe WIMGAPI Apply/Capture paths.
- Cancellation before original replacement cannot change the source.
  On unmount failure, the staged WIM and workspace are **retained**
  rather than recursively deleting a potentially active mount point.
  Recovery may require privileged Windows DISM mount cleanup.
- Failed mount does **not** silently retry Apply/Capture; old behaviour
  stays the default unless explicitly opted into mount mode.
- Tree equality is a metadata-level safety test, not proof that all
  unchanged stream bytes remain identical; real payload hashes, ACL,
  reparse, alternate streams, Windows mount-driver recovery and crash
  testing are prerequisites for enabling by default.
- This is NOT a complete custom WIM format metadata writer.
  The pure metadata preflight/decoder also remains read-only.

To disable the preview, remove the ISO_WCX_WIM_FAST_MOUNT environment
variable or set it to 0. The default is always the established
Apply/Capture path. Never test directly on the only copy of an ISO/WIM.
