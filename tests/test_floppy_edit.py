#!/usr/bin/env python3
"""Native IMA/IMG/BIF floppy and FAT12 transactional edit regressions.

Independently constructs a 1.44 MB floppy with two FAT copies, a directory,
an LFN file and both normal and opaque El Torito BIF fixtures.
No disk mounting or external utilities needed.
"""
from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile

from test_virtual_fat import fat_set, short_entry, vfat_entries

SECTOR = 512
SECTORS = 2880
FAT_START = SECTOR
FAT_SIZE = 9 * SECTOR
ROOT_START = 19 * SECTOR
DATA_START = 33 * SECTOR


def cluster(c: int) -> int:
    return DATA_START + (c - 2) * SECTOR


def floppy_image() -> bytes:
    image = bytearray(SECTORS * SECTOR)
    boot = bytearray(SECTOR)
    boot[:3] = b"\xEB\x3C\x90"
    boot[3:11] = b"MSDOS5.0"
    boot[11:13] = (512).to_bytes(2, "little")
    boot[13] = 1
    boot[14:16] = (1).to_bytes(2, "little")
    boot[16] = 2
    boot[17:19] = (224).to_bytes(2, "little")
    boot[19:21] = (SECTORS).to_bytes(2, "little")
    boot[21] = 0xF0
    boot[22:24] = (9).to_bytes(2, "little")
    boot[24:26] = (18).to_bytes(2, "little")
    boot[26:28] = (2).to_bytes(2, "little")
    boot[510:512] = b"\x55\xAA"
    image[:SECTOR] = boot

    fat = bytearray(FAT_SIZE)
    fat[:3] = b"\xF0\xFF\xFF"
    for c in (2, 3, 4, 5):
        fat_set(fat, "fat12", c, 0xFFF)
    image[FAT_START:FAT_START + FAT_SIZE] = fat
    image[FAT_START + FAT_SIZE:FAT_START + 2 * FAT_SIZE] = fat

    root = b"".join((
        short_entry(b"HELLO   TXT", 0x20, 2, len(b"Hello floppy")),
        short_entry(b"TESTDIR    ", 0x10, 3),
        vfat_entries("Long Filename.txt", b"LONGNA~1TXT"),
        short_entry(b"LONGNA~1TXT", 0x20, 4, len(b"Long name kept")),
    ))
    image[ROOT_START:ROOT_START + len(root)] = root
    child = b"".join((
        short_entry(b".          ", 0x10, 3),
        short_entry(b"..         ", 0x10, 0),
        short_entry(b"NESTED  TXT", 0x20, 5, len(b"Nested original")),
    ))
    image[cluster(3):cluster(3) + len(child)] = child
    for c, content in (
        (2, b"Hello floppy"),
        (4, b"Long name kept"),
        (5, b"Nested original"),
    ):
        image[cluster(c):cluster(c) + len(content)] = content
    return bytes(image)


def run(executable: pathlib.Path, *args: object, ok: bool = True) -> subprocess.CompletedProcess:
    proc = subprocess.run([str(executable), *(str(a) for a in args)],
                          capture_output=True, timeout=120, check=False)
    assert (proc.returncode == 0) == ok, (
        f"{executable.name} {args!r}, rc={proc.returncode}, "
        f"stdout={proc.stdout[-400:]!r}, stderr={proc.stderr[-700:]!r}")
    return proc


def readback(reader: pathlib.Path, image: pathlib.Path, expected: dict[str, bytes],
             destination: pathlib.Path):
    output = run(reader, image).stdout.decode("utf-8")
    for name in expected:
        assert name in output, (image, name, output)
    run(reader, image, "--extract", destination)
    for name, value in expected.items():
        assert (destination / name).read_bytes() == value, (image, name)


def mirrors_ok(data: bytes):
    assert data[:SECTOR] == floppy_image()[:SECTOR], "boot sector changed"
    assert data[FAT_START:FAT_START + FAT_SIZE] == (
        data[FAT_START + FAT_SIZE:FAT_START + 2 * FAT_SIZE]), "FAT mirrors differ"
    assert len(data) == SECTORS * SECTOR, "floppy geometry changed"


def main(editor: pathlib.Path, reader: pathlib.Path):
    with tempfile.TemporaryDirectory(prefix="iso-wcx-floppy-") as td:
        root = pathlib.Path(td)
        original = floppy_image()
        basic = {
            "HELLO.TXT": b"Hello floppy",
            "TESTDIR/NESTED.TXT": b"Nested original",
            "Long Filename.txt": b"Long name kept",
        }
        for ext in (".ima", ".img", ".vfd", ".flp", ".bif"):
            image = root / ("read" + ext)
            image.write_bytes(original)
            readback(reader, image, basic, root / ("out-" + ext[1:]))
            assert "Raw FAT12 floppy" in run(reader, image).stdout.decode()

        # Opaque El Torito BIF: preserve and export original bytes rather
        # than mistaking no-emulation boot code for a FAT directory.
        opaque = root / "opaque.bif"
        bootcode = (b"\xFA\x31\xC0\x8E\xD8" * 512)
        opaque.write_bytes(bootcode)
        output = run(reader, opaque).stdout.decode()
        assert "boot-image.bin" in output and "opaque" in output
        run(reader, opaque, "--extract", root / "opaque-out")
        assert (root / "opaque-out/boot-image.bin").read_bytes() == bootcode

        image = root / "edit.ima"
        image.write_bytes(original)
        payload = root / "payload.bin"
        payload.write_bytes((b"FAT12 edited contents: " * 100) + b"!")
        run(editor, image, "add", "NEWFILE.TXT", payload)
        run(editor, image, "add", "TESTDIR/NESTED.TXT", payload)
        run(editor, image, "mkdir", "TOOLS")
        run(editor, image, "add", "TOOLS/SECOND.DAT", payload)
        readback(reader, image, {
            **basic, "TESTDIR/NESTED.TXT": payload.read_bytes(),
            "NEWFILE.TXT": payload.read_bytes(),
            "TOOLS/SECOND.DAT": payload.read_bytes(),
        }, root / "edit-out")
        mirrors_ok(image.read_bytes())

        # Existing LFN replacement must change data without losing its
        # original display name, VFAT metadata or DOS alias.
        run(editor, image, "add", "long filename.TXT", payload)
        readback(reader, image, {
            "Long Filename.txt": payload.read_bytes(),
            "HELLO.TXT": b"Hello floppy",
        }, root / "replace-existing-lfn")
        run(editor, image, "delete", "LONGNA~1.TXT")
        assert "Long Filename.txt" not in run(reader, image).stdout.decode()

        # Create and extract true Unicode long names: Cyrillic, Hebrew,
        # supplementary emoji, and two colliding 8.3 alias prefixes.
        name_ru = "Очень длинное имя файла 📦.txt"
        name_he = "תיקיית מסמכים"
        name_deep = "D" * 60 + "📦.dat"
        name_second = "Y" * 65 + ".BIN"
        run(editor, image, "mkdir", name_he)
        run(editor, image, "add", name_ru, payload)
        run(editor, image, "add", name_he + "/תקציב 2026.xlsx", payload)
        run(editor, image, "add", name_he + "/" + name_deep, payload)
        # Two >13-unit names force an LFN chain across a FAT directory cluster.
        run(editor, image, "add", name_he + "/" + name_second, payload)
        run(editor, image, "add", "Documents version one.txt", payload)
        run(editor, image, "add", "Documents version two.txt", payload)
        run(editor, image, "add", "lowercase.txt", payload)
        readback(reader, image, {
            name_ru: payload.read_bytes(),
            name_he + "/תקציב 2026.xlsx": payload.read_bytes(),
            name_he + "/" + name_deep: payload.read_bytes(),
            name_he + "/" + name_second: payload.read_bytes(),
            "Documents version one.txt": payload.read_bytes(),
            "Documents version two.txt": payload.read_bytes(),
            "lowercase.txt": payload.read_bytes(),
        }, root / "unicode-lfn")
        assert b"DOCUME~1TXT" in image.read_bytes()
        assert b"DOCUME~2TXT" in image.read_bytes()
        run(editor, image, "delete", name_he)
        listing = run(reader, image).stdout.decode("utf-8")
        assert name_he not in listing
        run(editor, image, "delete", name_ru)
        assert name_ru not in run(reader, image).stdout.decode("utf-8")
        mirrors_ok(image.read_bytes())

        # Batch mkdir and file additions must be ordered correctly.
        image.write_bytes(original)
        run(editor, image, "batch", "add", "DATA/FILE.TXT", payload,
            "mkdir", "DATA", "mkdir", "DATA/SUB",
            "add", "DATA/SUB/EXTRA.BIN", payload)
        readback(reader, image, {
            **basic, "DATA/FILE.TXT": payload.read_bytes(),
            "DATA/SUB/EXTRA.BIN": payload.read_bytes(),
        }, root / "batch-out")
        run(editor, image, "delete", "DATA")
        assert "DATA/" not in run(reader, image).stdout.decode()
        mirrors_ok(image.read_bytes())

        # Keep original bytes on cancellation and unsupported operation.
        for op, name in (("cancel", "CANCEL.TXT"),
                         ("add", "bad?.txt"),
                         ("add", "../unsafe.txt"),
                         ("add", "X" * 256 + ".txt")):
            before = image.read_bytes()
            run(editor, image, op, name, payload, ok=False)
            assert image.read_bytes() == before
        before = opaque.read_bytes()
        run(editor, opaque, "add", "TRY.TXT", payload, ok=False)
        assert opaque.read_bytes() == before

        # A damaged mirror or cyclic cluster chain must never be rewritten.
        mismatch = root / "mismatch.ima"
        broken = bytearray(original)
        broken[FAT_START + FAT_SIZE + 3] ^= 0x7F
        mismatch.write_bytes(broken)
        run(editor, mismatch, "add", "EXTRA.TXT", payload, ok=False)
        assert mismatch.read_bytes() == broken

        cycle = root / "cycle.img"
        broken = bytearray(original)
        for at in (FAT_START, FAT_START + FAT_SIZE):
            table = bytearray(broken[at:at + FAT_SIZE])
            fat_set(table, "fat12", 2, 2)
            broken[at:at + FAT_SIZE] = table
        cycle.write_bytes(broken)
        run(editor, cycle, "add", "EXTRA.TXT", payload, ok=False)
        assert cycle.read_bytes() == broken
        assert not list(root.rglob("*.iso-wcx-fat12-*.tmp"))

        print("PASS: native IMA/IMG/VFD/FLP/BIF, FAT12 write & rollback, "
              "LFN preservation, mirrors, nested folders, opaque BIF")


if __name__ == "__main__":
    main(*(pathlib.Path(p).resolve() for p in sys.argv[1:3]))
