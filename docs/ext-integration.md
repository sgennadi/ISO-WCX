# Native ext2/3/4 reader (Preview)

No mount, kernel driver, external extraction program or parser DLL.
Built as a C++20 volume backend directly into `ISO-WCX.wcx/wcx64`.
Available within partitioned IMG/RAW, fixed VHD and clean VHDX.

Supported in this experimental subset: 1/2/4 KiB blocks, 32/64-bit group
descriptors, inodes and regular files, nested directory traversal, ext2/3
direct + single/double/triple indirect pointers, ext4 extent trees
(including depth-limited index nodes and sparse/unwritten extents).

Safety: read-only bounded partition offsets, max 8 MiB per directory,
depth 32, max 100000 entries; invalid pointers, loops, traversal names and
unsupported features fail rather than following unsafe paths. Symlinks,
special files, compressed/encrypted/verity/inline/casefold inodes are not
extracted; journal recovery, meta_bg and bigalloc are refused.
Metadata checksums and authentic ext4 corpus compatibility remain a
known gap. This is NOT complete ext4 read support yet.

Validation: synthetic independent images cover legacy pointers and ext4
extents, nested directories, sparse files, malformed superblocks, group
descriptors, loops and rollback. Real ext4 images remain required.
