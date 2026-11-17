# WIM metadata directory decoder (read-only research stage)

The WIM fast edit preflight in `wim_fast_edit_probe.hpp` validates the header
and resource lookup. The next step, `wim_metadata_tree_probe.hpp`, parses
UNCOMPRESSED metadata resources only, without applying the image.

It checks the serialized security descriptor table, UTF-16 names and surrogate
pairs, directory and extra-stream lengths, security IDs, child-directory
pointers, duplicate/cyclic offsets, record overlap, and depth/resource budgets.
It never follows host filesystem links, writes to the WIM, or extracts file
payloads. There are no helper EXEs or additional runtime DLLs.

**Do not mistake a successful decoder result for permission to edit.** Most
real install.wim images have compressed metadata and are not decoded by this
stage. Neither hash-to-blob verification, all security descriptor ACL
semantics, extended reparse objects, content reference counts, the WIM
integrity resource, nor transaction-safe metadata serialization has been
implemented. The existing full WIMGAPI apply/capture editor remains in use.

## Stage 3A: bounded SHA-1 resource-link audit (read-only)

The separate `wim_resource_audit.hpp` scanner is the next research step.
It calculates the actual WIM-format SHA-1 digest over **uncompressed**
directory metadata, verifies metadata resource hashes from the 50-byte
lookup entries, and collects the 20-byte hashes referenced by regular
file entries and alternate data streams. Every nonzero hash must exist
in the lookup table and point to a non-metadata resource.

For small plain/uncompressed file and ADS resources, the scanner reads
at most 32 MiB per resource and 128 MiB in aggregate, then compares
its SHA-1 to the hash referenced by the metadata. It rejects truncated
lookup records, missing ADS/file resources, corrupt hash digests,
duplicate resource-hash descriptors and out-of-bounds locations.
The scan limits metadata to 128 MiB per image, 256 MiB across images,
and 1 million stream references and lookup rows.

At Stage 3A, compressed resources were deliberately not authenticated.
Stage 3B below adds bounded XPRESS-HUFF decoding on Windows. **LZX/LZMS,
unknown algorithms, and oversized payloads still return an explicitly
incomplete result** and are never treated as verified. Even a fully successful *bounded* SHA-1 audit does not check
all WIM integrity resources, ACL semantics, hard links, XML or global
reference counts. It is not an authorization for fast writes.
Original WIM files are opened read-only: no WIMGAPI Apply/Capture, no
mount, no external binaries, no temporary workspace and no modification.

The cross-platform CTest uses known SHA-1 vectors and independently
constructed disposable WIM fixtures with a regular file and an ADS.
It checks exact byte preservation after scanning, missing lookup
references, metadata and payload corruption, truncated tables, and
compressed-resource refusal.

## Stage 3B: compressed WIM chunk inspection and XPRESS-HUFF (read-only)

`wim_chunked_resource.hpp` now validates independent WIM compression
blocks using the WIM header chunk size (typically 32 KiB). A compressed
resource has an (N-1)-element offset table preceding the stored chunks.
Offsets must be strictly increasing and remain inside the resource.
The final chunk may have a shorter uncompressed length. Raw chunks
are copied only when their stored length matches the expected expanded
length. Invalid offsets, overlapping/empty blocks, unexpectedly large
chunks and excessive resource sizes are rejected before allocation.

For Windows 8+ the plugin resolves `CreateDecompressor`, `Decompress`
and `CloseDecompressor` from the system `Cabinet.dll` and uses
`COMPRESS_ALGORITHM_XPRESS_HUFF | COMPRESS_RAW`. Compressed XPRESS
metadata and bounded referenced file/ADS resources are SHA-1 verified
**after expansion**, and metadata must also pass the existing directory
and security-descriptor tree decoder. No other algorithm is silently
substituted for XPRESS. LZX, LZMS, XPRESS2, solid WIM/ESD/SWM and
unknown codecs are *not* supported by this path.

Limits remain 128 MiB per metadata resource, 256 MiB aggregate
metadata, 32 MiB per referenced file/ADS and 128 MiB aggregate
payloads. The audit runs only on explicit `Export Image Structure`;
regular WIM opening/F5 does not scan compressed data. Stage-specific
counters show XPRESS metadata/resources verified, validated chunk
tables, and unverified resources. The source WIM is never opened
for writing and no folder is mounted.

Cross-platform CTest verifies chunk boundary tables, raw blocks,
truncated/descending/duplicate offsets and LZX refusal. Windows
Win32/x64 tests generate actual raw XPRESS-HUFF blocks with the
system compressor and verify decoding of both a compressed metadata
resource and a referenced compressed file alongside a plain ADS.
This still does NOT validate the full WIM integrity stream, hardlink
semantics, ACLs, resource refcounts or atomic metadata serialization.
The direct metadata writer remains DISABLED.

Before enabling fast mkdir/delete, implement independent metadata decompression,
cross-reference every original stream hash against resource descriptors,
support the full Windows metadata forms, then perform staged atomic updates,
independently verify them with Windows WIMGAPI, and add genuine-image,
fuzzing, cancellation, power-loss, NTFS ACL, ADS and multiple-index tests.

Format study: Microsoft's published WIM layout and the independent
`microsoft/go-winio/wim` reader. No wimlib source is copied or linked.

Serialized directory and stream record sizes are rounded up to an 8-byte
storage boundary when advancing. A zero or 8-byte sentinel ends a sibling
list. Logical record length must still cover the declared names/streams.
