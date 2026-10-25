#!/usr/bin/env python3
"""Synthetic VHDX v1 native reader regression tests.

Creates clean base VHDX headers, paired region tables, metadata and payload BAT
entries independently of ISO-WCX. No mounts or writes to physical disks.
Also exercises FAT12 extraction from a VHDX-wrapped partitioned raw image.
"""
from __future__ import annotations

import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile

from test_virtual_fat import build_disk

MIB = 1024 * 1024
BLOCK = 2 * MIB
HEADER_A = 0x10000
HEADER_B = 0x20000
REGION_A = 0x30000
REGION_B = 0x40000
METADATA = 2 * MIB
BAT = 3 * MIB
PAYLOAD = 4 * MIB

GUID_BAT = bytes.fromhex("6677c22d23f600429d64115e9bfd4a08")
GUID_METADATA = bytes.fromhex("06a27c8b90479a4bb8fe575f050f886e")
GUID_PARAMS = bytes.fromhex("3767a1ca36fa434db3b633f0aa44e76b")
GUID_SIZE = bytes.fromhex("2442a52f1bcd7648b2115dbed83bf4b8")
GUID_SECTOR = bytes.fromhex("1dbf41816fa90947ba47f233a8faab5f")
GUID_PHYSICAL = bytes.fromhex("c748a3cd5d4471449cc9e9885251c556")
GUID_DISK = bytes.fromhex("ab12cabee6b2234593efc309e000c746")
GUID_PARENT = bytes.fromhex("2d5fd3a80bb34d45abf7d3d84834ab0c")


def crc32c(data: bytes) -> int:
    value = 0xFFFFFFFF
    for octet in data:
        value ^= octet
        for _ in range(8):
            value = (value >> 1) ^ (0x82F63B78 if value & 1 else 0)
    return value ^ 0xFFFFFFFF


def seal(buf: bytearray) -> None:
    buf[4:8] = b"\0" * 4
    struct.pack_into("<I", buf, 4, crc32c(buf))


def run(executable: pathlib.Path, *args: object, ok: bool = True) -> bytes:
    proc = subprocess.run([str(executable), *map(str, args)],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          timeout=45, check=False)
    if (proc.returncode == 0) != ok:
        raise AssertionError(
            f"{executable.name} {args!r}: code={proc.returncode}, "
            f"stderr={proc.stderr[-1300:]!r}, stdout={proc.stdout[-650:]!r}"
        )
    return proc.stdout


def make_header(seq: int = 2, dirty: bool = False,
                log_offset: int = MIB) -> bytearray:
    header = bytearray(4096)
    header[0:4] = b"head"
    struct.pack_into("<Q", header, 8, seq)
    header[16:32] = b"A" * 16  # FileWriteGuid
    header[32:48] = b"B" * 16  # DataWriteGuid
    if dirty:
        header[48:64] = b"L" * 16  # outstanding log GUID
    struct.pack_into("<H", header, 64, 0)  # LogVersion
    struct.pack_into("<H", header, 66, 1)  # Version
    struct.pack_into("<I", header, 68, MIB)
    struct.pack_into("<Q", header, 72, log_offset)
    seal(header)
    return header


def make_regions() -> bytearray:
    table = bytearray(65536)
    table[0:4] = b"regi"
    struct.pack_into("<I", table, 8, 2)
    for i, (id_bytes, offset) in enumerate((
        (GUID_METADATA, METADATA), (GUID_BAT, BAT)
    )):
        loc = 16 + i * 32
        table[loc:loc + 16] = id_bytes
        struct.pack_into("<QII", table, loc + 16, offset, MIB, 1)
    seal(table)
    return table


def make_metadata(logical_size: int, sector: int, fixed: bool) -> bytearray:
    content = bytearray(MIB)
    content[0:8] = b"metadata"
    struct.pack_into("<H", content, 10, 5)
    parameters = struct.pack("<II", BLOCK, 1 if fixed else 0)
    items = (
        (GUID_PARAMS, parameters, 4),
        (GUID_SIZE, struct.pack("<Q", logical_size), 6),
        (GUID_SECTOR, struct.pack("<I", sector), 6),
        (GUID_PHYSICAL, struct.pack("<I", sector), 6),
        (GUID_DISK, b"\x42" * 16, 6)
    )
    offset = 65536
    for i, (guid, data, flags) in enumerate(items):
        assert len(guid) == 16, "a VHDX metadata GUID must contain 16 bytes"
        at = 32 + 32 * i
        content[at:at + 16] = guid
        struct.pack_into("<III", content, at + 16,
                         offset, len(data), flags)
        content[offset:offset + len(data)] = data
        offset += len(data)
    return content


def make_image(raw: bytes, *, fixed: bool = True, sector: int = 512) -> bytearray:
    assert raw and len(raw) % sector == 0
    blocks = (len(raw) + BLOCK - 1) // BLOCK
    allocated = blocks if fixed else 1
    image = bytearray(PAYLOAD + allocated * BLOCK)
    image[0:8] = b"vhdxfile"
    image[HEADER_A:HEADER_A + 4096] = make_header(2)
    image[HEADER_B:HEADER_B + 4096] = make_header(1)
    table = make_regions()
    image[REGION_A:REGION_A + len(table)] = table
    image[REGION_B:REGION_B + len(table)] = table
    image[METADATA:METADATA + MIB] = make_metadata(len(raw), sector, fixed)
    chunk_ratio = (1 << 23) * sector // BLOCK
    for i in range(blocks):
        if not fixed and i:
            continue
        physical = PAYLOAD + i * BLOCK
        chunk = raw[i * BLOCK:(i + 1) * BLOCK]
        image[physical:physical + len(chunk)] = chunk
        bat_index = i + i // chunk_ratio
        struct.pack_into("<Q", image, BAT + bat_index * 8,
                         (physical // MIB) << 20 | 6)
    return image



def add_optional_region(image: bytearray, offset: int, *,
                        required: bool = False) -> None:
    """Add an unknown vendor-defined region with valid checksums to both tables."""
    guid = bytes.fromhex("00112233445566778899aabbccddeeff")
    for base in (REGION_A, REGION_B):
        table = bytearray(image[base:base + 65536])
        struct.pack_into("<I", table, 8, 3)
        at = 16 + 2 * 32
        table[at:at + 16] = guid
        struct.pack_into("<QII", table, at + 16, offset, MIB,
                         int(required))
        seal(table)
        image[base:base + len(table)] = table

def write(path: pathlib.Path, image: bytearray) -> None:
    path.write_bytes(image)


def main(partition_inspector: pathlib.Path,
         fat_inspector: pathlib.Path,
         reader_test: pathlib.Path) -> None:
    with tempfile.TemporaryDirectory(prefix="iso-wcx-vhdx-") as temp:
        root = pathlib.Path(temp)
        raw_path = root / "fat12.img"
        expected_files = build_disk(raw_path, "fat12")
        raw_bytes = raw_path.read_bytes()
        fixed_path = root / "fixed.vhdx"
        dynamic_path = root / "dynamic.vhdx"
        corrupted_path = root / "bad.vhdx"

        fixed = make_image(raw_bytes, fixed=True)
        dynamic = make_image(raw_bytes, fixed=False)
        write(fixed_path, fixed)
        write(dynamic_path, dynamic)

        for image in (fixed_path, dynamic_path):
            listing = run(partition_inspector, image).decode("utf-8")
            assert "VHDX (native read-only)" in listing, listing
            assert "\t2048\t2880\t0x01" in listing, listing
            fat_files = run(fat_inspector, "list", image, 1).decode("utf-8")
            assert "README.TXT" in fat_files, fat_files
            assert "Long Filename.txt" in fat_files, fat_files
            assert "TESTDIR/NESTED.TXT" in fat_files, fat_files
            for name, content in expected_files.items():
                assert run(fat_inspector, "cat", image, 1, name) == content

        # Optional, non-overlapping vendor regions are legal; protect their
        # physical extents against malicious BAT payload mappings.
        vendor = bytearray(fixed)
        vendor_start = len(vendor)
        assert vendor_start % MIB == 0
        vendor.extend(b"\0" * MIB)
        add_optional_region(vendor, vendor_start)
        vendor_path = root / "vendor-region.vhdx"
        write(vendor_path, vendor)
        assert "VHDX" in run(partition_inspector, vendor_path).decode("utf-8")
        for name, content in expected_files.items():
            assert run(fat_inspector, "cat", vendor_path, 1, name) == content

        # Forensic BAT alias regression: two independently mapped logical
        # payload blocks claim one physical block. Raw data is all zeros so
        # merely reading the wrong block would incorrectly look successful.
        twin = make_image(bytes(4 * MIB), fixed=True)
        struct.pack_into("<Q", twin, BAT + 8, (PAYLOAD // MIB) << 20 | 6)
        duplicate_bat = root / "aliased-payload.vhdx"
        write(duplicate_bat, twin)
        run(reader_test, duplicate_bat, BLOCK - 128, 256, 0, ok=False)
        twin_good = root / "unaliased-payload.vhdx"
        write(twin_good, make_image(bytes(4 * MIB), fixed=True))
        run(reader_test, twin_good, BLOCK - 128, 256, 0)

        # Fully allocated block zero, sparse/unmapped/zero second block.
        run(reader_test, dynamic_path, 3 * MIB, 4096, 0)
        run(reader_test, fixed_path, len(raw_bytes) - 32, 32, 0)
        run(reader_test, fixed_path, len(raw_bytes), 1, 0, ok=False)
        run(fat_inspector, "list", fixed_path, 1, "4096", ok=False)

        # If primary header fails CRC, the second valid copy is used.
        one_header_bad = bytearray(fixed)
        one_header_bad[HEADER_A + 36] ^= 1
        write(corrupted_path, one_header_bad)
        assert "VHDX" in run(partition_inspector, corrupted_path).decode("utf-8")

        invalid_samples = {}
        changed = bytearray(fixed)
        changed[0:8] = b"NOT-VHDX"
        invalid_samples["signature"] = changed
        changed = bytearray(fixed)
        changed[HEADER_A + 36] ^= 1
        changed[HEADER_B + 36] ^= 1
        invalid_samples["both-header-crc"] = changed
        changed = bytearray(fixed)
        changed[REGION_A + 100] ^= 1
        changed[REGION_B + 100] ^= 1
        invalid_samples["both-region-crc"] = changed
        changed = bytearray(fixed)
        changed[METADATA + 65536 + 4] = 2  # HasParent
        invalid_samples["has-parent"] = changed
        changed = bytearray(fixed)
        struct.pack_into("<Q", changed, BAT, 7)  # Partially present
        invalid_samples["partial-block"] = changed
        changed = bytearray(fixed)
        struct.pack_into("<Q", changed, BAT, 6 | 8)  # Reserved BAT bit
        invalid_samples["reserved-bat"] = changed
        changed = bytearray(fixed)
        struct.pack_into("<Q", changed, BAT, (999 << 20) | 6)
        invalid_samples["out-of-bounds-payload"] = changed
        changed = bytearray(fixed)
        header = make_header(3, dirty=True)
        changed[HEADER_A:HEADER_A + len(header)] = header
        invalid_samples["dirty-log"] = changed
        changed = bytearray(fixed)
        changed[HEADER_A:HEADER_A + 4096] = make_header(2, log_offset=PAYLOAD)
        changed[HEADER_B:HEADER_B + 4096] = make_header(1, log_offset=PAYLOAD)
        invalid_samples["payload-overlaps-log"] = changed
        changed = bytearray(fixed)
        changed[METADATA + 32:METADATA + 48] = b"X" * 16
        invalid_samples["unknown-required-metadata"] = changed
        changed = bytearray(fixed)
        add_optional_region(changed, METADATA)  # Overlaps known Metadata region.
        invalid_samples["overlapping-optional-region"] = changed
        changed = bytearray(fixed)
        add_optional_region(changed, BAT, required=True)
        invalid_samples["unknown-required-region"] = changed
        changed = bytearray(fixed)
        add_optional_region(changed, PAYLOAD)  # Aliases present BAT payload.
        invalid_samples["optional-overlaps-payload"] = changed
        changed = bytearray(fixed)
        struct.pack_into("<I", changed, METADATA + 32 + 3 * 32 + 16, 65536)
        invalid_samples["overlapping-metadata-item"] = changed
        changed = bytearray(fixed)
        for at in (HEADER_A, HEADER_B):
            header = bytearray(changed[at:at + 4096])
            struct.pack_into("<H", header, 64, 1)  # Invalid log version.
            seal(header)
            changed[at:at + len(header)] = header
        invalid_samples["invalid-log-version"] = changed
        changed = bytearray(fixed)
        for at in (HEADER_A, HEADER_B):
            header = bytearray(changed[at:at + 4096])
            header[80] = 1  # Reserved header byte.
            seal(header)
            changed[at:at + len(header)] = header
        invalid_samples["reserved-header-byte"] = changed
        changed = bytearray(fixed)
        for at in (REGION_A, REGION_B):
            table = bytearray(changed[at:at + 65536])
            struct.pack_into("<I", table, 12, 1)
            seal(table)
            changed[at:at + len(table)] = table
        invalid_samples["reserved-region-header"] = changed
        changed = bytearray(fixed)
        changed[METADATA + 12] = 1
        invalid_samples["reserved-metadata-header"] = changed


        for name, sample in invalid_samples.items():
            write(corrupted_path, sample)
            # BAT damage may be discovered only when a mapped block is read.
            if name in ("partial-block", "reserved-bat", "out-of-bounds-payload",
                        "payload-overlaps-log", "optional-overlaps-payload"):
                run(reader_test, corrupted_path, 0, 512, 0, ok=False)
            else:
                run(partition_inspector, corrupted_path, ok=False)

        # With 4096-byte logical sectors, MBR partitions must use 4096-byte
        # addressing and the inspector must pick this from metadata.
        raw_4k = bytearray(8 * MIB)
        raw_4k[510:512] = b"\x55\xAA"
        raw_4k[446 + 4] = 0x07
        struct.pack_into("<II", raw_4k, 446 + 8, 256, 256)
        four_k = root / "4k.vhdx"
        write(four_k, make_image(raw_4k, fixed=False, sector=4096))
        output = run(partition_inspector, four_k).decode("utf-8")
        assert "\t256\t256\t0x07" in output, output
        run(partition_inspector, four_k, "512", ok=False)

        renamed = root / "mislabelled.img"
        shutil.copyfile(fixed_path, renamed)
        run(partition_inspector, renamed, ok=False)

    print("PASS: VHDX metadata, CRC32C fallback, BAT, FAT12 extraction, "
          "512/4096 sectors, sparse/invalid blocks and corrupt-image rejection")


if __name__ == "__main__":
    main(*(pathlib.Path(argument).resolve() for argument in sys.argv[1:4]))
