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
