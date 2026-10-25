#!/usr/bin/env python3
"""Independent FAT16/FAT32 transactional writer regression fixtures.

Uses synthetic partitioned FAT images, then independently converts their
filesystem bytes into 2-FAT, exact-length, unpartitioned raw superfloppies.
Tests mirroring, FAT32 FSInfo/backup, high cluster words, VFAT LFN, refusal
and atomic rollback. No OS mounting or external disk tools required.
"""
from __future__ import annotations

import pathlib
import struct
import sys
import tempfile

from test_virtual_fat import (SECTOR, PARTITION_LBA, FAT_CONFIG,
                              build_disk, fat_set)
from test_floppy_edit import run, readback

MAX_FAT32_CLUSTER = 0x0fffffff


def le16(raw: bytes, at: int) -> int:
    return struct.unpack_from("<H", raw, at)[0]


def le32(raw: bytes, at: int) -> int:
    return struct.unpack_from("<I", raw, at)[0]


def make_fsinfo() -> bytes:
    sector = bytearray(SECTOR)
    struct.pack_into("<I", sector, 0, 0x41615252)
    struct.pack_into("<I", sector, 484, 0x61417272)
    struct.pack_into("<II", sector, 488, 0xffffffff, 0xffffffff)
    struct.pack_into("<I", sector, 508, 0xaa550000)
    return bytes(sector)


def make_raw(root: pathlib.Path, kind: str) -> pathlib.Path:
    original = root / ("partitioned-" + kind + ".img")
    build_disk(original, kind)
    total, reserved, fat_sectors, _, _ = FAT_CONFIG[kind]
    with original.open("rb") as src:
        src.seek(PARTITION_LBA * SECTOR)
        raw = bytearray(src.read(total * SECTOR))
    assert len(raw) == total * SECTOR and raw[510:512] == b"\x55\xaa"

    # The independent partitioned fixture has one FAT. Move its data area
    # to create two identical mirrored FATs without increasing volume length.
    fat_start = reserved * SECTOR
    fat_length = fat_sectors * SECTOR
    after_fat = fat_start + fat_length
    table = bytearray(raw[fat_start:after_fat])
    if kind == "fat16":
        fat_set(table, kind, 0, 0xfff8)
    else:
        fat_set(table, kind, 0, 0x0ffffff8)
    raw = raw[:fat_start] + table + table + raw[after_fat:-fat_length]
    raw[16] = 2
    if kind == "fat32":
        # A FAT32 writer must validate and rewrite both FSInfo records.
        raw[48:50] = (1).to_bytes(2, "little")
        raw[50:52] = (6).to_bytes(2, "little")
        raw[SECTOR:2 * SECTOR] = make_fsinfo()
        raw[6 * SECTOR:7 * SECTOR] = raw[:SECTOR]
        raw[7 * SECTOR:8 * SECTOR] = make_fsinfo()
    assert len(raw) == total * SECTOR
    output = root / ("write-" + kind + ".img")
    output.write_bytes(raw)
    return output


def geometry(image: pathlib.Path, kind: str) -> tuple[int, int, int]:
    raw = image.read_bytes()
    reserved = le16(raw, 14)
    sectors = le16(raw, 22) if kind == "fat16" else le32(raw, 36)
    length = sectors * SECTOR
    at = reserved * SECTOR
    assert raw[at:at + length] == raw[at + length:at + 2 * length], \
        "FAT mirrors differ after transactional write"
    assert raw[16] == 2, "FAT mirroring disabled"
    return at, length, len(raw)


def assert_fsinfo(image: pathlib.Path):
    raw = image.read_bytes()
    p = raw[SECTOR:SECTOR + SECTOR]
    backup = raw[7 * SECTOR:8 * SECTOR]
    assert p == backup and le32(p, 0) == 0x41615252
    assert le32(p, 484) == 0x61417272
    assert le32(p, 508) == 0xaa550000
    free_count = le32(p, 488)
    next_free = le32(p, 492)
    assert 0 < free_count < 70000, free_count
    assert 2 <= next_free <= 65536 + 4096, next_free
    boot = raw[:SECTOR]
    assert raw[6 * SECTOR:7 * SECTOR] == boot, "backup boot sector changed"


def reject(editor: pathlib.Path, image: pathlib.Path,
           source: pathlib.Path, root: pathlib.Path, name: str):
    before = image.read_bytes()
    run(editor, image, "add", "REJECT.TXT", source, ok=False)
    assert image.read_bytes() == before, name
    assert not list(root.rglob("*.iso-wcx-fat-*.tmp")), name


def exercise(kind: str, editor: pathlib.Path, reader: pathlib.Path,
             root: pathlib.Path):
    image = make_raw(root, kind)
    initial = image.read_bytes()
    expected = {
        "README.TXT": (("Hello from " + kind + "!\n") * 48).encode(),
        "Long Filename.txt": ("VFAT-" + kind).encode(),
        "TESTDIR/NESTED.TXT": ("nested-" + kind).encode(),
    }
    listing = run(reader, image).stdout.decode("utf-8")
    assert (("FAT16" if kind == "fat16" else "FAT32") in listing), listing
    readback(reader, image, expected, root / ("initial-" + kind))

    payload = root / (kind + "-input.bin")
    payload.write_bytes(b"fat transaction test " * 89 + b"end")
    long_name = "Длинное имя документа 📁.txt"
    nested = "מחלקת נתונים"
    run(editor, image, "batch", "add", nested + "/hello 世界.bin", payload,
        "mkdir", nested, "add", "DATA001.DAT", payload,
        "mkdir", "NEW-DIR")
    run(editor, image, "add", long_name, payload)
    readback(reader, image, {
        **expected,
        nested + "/hello 世界.bin": payload.read_bytes(),
        "DATA001.DAT": payload.read_bytes(),
        long_name: payload.read_bytes(),
    }, root / ("edited-" + kind))
    at, length, size = geometry(image, kind)
    assert len(initial) == size, "image geometry changed"
    if kind == "fat32":
        assert_fsinfo(image)
        # The root directory initially occupies a single 512-byte cluster.
        # This Unicode LFN overflows its 16 slots and tests a chain extension
        # where the LFN records cross a physical cluster boundary.
        expand = "root expansion and Unicode 📁 example.txt"
        run(editor, image, "add", expand, payload)
        readback(reader, image, {expand: payload.read_bytes()},
                 root / "expanded-fat32-root")
        grown = image.read_bytes()
        fat_at = le16(grown, 14) * SECTOR
        assert (le32(grown, fat_at + 6 * 4) & MAX_FAT32_CLUSTER) < 0x0ffffff8,             "FAT32 root directory did not extend to the next cluster"
        assert_fsinfo(image)

    # Replacement in-place preserves both existing VFAT long-name records
    # and original DOS short name, even when using case-insensitive lookup.
    run(editor, image, "add", "LONG FILENAME.TXT", payload)
    readback(reader, image, {
        "Long Filename.txt": payload.read_bytes(),
        "TESTDIR/NESTED.TXT": expected["TESTDIR/NESTED.TXT"],
    }, root / ("replace-" + kind))
    run(editor, image, "delete", nested)
    assert nested not in run(reader, image).stdout.decode("utf-8")
    run(editor, image, "delete", long_name)
    assert long_name not in run(reader, image).stdout.decode("utf-8")
    geometry(image, kind)
    if kind == "fat32":
        assert_fsinfo(image)

    # Refuse damaged mirror, bad boot geometry, truncated volume and
    # malformed FAT32 primary/backup FSInfo/boot without touching original.
    mirror = root / ("mirror-damaged-" + kind + ".img")
    corrupt = bytearray(image.read_bytes())
    corrupt[at + length + 20] ^= 0x20
    mirror.write_bytes(corrupt)
    reject(editor, mirror, payload, root, "mirror divergence")

    geometry_bad = root / ("geometry-bad-" + kind + ".img")
    corrupt = bytearray(image.read_bytes())
    corrupt[13] = 3  # Non-power-of-two sectors per cluster
    geometry_bad.write_bytes(corrupt)
    reject(editor, geometry_bad, payload, root, "BPB geometry")

    truncated = root / ("truncated-" + kind + ".img")
    truncated.write_bytes(image.read_bytes()[:-SECTOR])
    reject(editor, truncated, payload, root, "truncated disk")

    if kind == "fat32":
        for name, offset in (
            ("fsinfo", SECTOR),
            ("backup-fsinfo", 7 * SECTOR),
            ("backup-boot", 6 * SECTOR),
        ):
            broken = root / (name + ".img")
            corrupt = bytearray(image.read_bytes())
            corrupt[offset] ^= 0x01
            broken.write_bytes(corrupt)
            reject(editor, broken, payload, root, name)
        bad_active = root / "active-fat.img"
        corrupt = bytearray(image.read_bytes())
        corrupt[40:42] = (0x0080).to_bytes(2, "little")
        bad_active.write_bytes(corrupt)
        reject(editor, bad_active, payload, root, "active FAT mode")

        # Force next writable cluster past 65535. The new file's FAT32
        # directory entry must carry high-word cluster bits correctly.
        high = root / "high-cluster.img"
        corrupt = bytearray(initial)
        start = le16(corrupt, 14) * SECTOR
        fatbytes = le32(corrupt, 36) * SECTOR
        table = bytearray(corrupt[start:start + fatbytes])
        for c in range(8, 65536):
            fat_set(table, kind, c, MAX_FAT32_CLUSTER)
        # The upper four bits belong to the on-disk FAT32 record, not the
        # 28-bit cluster value: allocation must preserve them unchanged.
        fat_set(table, kind, 65536, 0xa0000000)
        for copy in (0, 1):
            corrupt[start + copy * fatbytes:start + (copy + 1) * fatbytes] = table
        high.write_bytes(corrupt)
        run(editor, high, "add", "HIGH.BIN", payload)
        readback(reader, high, {"HIGH.BIN": payload.read_bytes()},
                 root / "high-cluster-readback")
        assert_fsinfo(high)
        after = high.read_bytes()
        for fat_copy in (0, 1):
            raw_entry = le32(after, start + fat_copy * fatbytes + 65536 * 4)
            assert raw_entry & 0xf0000000 == 0xa0000000,                 "writer cleared FAT32 reserved high nibble"
        geometry(high, kind)

    # Explicit partitioned FAT input must stay read-only for this preview.
    partitioned = root / ("partitioned-" + kind + ".img")
    reject(editor, partitioned, payload, root, "partitioned MBR")


def main(editor: pathlib.Path, reader: pathlib.Path) -> None:
    with tempfile.TemporaryDirectory(prefix="iso-wcx-fat16-fat32-") as tmp:
        root = pathlib.Path(tmp)
        exercise("fat16", editor, reader, root)
        exercise("fat32", editor, reader, root)
        print("PASS: FAT16/FAT32 atomic add replace recursive delete VFAT, "
              "mirrors, FSInfo, backup boot, >65535 clusters, rollback")


if __name__ == "__main__":
    main(*(pathlib.Path(arg).resolve() for arg in sys.argv[1:3]))
