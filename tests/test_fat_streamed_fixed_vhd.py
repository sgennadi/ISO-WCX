#!/usr/bin/env python3
"""Native streamed FAT staging and partitioned fixed VHD write regression.

No disk mount, external formatter, helper EXE or pytsk3. Construct FAT32
>256 MiB as a sparse source and write through the built-in transactional
WCX FAT engine; confirm Unicode round trip, mirrors, boot+FSInfo, rollback.
Also test MBR partition1/2 writes on a fixed VHD with a checksumed footer.
"""
from __future__ import annotations

import pathlib
import os
import stat
import struct
import subprocess
import sys
import tempfile

from test_virtual_fat import (SECTOR, PARTITION_LBA, FAT_CONFIG,
                              build_disk, make_fixed_vhd, fat_set)
from test_fat16_fat32_edit import make_fsinfo
from test_floppy_edit import run, readback


def long_fat32(path: pathlib.Path) -> int:
    total = 600000  # 292.97 MiB: exceeds the old 256-MiB image limitation.
    reserved, fats, fat_sectors, spc = 32, 2, 1200, 4
    system = reserved + fats * fat_sectors
    count = (total - system) // spc
    assert 65525 <= count < (fat_sectors * SECTOR // 4) - 2
    boot = bytearray(SECTOR)
    boot[:3] = b"\xeb\x58\x90"
    boot[3:11] = b"MSDOS5.0"
    struct.pack_into("<H", boot, 11, SECTOR)
    boot[13] = spc
    struct.pack_into("<H", boot, 14, reserved)
    boot[16] = fats
    boot[21] = 0xf8
    struct.pack_into("<I", boot, 32, total)
    struct.pack_into("<I", boot, 36, fat_sectors)
    struct.pack_into("<I", boot, 44, 2)  # root directory first cluster
    struct.pack_into("<H", boot, 48, 1)  # FSInfo primary
    struct.pack_into("<H", boot, 50, 6)  # backup boot
    boot[510:512] = b"\x55\xaa"

    table = bytearray(fat_sectors * SECTOR)
    fat_set(table, "fat32", 0, 0x0ffffff8)
    fat_set(table, "fat32", 1, 0x0fffffff)
    fat_set(table, "fat32", 2, 0x0fffffff)
    size = total * SECTOR
    with path.open("wb") as f:
        f.truncate(size)
        f.seek(0)
        f.write(boot)
        f.seek(SECTOR)
        f.write(make_fsinfo())
        f.seek(6 * SECTOR)
        f.write(boot)
        f.seek(7 * SECTOR)
        f.write(make_fsinfo())
        for copy in range(fats):
            f.seek((reserved + copy * fat_sectors) * SECTOR)
            f.write(table)
    return size


def verify_fat32(image: pathlib.Path, original_boot: bytes, size: int):
    assert image.stat().st_size == size
    with image.open("rb") as f:
        assert f.read(SECTOR) == original_boot
        f.seek(6 * SECTOR)
        assert f.read(SECTOR) == original_boot
        f.seek(SECTOR)
        info = f.read(SECTOR)
        f.seek(7 * SECTOR)
        assert f.read(SECTOR) == info
        assert int.from_bytes(info[488:492], "little") < 200000
        f.seek(32 * SECTOR)
        fat0 = f.read(1200 * SECTOR)
        f.seek((32 + 1200) * SECTOR)
        assert f.read(1200 * SECTOR) == fat0, "mirrored FAT32 tables diverged"


def test_large(editor: pathlib.Path, reader: pathlib.Path, root: pathlib.Path):
    image = root / "large-fat32.raw"
    size = long_fat32(image)
    if os.name != "nt":
        image.chmod(0o640)  # The atomic clone must not silently become 0600.
    with image.open("rb") as f:
        original_boot = f.read(SECTOR)
    payload = root / "large-input.bin"
    payload.write_bytes(b"mapped image \xe2\x9c\x93" * 257)
    logical = "Large FAT32 Привет שלום 📁.txt"
    run(editor, image, "add", logical, payload)
    readback(reader, image, {logical: payload.read_bytes()},
             root / "large-extracted")
    verify_fat32(image, original_boot, size)
    if os.name != "nt":
        assert stat.S_IMODE(image.stat().st_mode) == 0o640,             "atomic FAT update changed original POSIX access permissions"

    # Fail closed for invalid FAT mirror even when a stage would be large.
    corrupt = root / "large-mirror-broken.raw"
    with image.open("rb") as source, corrupt.open("wb") as dest:
        while chunk := source.read(1024 * 1024):
            dest.write(chunk)
    with corrupt.open("r+b") as f:
        f.seek((32 + 1200) * SECTOR + 25)
        f.write(b"\xff")
    first = corrupt.stat().st_size
    run(editor, corrupt, "add", "BAD.TXT", payload, ok=False)
    assert corrupt.stat().st_size == first
    assert not list(root.rglob("*.iso-wcx-fat-*.tmp*")), \
        "orphaned mapped stage remained after failed write"


def extend_two_partitions(raw: pathlib.Path, kind: str):
    total, _, _, _, ptype = FAT_CONFIG[kind]
    second = PARTITION_LBA + total + 2048
    first = PARTITION_LBA * SECTOR
    with raw.open("r+b") as f:
        f.seek(first)
        volume = f.read(total * SECTOR)  # FAT16 8 MiB: small test fixture.
        f.truncate((second + total + 2048) * SECTOR)
        f.seek(second * SECTOR)
        f.write(volume)
        # This read-only FAT16 fixture intentionally uses 0xFFFF in entry 0.
        # The transactional writer requires the media descriptor (0xF8) in
        # the low byte, as specified by FAT16: normalize only partition #2.
        f.seek((second + FAT_CONFIG["fat16"][1]) * SECTOR)
        f.write(b"\xf8\xff")
        f.seek(462 + 4)
        f.write(bytes([ptype]))
        f.seek(462 + 8)
        f.write(second.to_bytes(4, "little"))
        f.write(total.to_bytes(4, "little"))
    return second, total


def test_fixed_vhd(editor: pathlib.Path, reader: pathlib.Path, root: pathlib.Path):
    original = root / "two-partitions.img"
    messages = build_disk(original, "fat16")
    second, total = extend_two_partitions(original, "fat16")
    vhd = root / "two-partitions.vhd"
    make_fixed_vhd(original, vhd)
    # Snapshot *all* bytes outside partition2, not just the MBR/footer.
    # Even a single bit altered in partition1, gaps or VHD metadata fails.
    original_disk = vhd.read_bytes()
    with vhd.open("rb") as f:
        mbr = f.read(SECTOR)
        f.seek(-512, 2)
        footer = f.read(SECTOR)
    data = root / "vhd-data.bin"
    data.write_bytes(b"hello from FAT16 fixed VHD partition 2")
    logical = "partition2/Новый файл שלום.txt"
    run(editor, vhd, "add", logical, data)
    readback(reader, vhd, {logical: data.read_bytes(),
                          "partition1/README.TXT": messages["README.TXT"]},
             root / "fixed-vhd-output")
    with vhd.open("rb") as f:
        assert f.read(SECTOR) == mbr
        f.seek(-512, 2)
        assert f.read(SECTOR) == footer
    edited_disk = vhd.read_bytes()
    start = second * SECTOR
    end = (second + total) * SECTOR
    assert len(edited_disk) == len(original_disk)
    assert edited_disk[:start] == original_disk[:start], (
        "modification escaped partition2 into MBR, partition1 or alignment gap")
    assert edited_disk[end:] == original_disk[end:], (
        "modification escaped partition2 into trailing sectors or VHD footer")
    assert edited_disk[start:end] != original_disk[start:end], (
        "FAT partition2 was not updated")
    before = vhd.stat().st_size
    run(editor, vhd, "add", "README.TXT", data, ok=False)
    run(editor, vhd, "add", "partition3/FAIL.TXT", data, ok=False)
    assert vhd.stat().st_size == before
    assert not list(root.rglob("*.iso-wcx-fat-*.tmp*"))

    # Never allow one transaction to span two independently mapped volumes.
    run(editor, vhd, "batch", "add", "partition1/ONE.TXT", data,
        "add", "partition2/TWO.TXT", data, ok=False)
    assert "TWO.TXT" not in run(reader, vhd).stdout.decode("utf-8")
    assert not list(root.rglob("*.iso-wcx-fat-*.tmp*"))


def main(editor: pathlib.Path, reader: pathlib.Path):
    with tempfile.TemporaryDirectory(prefix="iso-wcx-stream-fat-") as d:
        root = pathlib.Path(d)
        test_large(editor, reader, root)
        test_fixed_vhd(editor, reader, root)
        print("PASS: large FAT32 mapped staging, unicode VFAT, FAT32 metadata, "
              "fixed VHD MBR partition2 edits, footer identity and rollback")


if __name__ == "__main__":
    main(*(pathlib.Path(x).resolve() for x in sys.argv[1:3]))
