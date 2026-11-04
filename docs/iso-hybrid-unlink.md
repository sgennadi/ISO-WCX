# Preview: boot-preserving ISO9660/Joliet file deletion

This experimental in-process WCX code allows deletion of **existing regular
file directory entries only** from supported ISO9660/Joliet optical images
with a nonzero first 32 KiB boot/system area. This can include isohybrid
bootable images such as Parted Magic. It is not general ISO remastering.

The operation keeps the complete ISO image byte size unchanged. It updates
only the sectors that contain the exact deleted file records, in BOTH
ISO9660 and Joliet when the latter exists. No file payload, volume
descriptor, path table, MBR/GPT system area, El Torito catalog, boot
image sector, or EFI payload is intentionally overwritten. Deleted file
data remains as orphaned bytes and **is not securely erased**.

Strict gates: no UDF bridge, no Rock Ridge/SUSP records, no multi-extent,
ambiguous ISO/Joliet namespace pairing or file locations, no directory
deletion, no additions/resizes, no deleting a file containing an El Torito
boot image/catalog location. Any mismatched layout fails unchanged.

Execution is transactional: validate all operations first; stage a private
same-volume full file copy; patch the precise directory sectors; independently
reparse both trees and validate boot area/catal​og sectors; replace the
source only after successful validation. A large image still requires a
full staged copy and enough free disk space; this is *not* a zero-copy edit.

The feature is deliberately **Preview and fails closed**. In particular,
bootloaders can reference filenames or physical file locations not exposed
by ISO9660 directory entries. The operation does not guarantee that a boot
system remains functional after intentionally deleting an OS-critical file.
Test real images on disposable copies and confirm BIOS/UEFI boot before release.

No external tools, packaged executables, or separate DLLs are used.
