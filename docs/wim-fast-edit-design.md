# WIM metadata-only editing: safe implementation plan

## Why an empty folder is slow today

Our Windows WIMGAPI backend must apply the selected WIM index to a private
NTFS workspace, create the folder and capture the index again. PR #61
reports WIMGAPI progress, preserves cancellation and eliminates redundant
readback of every unchanged compressed stream, but **not** Apply/Capture.

## A faster approach

A WIM is a collection of content-addressed resources and per-image directory
metadata. A dedicated editor could change one directory entry by appending
only a new metadata resource, adjusting lookup/XML/integrity structures and
atomically publishing a verified staged image. It must **never** decompress,
copy or recapture unchanged file payloads merely to create a directory.

## Stage 1: read-only preflight (implemented, no edits)

The header-only \`wim_fast_edit_probe.hpp\` parses only the 208-byte WIM header
and uncompressed lookup table, with bounds checking against file size and
limits on table and metadata sizes. It reports image/metadata counts and whether
the metadata resource is compressed. It rejects malformed offsets, overlaps,
read-only and segmented formats from candidate classification.

**A metadata layout candidate does not mean it is safe to write.** The
preflight does not decode the metadata directory tree or security descriptors,
does not verify resource hashes or integrity tables, and does not modify any
archive. It is a development prerequisite, not a new WCX writing capability.

## Stage 2: metadata decoder

Validate the complete metadata resource, security descriptor table, UTF-16
directory records, file/stream references, hard links, parent/subdirectory
offsets and supported compression algorithms. Reject unsupported extensions
and encrypted/solid/delta/spanned/read-only WIMs. Add independent tests using
genuine WIM images created by Windows WIMGAPI plus mutation/fuzz cases.

## Experimental Windows mount-and-commit Preview (after v0.21.0)

A separate, opt-in folder-only writer uses the system WIMGAPI mount driver on
a transactional sibling clone. It avoids the full WIMApplyImage/WIMCaptureImage
pipeline for a batch of directory creations within one index. It is **not**
a custom WIM metadata serializer and is **disabled by default** until real
privileged Windows mount recovery and content-integrity verification finish.
See [WIM mount fast Preview](wim-mount-fast-preview.md).

## Stage 3: transactional fast writer

On a private staged copy, append a rebuilt metadata resource and update
resource hashes/refcounts, lookup table, XML counts, WIM header and integrity
table if present. Never overwrite the original in place. Verify the staged
WIM with **independent Windows WIMGAPI**, compare edited and unchanged trees,
and atomically replace the source only after full checks. Test power-failure
and cancellation paths, mixed indexes, Unicode and full stream/ACL fidelity.

## Library and license assessment

[wimlib](https://github.com/ebiggers/wimlib) supports
\`wimlib_update_image()\` and \`wimlib_overwrite()\`, normally appending new
metadata without full recapture. Its Windows build instructions use MinGW-w64;
Visual Studio builds are not directly supported upstream. Its COPYING file
offers GPLv3+ and (without GPL-only dependencies) LGPLv2.1+ for the library.
Embedding or static linking wimlib into a single proprietary-distribution WCX
would require an explicit compliance, source/relinkability and build review.
Nothing from wimlib is copied, statically linked or distributed here.

For now, ISO-WCX uses only Windows system WIMGAPI in-process. Fast direct
metadata writes must remain disabled until independent on-disk tests pass.
