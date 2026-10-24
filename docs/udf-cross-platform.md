# UDF portable whole-device images (read-only)

The external [format-udf](https://github.com/JElchison/format-udf) project
is a GPL-2.0 Bash wrapper around operating-system UDF formatting tools,
**not** a reusable parser. It highlights UDF revision 2.01, logical block
sizes 512/4096, and an optional misleading FAT32-type (0x0B) MBR entry that
spans the **entire device from LBA zero**. No source from it is copied.

ISO-WCX independently recognizes UDF logical-sector sizes 512, 1024, 2048,
and 4096 using AVDP, descriptor tag checks and logical volume mapping.
For .raw/.img, the virtual partition reader is tried first; if no supported
partition filesystem exists, the optical/UDF reader validates the volume.
A fake MBR is not a real partition; do **not** shift UDF byte offsets, mount
it as FAT, or format any physical disk. This is a read-only feature.

**Writing remains strictly narrower:** only compatible, plain 2048-byte
UDF-only Type-1 images with a verified single-block bitmap/closed LVID are
writable in Preview. A nonzero system area (fake MBR/hybrid) and non-2048
UDF are rejected without changing the source. No mount or system formatter
is part of the WCX plugin.

Independent CI-only `mkudffs` builds UDF 2.01 512/4096-byte file images;
`udfinfo` validates the filesystem; a synthetic clean-room fake MBR is
added; `disc_inspect` must recognize .udf/.iso/.img/.raw by content; and
unsupported writes must preserve original bytes. UDF 1.02 2048-byte
write tests continue in the same workflow. No GPL code or external runtime
program is included in the plugin. Real device and cross-OS GUI tests are
outside this image-only regression suite.
