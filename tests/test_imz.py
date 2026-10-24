#!/usr/bin/env python3
"""IMZ WinImage ZIP/Deflate integration and strict refusal tests.

Uses the same Disc/WCX reader path as the native IMA support. The ZIP test
fixtures are independent stdlib-generated archives: no runtime ZIP process.
"""
from __future__ import annotations
import pathlib
import struct
import sys
import tempfile
import zipfile
from test_floppy_edit import floppy_image, readback, run

def make_zip(path: pathlib.Path, image: bytes, method: int) -> None:
    with zipfile.ZipFile(path, "w", compression=method) as archive:
        archive.writestr("diskette.ima", image)

def refuse(reader: pathlib.Path, image: pathlib.Path) -> None:
    run(reader, image, ok=False)

def main(editor: pathlib.Path, reader: pathlib.Path) -> None:
    with tempfile.TemporaryDirectory(prefix="iso-wcx-imz-") as td:
        root = pathlib.Path(td)
        original = floppy_image()
        expected = {
            "HELLO.TXT": b"Hello floppy",
            "Long Filename.txt": b"Long name kept",
            "TESTDIR/NESTED.TXT": b"Nested original",
        }
        for method in (zipfile.ZIP_DEFLATED, zipfile.ZIP_STORED):
            image = root / ("test-deflate.imz" if method else "test-stored.imz")
            make_zip(image, original, method)
            output = run(reader, image).stdout.decode("utf-8")
            assert "IMZ / FAT12 floppy" in output, output
            readback(reader, image, expected, root / image.stem)
            before = image.read_bytes()
            payload = root / "input.bin"
            payload.write_bytes(b"attempt write")
            run(editor, image, "add", "NEW.TXT", payload, ok=False)
            assert image.read_bytes() == before, "compressed IMZ mutated by editor"

        # CRC mismatch: central directory can be rewritten independently of
        # compressed data; the whole IMZ must be rejected.
        bad_crc = root / "bad-crc.imz"
        make_zip(bad_crc, original, zipfile.ZIP_DEFLATED)
        broken = bytearray(bad_crc.read_bytes())
        cd = broken.index(b"PK\x01\x02")
        struct.pack_into("<I", broken, cd + 16,
                         struct.unpack_from("<I", broken, cd + 16)[0] ^ 1)
        bad_crc.write_bytes(broken)
        refuse(reader, bad_crc)

        # Reject more than one file (no accidental selection or hidden payload).
        multi = root / "multi.imz"
        with zipfile.ZipFile(multi, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("diskette.ima", original)
            archive.writestr("other.txt", "untrusted")
        refuse(reader, multi)

        # Reject nested relative paths and directory entries.
        nested = root / "nested.imz"
        with zipfile.ZipFile(nested, "w") as archive:
            archive.writestr("../diskette.ima", original)
        refuse(reader, nested)

        # An IMZ claims 33 MiB output but contains a small compressed stream:
        # the bound must be checked before allocation or decompression.
        huge = root / "zip-bomb.imz"
        with zipfile.ZipFile(huge, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            archive.writestr("oversized.ima", bytes(33 * 1024 * 1024))
        refuse(reader, huge)

        # ZIP headers advertising encryption must not be silently read.
        encrypted = root / "encrypted-flag.imz"
        make_zip(encrypted, original, zipfile.ZIP_DEFLATED)
        broken = bytearray(encrypted.read_bytes())
        cd = broken.index(b"PK\x01\x02")
        local = broken.index(b"PK\x03\x04")
        for at in (local + 6, cd + 8):
            value = struct.unpack_from("<H", broken, at)[0]
            struct.pack_into("<H", broken, at, value | 1)
        encrypted.write_bytes(broken)
        refuse(reader, encrypted)
        print("PASS: native IMZ stored+deflated FAT12 browse/extract; "
              "CRC, encrypted, traversal, multiple member and zip-bomb refusal")


if __name__ == "__main__":
    main(*(pathlib.Path(p).resolve() for p in sys.argv[1:3]))
