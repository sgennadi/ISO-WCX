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

**Compressed LZX/XPRESS/LZMS metadata or file resources are not
authenticated by this stage.** They return an explicitly incomplete
result; large uncompressed payloads above the audit budget also remain
unverified. Even a fully successful *bounded* SHA-1 audit does not check
all WIM integrity resources, ACL semantics, hard links, XML or global
reference counts. It is not an authorization for fast writes.
Original WIM files are opened read-only: no WIMGAPI Apply/Capture, no
mount, no external binaries, no temporary workspace and no modification.

The cross-platform CTest uses known SHA-1 vectors and independently
constructed disposable WIM fixtures with a regular file and an ADS.
It checks exact byte preservation after scanning, missing lookup
references, metadata and payload corruption, truncated tables, and
compressed-resource refusal.

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
