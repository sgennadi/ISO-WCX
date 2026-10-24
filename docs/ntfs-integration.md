# Native NTFS reader — ISO-WCX Preview

This read-only C++20 NTFS adapter is statically compiled into ISO-WCX
alongside FAT12/16/32, exFAT and ext2/3/4. It uses the same virtual disk
layers (partitioned IMG/RAW, fixed VHD, clean fixed/dynamic base VHDX).
No mounting, filesystem driver, extra DLL, Windows filesystem API, DISM
or third-party extraction command is used.

Initial implemented NTFS subset:
- Recognize the NTFS boot sector and validate sector/cluster/MFT geometry.
- Apply and check USA (multi-sector fixup) for FILE and INDX records.
- Read $MFT via validated nonresident data runs, including fragmented runs.
- Navigate directories using $I30 INDEX_ROOT and INDEX_ALLOCATION B+trees.
- Decode UTF-16 filenames, skip DOS name aliases, reject unsafe paths,
  stale references, reparse filenames and invalid metadata.
- Extract normal named-by-directory unnamed $DATA streams, both resident
  and nonresident, fragmented or sparse, with initialized-length zero-fill.
- Reuse the existing WCX atomic staging/rollback and progress cancellation.

Preview limits: No NTFS writes, journal replay, ATTRIBUTE_LIST extent
continuations, compressed/encrypted content, alternate named data streams,
reparse data, security descriptor restoration or file attribute recovery.
Metadata or filesystem corruption fails closed. Some real Windows NTFS
volumes that use attribute lists, self-healing or exotic layouts may be
rejected; production interoperability testing is not complete.

Synthetic tests cover fragmented FILE content, index allocation, resident
data, sparse/Unicode files, bad fixups, invalid MFT/reference/run metadata
and transactional rollback. Real Windows-created NTFS corpus and manual TC
GUI validation remain required before considering this stable.
