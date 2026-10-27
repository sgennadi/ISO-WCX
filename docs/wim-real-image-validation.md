# Real-image WIM fast-folder validation (developer only)

This is an explicit, **manual, privileged Preview test** for a genuine
multi-gigabyte install.wim through the actual ISO-WCX WCX ABI. It is not a
release feature, does not download a Windows image, does not run automatically
in CI, and is never bundled in the Total Commander ZIP.

## Build with Visual Studio 2026

Use an elevated Developer PowerShell for the native WIMGAPI mount driver:

    cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DISO_WCX_CXX_STANDARD=26 -DBUILD_TESTING=ON
    cmake --build build --config Release --target ISO-WCX wim_real_image_validate

With a genuine local WIM on writable NTFS (do not use a system-mounted
or production Windows installation image), run:

    .\build\Release\wim_real_image_validate.exe "D:\Images\install.wim" ".\build\Release\ISO-WCX.wcx64" 1

For a much slower, exhaustive data-stream audit, append `--full`:

    .\build\Release\wim_real_image_validate.exe "D:\Images\install.wim" ".\build\Release\ISO-WCX.wcx64" 1 --full

The default command hashes only up to four nonempty regular file streams
of at most 64 MiB from each WIM image index. The `--full` option hashes
**every listed nonempty regular file stream** in every image index both
before and after the edit, without extracting all files to disk. On large
install.wim files this can take much longer and read many gigabytes.
The audit covers the default file streams exposed by WIMGAPI; it does not
by itself verify every alternate NTFS stream, hardlink relationship, or
a boot test. The separate privileged Windows integrity test covers those
metadata features on its own disposable two-index WIM fixture.

For Win32, configure a separate -A Win32 build, use the matching Win32
wim_real_image_validate.exe and the ISO-WCX.wcx DLL.

**Disk space**: put the source on a local NTFS volume with available space
of at least **2x the input WIM size** plus the larger of 512 MiB or 10% of
input size. The helper itself creates one disposable full copy; ISO-WCX
creates a second private copy for native mount/commit. Windows may require
further mount scratch space or deny mounting on unsupported systems.

**Safety**: the original path is never passed to WCX PackFilesW and is
never committed. The tool hashes the original whole WIM with SHA-256,
makes and verifies a complete private clone, then performs an opt-in
folder-only mount/commit on the clone. It validates:

- All original paths, file sizes, attributes, timestamps and image indexes
  remain present (parent directory timestamps may change).
- Exactly the requested new directory appears in the chosen image index.
- By default, several small/medium regular file streams from **every
  populated index** are independently read through WIMGAPI and checked
  with SHA-256 before and after editing, including exact byte lengths.
  With `--full`, **every listed nonempty regular file stream** is verified.
- The source size, write time, and full-file SHA-256 are unchanged.

The default sample-stream check is **not** a full hash of every decompressed file.
The separate Windows CI test covers actual WIMGAPI Mount/Commit of a
two-index LZX WIM with protected ACL, named ADS, file hardlink identity and
incompressible payloads. These tests complement each other, but neither
proves crash recovery or performance for arbitrary install.wim files.

The helper prints a PASS line with image index, number of original WIM
entries, number of sampled resources and elapsed wall-clock time.

## On failure

It prints the private test workspace path and retains it. **Never delete
a workspace while Windows still reports an active mount.** Follow the
private WIM-MOUNT-RECOVERY.txt, if present, and inspect
dism /Get-MountedImageInfo to identify the exact mount and staged image.
Do not run global mount cleanup; do not assume a failed mount is detached.
The developer tool is not designed to automatically restore arbitrary
driver failures or an OS crash.

## Release criterion

Passing CI plus this manual test on multiple disposable genuine
install.wim images is necessary but not sufficient for enabling the
fast editor by default. Include tests on real hardware for mount-driver
unavailability, NTFS reparse points, non-ASCII/case conflicts, ACL/ADS
and WIM index parity, insufficient disk space, cancellation and power
loss recovery. Keep FastMountFolders disabled by default until reviewed.
