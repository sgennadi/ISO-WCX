# ISO/UDF Bridge same-length data replacement (Preview)

ISO-WCX can update **file contents only** for a strict safe subset of
nonbootable ISO/UDF Bridge images. This feature lives inside the main
C++20 WCX DLL; no helper EXE, disk mount, driver or extra DLL is required.

## Preconditions

- The image is a valid cooked 2048-byte ISO9660 (optionally Joliet) **and**
  UDF bridge, and both parsers independently accept their metadata.
- The requested relative file path exists as a regular file in **both**
  namespaces. A file must have a nonzero size.
- The ISO LBA and UDF mapping point at the **identical physical byte extent**;
  UDF must use one nonsparse, nonembedded extent with no shared/overlapping
  mapping. More complex UDF allocation mappings are rejected.
- Replacement must have **exactly the same byte length**.
- El Torito bootable bridges are excluded from this initial write mode.
  Images with nonzero system-area hybrid boot code, Rock Ridge, complex
  ISO multi-extent layouts, or unsupported UDF structures are not rewritten.
- There is adequate free space beside the image for a full staged copy.
  No read/write locks may prevent replacement of the original image.

## Transaction and verification

The editor parses ISO and UDF, checks file maps, creates a sibling temporary
copy, patches only the validated file data range, reparses **both** staged
namespaces, and compares file bytes against the replacement source.
A failed check, cancellation, or unsupported mutation leaves the original
path intact. The staged file is atomically replaced only after validation.

## Not supported

No directory creation/removal, adding/deleting files, resizing, UDF FE/FID
metadata writes, free-space allocation, updating VAT/metadata partitions,
repairing corrupted UDF, NTFS metadata restoration, or rewriting El Torito
on bridged media. UDF-only optical images remain read-only.

The test suite builds a self-contained synthetic ISO/UDF Bridge with a shared
file data extent, tests successful replacement through the same WCX edit
core, independently reads back ISO and UDF, and tests rollback when the
file sizes or extents differ or the image is bootable.

This remains **Preview**: a broad real-media interoperability corpus and
manual Total Commander GUI testing are still required.
