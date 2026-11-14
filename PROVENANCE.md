# Code provenance and publication policy

The original ISO.WCX 1.7.6 sources identify Sergey Oblomov and other contributors in their documentation, but the provided archive did not establish an explicit redistribution license.

**Policy:** do not commit, redistribute, or incorporate the legacy ISO.WCX source files in this public repository until their license and permissions are verified.

The initial C++20 scaffold, utility header, tests, workflow, and documents were newly written for this repository. The experimental UDF files from earlier work remain outside this repository pending separate review.

Modernization must retain Total Commander WCX ABI compatibility and should be accompanied by independent test fixtures and documented source provenance.

## v0.14 compressed optical containers

Native, independently authored C++20 readers use only format layout facts
cross-checked against upstream references:

- ISZ layout: oserres/isz-tool (GPLv3): no code copied or linked.
- DAA v1 layout: rsenn/daa2iso (GPLv2): no code copied or linked.
- CHD v5 map structure was initially researched from rtissera/libchdr (BSD-3)
  without copying codec source. The v0.14 compressed CHD reader now **links
  libchdr statically** at pinned commit
  `47d794151b2b64f6aba60304e6480f4f5a2bc24b`.
  Our C++ CD/DVD optical adapter is separately authored. libchdr's bundled
  miniz, LZMA SDK, Zstandard and dr_flac are all statically compiled; their
  notices are included in THIRD_PARTY_NOTICES.txt.

Compression libraries: zlib is statically linked on Windows (as previously).
bzip2/libbzip2 1.0.8 is built from pinned upstream source with a verified
SHA-256 and statically linked for Windows/Linux (the end user installs no DLL).
Copyright (C) Julian R Seward, 1996-2010; see
https://sourceware.org/bzip2/ and its upstream license.

## ISO Master / TotalImage format research (2026-11-14)

- ISO Master uses bkisofs and is distributed as GPLv2. Its ISO9660,
  Joliet, Rock Ridge and El Torito workflows were reviewed as feature
  references only. No ISO Master / bkisofs source is copied, linked or
  distributed in ISO-WCX.
- TotalImage (https://github.com/TotalImage/TotalImage), MIT license,
  copyright (c) 2020-2026 TotalImage Team, is a reference for Anex86
  FDI/HDI and T98-Next NHD layout fields. Implementation is independent
  C++20, additionally cross-checked against the public PC-98 format
  descriptions at https://www.pc98.org/project/doc/hdi.html and
  https://www.pc98.org/project/doc/nhd.html. No TotalImage source code
  or binaries are embedded or distributed. The reference project's
  status page documents predominantly read-only filesystem handlers;
  those are not presented as write-ready ISO-WCX features.
- The native read path requires a complete, in-bounds header and exact
  CHS/data length; it never treats an invalid headered image as a raw
  disk. FDI FAT floppy and MBR/GPT partitioned HDI/NHD are read-only.
  PC-98 native partitions require a separate strictly validated parser.
