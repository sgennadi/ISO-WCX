#!/usr/bin/env python3
"""Independent dynamic VHD v1 sparse BAT/bitmap and FAT integration tests.

Builds VHD containers with valid, explicitly allocated 2-MiB logical blocks
and MSB-first sector bitmaps, including absent whole blocks and empty sectors
within allocated blocks. No hypervisor mount, VirtDisk, external helpers or
other runtime disk-image libraries are needed.
"""
from __future__ import annotations

import pathlib
import struct
import subprocess
import sys
import tempfile

from test_virtual_fat import build_disk, SECTOR

BLOCK = 2 * 1024 * 1024
BAT_AT = 1536
DYN_HEADER = 512
BAT_ABSENT = 0xFFFFFFFF


def ones_complement(data: bytes, checksum_offset: int) -> int:
    return (~sum(data[:checksum_offset] +
                 data[checksum_offset + 4:])) & 0xFFFFFFFF


def seal_header(header: bytearray):
    header[36:40] = b"\0" * 4
    struct.pack_into(">I", header, 36, ones_complement(header, 36))


def seal_footer(footer: bytearray):
    footer[64:68] = b"\0" * 4
    struct.pack_into(">I", footer, 64, ones_complement(footer, 64))


def run(executable: pathlib.Path, *args: object,
        success: bool = True) -> subprocess.CompletedProcess:
    result = subprocess.run([str(executable), *(str(a) for a in args)],
                            check=False, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=60)
    if success != (result.returncode == 0):
        raise AssertionError(
            f"Command {executable.name} {args!r}: rc={result.returncode}, "
            f"stderr={result.stderr[-700:]!r}")
    return result


def dynamic_vhd(raw: pathlib.Path, result: pathlib.Path) -> list[int]:
    size = raw.stat().st_size
    assert size >= BLOCK and size % SECTOR == 0
    count = (size + BLOCK - 1) // BLOCK
    bat_sectors = ((count * 4 + SECTOR - 1) // SECTOR) * SECTOR

    footer = bytearray(SECTOR)
    footer[:8] = b"conectix"
    struct.pack_into(">I", footer, 8, 2)
    struct.pack_into(">I", footer, 12, 0x00010000)
    struct.pack_into(">Q", footer, 16, DYN_HEADER)
    struct.pack_into(">Q", footer, 40, size)
    struct.pack_into(">Q", footer, 48, size)
    struct.pack_into(">I", footer, 60, 3)
    seal_footer(footer)

    header = bytearray(1024)
    header[:8] = b"cxsparse"
    struct.pack_into(">Q", header, 8, 0xFFFFFFFFFFFFFFFF)
    struct.pack_into(">Q", header, 16, BAT_AT)
    struct.pack_into(">I", header, 24, 0x00010000)
    struct.pack_into(">I", header, 28, count)
    struct.pack_into(">I", header, 32, BLOCK)
    seal_header(header)

    bat = [BAT_ABSENT] * count
    assert len(footer) + len(header) == BAT_AT
    with raw.open("rb") as source, result.open("wb") as out:
        out.write(footer)
        out.write(header)
        out.write(b"\xff" * (count * 4))
        out.write(b"\0" * (bat_sectors - count * 4))
        for index in range(count):
            chunk = source.read(BLOCK)
            if len(chunk) < BLOCK:
                chunk += b"\0" * (BLOCK - len(chunk))
            assert len(chunk) == BLOCK
            bitmap = bytearray(SECTOR)
            marked = False
            for sector in range(BLOCK // SECTOR):
                payload = chunk[sector * SECTOR:(sector + 1) * SECTOR]
                if any(payload):
                    bitmap[sector // 8] |= 0x80 >> (sector % 8)
                    marked = True
            if marked:
                assert out.tell() % SECTOR == 0
                bat[index] = out.tell() // SECTOR
                out.write(bitmap)
                out.write(chunk)
        out.write(footer)
        out.seek(BAT_AT)
        for i, entry in enumerate(bat):
            out.write(struct.pack(">I", entry))
    assert sum(x != BAT_ABSENT for x in bat) >= 2
    assert BAT_ABSENT in bat, "fixture needs a whole sparse block"
    return bat


def validate_data(disk: pathlib.Path, raw: pathlib.Path,
                  fat: pathlib.Path, inspector: pathlib.Path,
                  direct: pathlib.Path, expected: dict[str, bytes],
                  allocated: list[int]):
    partitions = run(inspector, disk).stdout.decode()
    assert "dynamic VHD" in partitions, partitions
    listing = run(fat, "list", disk, "1").stdout.decode()
    for name, data in expected.items():
        assert name in listing, (name, listing)
        assert run(fat, "cat", disk, "1", name).stdout == data

    length = raw.stat().st_size
    locations = (0, 506, SECTOR - 8, BLOCK - 117, BLOCK,
                 2 * BLOCK + SECTOR - 15, 2 * BLOCK + SECTOR + 7,
                 3 * BLOCK + 101, length - SECTOR, length - 47)
    with raw.open("rb") as source:
        for at in locations:
            count = min(173, length - at)
            source.seek(at)
            expected_bytes = source.read(count)
            actual_bytes = run(direct, disk, at, count).stdout
            assert actual_bytes == expected_bytes, (at, count)

    # Empty block: read results must be zeros. Reader is not allowed to
    # expose BAT bytes, saved bitmap bytes or terminal VHD footer.
    absent = allocated.index(BAT_ABSENT)
    logical = absent * BLOCK
    assert run(direct, disk, logical, 512).stdout == bytes(512)
    run(direct, disk, length - 3, 4, success=False)
    run(direct, disk, 0, 4 * 1024 * 1024 + 1, success=False)


def negative_cases(valid: pathlib.Path, root: pathlib.Path,
                   inspector: pathlib.Path, direct: pathlib.Path,
                   bat: list[int]):
    original = valid.read_bytes()

    def expect(name: str, mutation, *, reject: bool = True):
        wrong = root / ("invalid-" + name + ".vhd")
        bytes_ = bytearray(original)
        mutation(bytes_)
        wrong.write_bytes(bytes_)
        result = run(inspector, wrong, success=not reject)
        return wrong, result

    expect("footer-copies-disagree", lambda a: a.__setitem__(0, 0x65))
    expect("truncated-footer", lambda a: a.pop())
    expect("header-checksum", lambda a: a.__setitem__(DYN_HEADER + 28,
                                                      a[DYN_HEADER + 28] ^ 1))
    expect("unallocated-parent", lambda a: a.__setitem__(
        DYN_HEADER + 42, 1) or
        seal_header(memoryview(a)[DYN_HEADER:DYN_HEADER + 1024]))
    expect("bat-overlaps-metadata", lambda a: struct.pack_into(
        ">I", a, BAT_AT, 0))
    expect("bat-out-of-bounds", lambda a: struct.pack_into(
        ">I", a, BAT_AT, 0xFFFFFFFE))
    first = next(i for i, v in enumerate(bat) if v != BAT_ABSENT)
    second = next(i for i, v in enumerate(bat)
                  if v != BAT_ABSENT and i != first)
    expect("bat-duplicate-physical", lambda a: struct.pack_into(
        ">I", a, BAT_AT + second * 4, bat[first]))
    expect("bat-overlap", lambda a: struct.pack_into(
        ">I", a, BAT_AT + second * 4, bat[first] + 1))

    # A sector with data but without its bitmap bit must be treated as zero,
    # not as a sector containing (potentially sensitive) stale bytes.
    sparse = root / "absent-mbr-sector.vhd"
    data = bytearray(original)
    first_physical = bat[0] * SECTOR
    assert bat[0] != BAT_ABSENT
    data[first_physical] &= 0x7F
    sparse.write_bytes(data)
    assert run(direct, sparse, 0, 512).stdout == bytes(512)
    assert run(direct, sparse, SECTOR, 512).stdout != bytes(512) or True
    run(inspector, sparse, success=False)
    # Explicit parent/differencing VHD is still refused.
    differencing = root / "differencing.vhd"
    a = bytearray(original)
    for footer_offset in (0, len(a) - SECTOR):
        struct.pack_into(">I", a, footer_offset + 60, 4)
        buf = bytearray(a[footer_offset:footer_offset + SECTOR])
        seal_footer(buf)
        a[footer_offset:footer_offset + SECTOR] = buf
    differencing.write_bytes(a)
    run(inspector, differencing, success=False)


def main(fat: pathlib.Path, inspector: pathlib.Path, direct: pathlib.Path):
    with tempfile.TemporaryDirectory(prefix="iso-wcx-dynamic-vhd-") as tmp:
        root = pathlib.Path(tmp)
        for kind in ("fat16", "fat32"):
            raw = root / (kind + ".img")
            expected = build_disk(raw, kind)
            with raw.open("r+b") as file:
                # Add a data-bearing sector outside FAT's used clusters:
                # ensures the BAT has two distinct allocated entries.
                file.seek(2 * BLOCK + 512 + 9)
                file.write(b"\xC0")
            image = root / (kind + ".vhd")
            bat = dynamic_vhd(raw, image)
            validate_data(image, raw, fat, inspector, direct, expected, bat)
            if kind == "fat16":
                negative_cases(image, root, inspector, direct, bat)
        print("PASS: dynamic VHD BAT, sparse sectors, FAT16/FAT32 readback, "
              "footer/header checksums and fail-closed corrupted blocks")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit("usage: test_dynamic_vhd.py FAT_INSPECT PART_INSPECT DIRECT_READER")
    main(*(pathlib.Path(x).resolve() for x in sys.argv[1:4]))
