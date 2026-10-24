# exFAT read-only integration (ISO-WCX Preview)

A native C++20 exFAT 1.0 volume reader has been added to the same
`VirtualArchive` as FAT12/16/32. It reads partitions in MBR/GPT IMG/RAW,
fixed VHD and clean base VHDX, without a mount, driver or extra DLL.

Supports Unicode UTF-16 file names, nested directories, FAT-chained and
NoFatChain contiguous file streams, validated entry-set/boot checksums,
allocation bitmap checks, valid-data length zero-fill, cancellation via
the WCX output callback and bounded reads.

Safety boundaries: read-only, no TexFAT, no journal recovery, no
symlinks/reparse targets, root/dir size cap 8 MiB, recursion depth 32,
100,000 entries, maximum 1 MiB cluster; malformed chain/metadata fail
closed. Real-world large-volume/exFAT vendor corpus is still pending.

Other filesystem families (NTFS/ext) and UDF/ISO editor extensions are
separate future stages.
