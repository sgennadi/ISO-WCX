# libMirage (CDEmu) compatibility comparison

**Reference:** [CDEmu / libMirage](https://github.com/cdemu/cdemu/tree/master/libmirage)
and the version 3.3.3 \`libmirage/README\` in the upstream repository.

This document is an **interoperability roadmap**, not a representation that any
upstream libMirage source code is included or that ISO-WCX supports every
format listed by libMirage.

## Architecture

libMirage separates:
1. image descriptors (disc / session / track layout),
2. logical sector reading and optional subchannel data,
3. random-access container filters and decoding.

ISO-WCX keeps the read-only \`Disc\` / \`Source\` parser, C++20 and portable WCX
interface. New format parsers must emit validated ranges, never import the
upstream GPL implementation, and must work through the shared bounded reader.

## Format coverage and verification

| Image / filter | ISO-WCX v0.11.0 preview | Missing / test next |
| --- | --- | --- |
| ISO/Joliet/Rock Ridge/UDF | Supported tested subsets | Vendor discs, metadata variants, UDF corner cases |
| CUE/BIN | BINARY Mode 1 / Mode 2 Form 1 | CD-DA and alternate codecs |
| NRG | Selected uncompressed DAO/TAO data tracks | Full TOC / subchannels |
| MDS/MDF | MDS v1 selected uncompressed data | MDS v2, MDX |
| CloneCD CCD/IMG | New single-session unswizzled Mode 1/2 Form 1 | Multi-session, scrambled tracks, .SUB |
| CSO / ZSO | Indexed random reads | More authentic compressed images |
| CHD | Not implemented | Independent parser or suitably licensed dependency |
| ISZ / DAA | Not implemented | Bounds-checked block-index reader and codec support |
| XZ / GZ / DMG / ECM | Not implemented | Filtered logical stream, bounded caches, memory limits |
| B5T/B6T, CDI, CIF, C2D | Not implemented | Descriptor parsing and synthetic fixtures |

## Licensing and safety

The CDEmu / libMirage project carries **GPL version 2 or later** licensing
(see its \`libmirage/COPYING\`). Do not copy or link libMirage's GPL code into
this repository without a compatible licensing decision and full compliance
with the applicable obligations. Only use publicly documented format facts,
original work and permissively licensed dependencies where appropriate.

Every new loader should pass:
- Windows x64 and Win32 WCX LoadLibrary / list / extract tests;
- Linux and Windows synthetic success / damaged-image regressions;
- negative tests for offset overflow, malformed index metadata, cycles,
  symlink escapes, and unsupported encrypted/scrambled data;
- targeted real-image interoperability tests *before* claiming full format
  compatibility.

**Note:** A libMirage format listing is not evidence that ISO-WCX already
implements that format.
