#!/usr/bin/env python3
"""Independent synthetic FAT12/16/32 HDD and fixed-VHD integration tests.

Tests read-only partition browsing and binary extraction with the native
virtual_fat_inspect executable. No disk mounting or host device access.
"""
from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile

SECTOR = 512
PARTITION_LBA = 2048

FAT_CONFIG = {
    "fat12": (2880, 1, 9, 32, 0x01),
    "fat16": (16384, 1, 64, 32, 0x06),
    "fat32": (69632, 32, 600, 0, 0x0C),
}


def short_entry(name: bytes, attr: int, cluster: int, size: int = 0) -> bytes:
    assert len(name) == 11
    b = bytearray(32)
    b[:11] = name
    b[11] = attr
    b[20:22] = ((cluster >> 16) & 0xFFFF).to_bytes(2, "little")
    b[26:28] = (cluster & 0xFFFF).to_bytes(2, "little")
    b[28:32] = size.to_bytes(4, "little")
    return bytes(b)


def checksum(name: bytes) -> int:
    result = 0
    for c in name:
        result = (((result & 1) << 7) + (result >> 1) + c) & 0xFF
    return result


def vfat_entries(name: str, short: bytes, bad_checksum: bool = False) -> bytes:
    utf16 = name.encode("utf-16le")
    units = [int.from_bytes(utf16[i:i + 2], "little")
             for i in range(0, len(utf16), 2)]
    count = (len(units) + 12) // 13
    assert 1 <= count <= 20
    value = (checksum(short) + (1 if bad_checksum else 0)) & 0xFF
    entries = []
    for number in range(count, 0, -1):
        component = units[(number - 1) * 13:number * 13]
        if len(component) < 13:
            component = component + [0] + [0xFFFF] * (12 - len(component))
        e = bytearray(32)
        e[0] = number | (0x40 if number == count else 0)
        e[11] = 0x0F
        e[13] = value
        for position, char in zip(
            list(range(1, 11, 2)) + list(range(14, 26, 2)) +
            list(range(28, 32, 2)), component
        ):
            e[position:position + 2] = char.to_bytes(2, "little")
        entries.append(bytes(e))
    return b"".join(entries)


def fat_set(table: bytearray, kind: str, cluster: int, value: int) -> None:
    if kind == "fat12":
        at = cluster * 3 // 2
        if cluster & 1:
            table[at] = (table[at] & 0x0F) | ((value & 0x0F) << 4)
            table[at + 1] = (value >> 4) & 0xFF
        else:
            table[at] = value & 0xFF
            table[at + 1] = (table[at + 1] & 0xF0) | ((value >> 8) & 0x0F)
    elif kind == "fat16":
        table[cluster * 2:cluster * 2 + 2] = value.to_bytes(2, "little")
    else:
        table[cluster * 4:cluster * 4 + 4] = value.to_bytes(4, "little")


def place(file, at: int, data: bytes) -> None:
    file.seek(at)
    file.write(data)


def build_disk(path: pathlib.Path, kind: str, *,
               bad_name=False, chain_error=False, directory_cycle=False) -> dict[str, bytes]:
    total, reserved, fat_sectors, root_entries, part_type = FAT_CONFIG[kind]
    root_sectors = (root_entries * 32 + 511) // 512
    first_data_sector = reserved + fat_sectors + root_sectors
    disk_size = (PARTITION_LBA + total + 2048) * SECTOR
    root = 6 if kind == "fat32" else 0
    messages = {
        "README.TXT": ("Hello from " + kind + "!\n").encode(),
        "Long Filename.txt": ("VFAT-" + kind).encode(),
        "TESTDIR/NESTED.TXT": ("nested-" + kind).encode(),
    }

    boot = bytearray(512)
    boot[:3] = b"\xEB\x58\x90" if kind == "fat32" else b"\xEB\x3C\x90"
    boot[3:11] = b"MSDOS5.0"
    boot[11:13] = (SECTOR).to_bytes(2, "little")
    boot[13] = 1
    boot[14:16] = reserved.to_bytes(2, "little")
    boot[16] = 1
    boot[17:19] = root_entries.to_bytes(2, "little")
    if total <= 65535:
        boot[19:21] = total.to_bytes(2, "little")
    else:
        boot[32:36] = total.to_bytes(4, "little")
    boot[21] = 0xF8
    if kind != "fat32":
        boot[22:24] = fat_sectors.to_bytes(2, "little")
    else:
        boot[36:40] = fat_sectors.to_bytes(4, "little")
        boot[44:48] = root.to_bytes(4, "little")
    boot[510:512] = b"\x55\xAA"

    fat = bytearray(fat_sectors * SECTOR)
    end = {"fat12": 0xFFF, "fat16": 0xFFFF, "fat32": 0x0FFFFFFF}[kind]
    fat_set(fat, kind, 0, end)
    fat_set(fat, kind, 1, end)
    for i in (2, 3, 4, 5, 6):
        fat_set(fat, kind, i, end)
    if chain_error:
        fat_set(fat, kind, 3, 3)
    if directory_cycle:
        fat_set(fat, kind, 2, 2)

    short_lfn = b"LONGNA~1TXT"
    root_directory = b"".join((
        short_entry(b"TESTDIR    ", 0x10, 2),
        short_entry(b"README  TXT", 0x20, 3,
                    600 if chain_error else len(messages["README.TXT"])),
        vfat_entries("Long Filename.txt", short_lfn, bad_checksum=bad_name),
        short_entry(short_lfn, 0x20, 4, len(messages["Long Filename.txt"])),
    ))
    nested = b"".join((
        short_entry(b".          ", 0x10, 2),
        short_entry(b"..         ", 0x10, root),
        short_entry(b"NESTED  TXT", 0x20, 5, len(messages["TESTDIR/NESTED.TXT"])),
    ))
    mbr = bytearray(512)
    mbr[446 + 4] = part_type
    mbr[446 + 8:446 + 12] = PARTITION_LBA.to_bytes(4, "little")
    mbr[446 + 12:446 + 16] = total.to_bytes(4, "little")
    mbr[510:512] = b"\x55\xAA"
    part_offset = PARTITION_LBA * SECTOR

    def cluster_offset(number: int) -> int:
        return part_offset + (first_data_sector + number - 2) * SECTOR

    with path.open("wb") as image:
        image.truncate(disk_size)
        place(image, 0, mbr)
        place(image, part_offset, boot)
        place(image, part_offset + reserved * SECTOR, fat)
        if root:
            place(image, cluster_offset(root), root_directory)
        else:
            place(image, part_offset + (reserved + fat_sectors) * SECTOR,
                  root_directory)
        place(image, cluster_offset(2), nested)
        place(image, cluster_offset(3), messages["README.TXT"])
        place(image, cluster_offset(4), messages["Long Filename.txt"])
        place(image, cluster_offset(5), messages["TESTDIR/NESTED.TXT"])
    return messages


def make_fixed_vhd(raw: pathlib.Path, target: pathlib.Path) -> None:
    data_size = raw.stat().st_size
    footer = bytearray(512)
    footer[:8] = b"conectix"
    footer[8:12] = (2).to_bytes(4, "big")
    footer[12:16] = (0x00010000).to_bytes(4, "big")
    footer[16:24] = (0xFFFFFFFFFFFFFFFF).to_bytes(8, "big")
    footer[40:48] = data_size.to_bytes(8, "big")
    footer[48:56] = data_size.to_bytes(8, "big")
    footer[60:64] = (2).to_bytes(4, "big")
    footer[64:68] = ((~sum(footer)) & 0xFFFFFFFF).to_bytes(4, "big")
    with raw.open("rb") as src, target.open("wb") as dst:
        while block := src.read(1024 * 1024):
            dst.write(block)
        dst.write(footer)


def run(executable: pathlib.Path, *args: str, success=True) -> subprocess.CompletedProcess:
    process = subprocess.run([str(executable), *map(str, args)],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             timeout=45, check=False)
    if success != (process.returncode == 0):
        raise AssertionError(
            f"Exit {process.returncode}, success={success}, args={args!r}, "
            f"stderr={process.stderr[-500:]!r}")
    return process


def main(executable: pathlib.Path) -> None:
    with tempfile.TemporaryDirectory(prefix="iso-wcx-fat-") as directory:
        root = pathlib.Path(directory)
        for kind in ("fat12", "fat16", "fat32"):
            raw = root / (kind + ".img")
            messages = build_disk(raw, kind)
            listing = run(executable, "list", raw, "1").stdout.decode("utf-8")
            for name, expected in messages.items():
                assert name in listing, (kind, name, listing)
                assert run(executable, "cat", raw, "1", name).stdout == expected
            assert "D\t0\tTESTDIR" in listing
            run(executable, "cat", raw, "1", "TESTDIR", success=False)
            run(executable, "cat", raw, "1", "../README.TXT", success=False)
            run(executable, "list", raw, "2", success=False)

            bad = root / ("corrupt-" + kind + ".img")
            build_disk(bad, kind, directory_cycle=True)
            run(executable, "list", bad, "1", success=False)
            build_disk(bad, kind, chain_error=True)
            run(executable, "cat", bad, "1", "README.TXT", success=False)
            build_disk(bad, kind, bad_name=True)
            out = run(executable, "list", bad, "1").stdout.decode("utf-8")
            assert "Long Filename.txt" not in out and "LONGNA~1.TXT" in out, out
            run(executable, "cat", bad, "1", "Long Filename.txt", success=False)

            if kind == "fat16":
                vhd = root / "fat16.vhd"
                make_fixed_vhd(raw, vhd)
                assert run(executable, "cat", vhd, "1", "README.TXT").stdout == messages["README.TXT"]
                assert "TESTDIR/NESTED.TXT" in run(
                    executable, "list", vhd, "1").stdout.decode("utf-8")

        print("PASS FAT12/16/32 list, nested/LFN, binary extract, fixed VHD, malformed FAT chains")


if __name__ == "__main__":
    main(pathlib.Path(sys.argv[1]).resolve())
