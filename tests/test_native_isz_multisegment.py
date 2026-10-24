#!/usr/bin/env python3
"""Independent native multi-part ISZ optical read regressions.

Synthetic split payloads exercise in-process cross-segment random seeks,
the documented 48/64-byte headers, 2/3/4-byte pointer tables and strict
rejection of missing/wrong/corrupt parts. The Windows lane additionally
tests the real WCX ABI; end users need no helper or image mount.
"""
from __future__ import annotations

import bz2
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib

import test_images

SECTOR = 2048
MASK = b"\xb6\x8c\xa5\xde"


def xor_table(data: bytes) -> bytes:
    return bytes(byte ^ MASK[i % len(MASK)] for i, byte in enumerate(data))


def header(raw: bytes, *, length: int, width: int, count: int,
           chunk_table: int, segment_table: int, data_offset: int,
           part_number: int = 0, serial: int = 0x27364859) -> bytearray:
    assert length in (48, 64) and len(raw) % SECTOR == 0
    result = bytearray(length)
    result[:4] = b"IsZ!"
    result[4] = length
    result[5] = 1
    struct.pack_into("<I", result, 6, serial)
    struct.pack_into("<H", result, 10, SECTOR)
    struct.pack_into("<I", result, 12, len(raw) // SECTOR)
    struct.pack_into("<I", result, 25, count)
    struct.pack_into("<I", result, 29, SECTOR)
    result[33] = width
    result[34] = part_number
    struct.pack_into("<I", result, 35, chunk_table)
    struct.pack_into("<I", result, 39, segment_table)
    struct.pack_into("<I", result, 43, data_offset)
    return result


def encode(raw: bytes, width: int) -> tuple[bytes, bytes, list[tuple[int, int]]]:
    assert width in (2, 3, 4) and len(raw) % SECTOR == 0
    indexes = bytearray()
    payload = bytearray()
    ranges: list[tuple[int, int]] = []
    for sector in range(len(raw) // SECTOR):
        data = raw[sector * SECTOR:(sector + 1) * SECTOR]
        if not any(data):
            kind, block, declared = 0, b"", SECTOR
        elif sector % 4 == 0:
            compressed = bz2.compress(data)
            assert compressed[:3] == b"BZh"
            kind, block = 3, b"\x00\x00\x00" + compressed[3:]
            declared = len(block)
        elif sector % 4 == 1:
            kind, block, declared = 1, data, SECTOR
        else:
            kind, block = 2, zlib.compress(data, 9)
            declared = len(block)
        assert declared < 1 << (width * 8 - 2)
        ranges.append((len(payload), len(block)))
        indexes.extend(((kind << (width * 8 - 2)) | declared).to_bytes(width, "little"))
        payload.extend(block)
    return xor_table(bytes(indexes)), bytes(payload), ranges


def isz_single(raw: bytes, width: int, length: int,
               implicit: bool = False) -> bytes:
    if implicit:
        return bytes(header(raw, length=length, width=0, count=0,
                            chunk_table=0, segment_table=0,
                            data_offset=length)) + raw
    table, payload, _ = encode(raw, width)
    return bytes(header(raw, length=length, width=width,
                        count=len(raw) // SECTOR, chunk_table=length,
                        segment_table=0, data_offset=length + len(table))
                 ) + table + payload


def isz_split(raw: bytes, width: int, length: int, *, part_style: bool,
              number_of_parts: int = 2) -> dict[str, bytes]:
    assert number_of_parts in (2, 3)
    table, payload, ranges = encode(raw, width)
    assert len(payload) > 256, "fixture needs nonzero payload"
    # Split *inside* stored/compressed blocks to exercise a single ReadAt
    # spanning two segment regions, not merely a boundary between chunks.
    nonzero = [(i, pos, count) for i, (pos, count) in enumerate(ranges)
               if count >= 16]
    assert len(nonzero) >= number_of_parts
    boundaries: list[tuple[int, int, int]] = []
    for fraction in range(1, number_of_parts):
        i, pos, size = nonzero[len(nonzero) * fraction // number_of_parts]
        consumed = max(1, size // 3)
        boundaries.append((pos + consumed, size - consumed, i))
    assert all(boundaries[i][0] < boundaries[i + 1][0]
               for i in range(len(boundaries) - 1))
    table_offset = length + (number_of_parts + 1) * 24
    first_data = table_offset + len(table)
    chunks = len(raw) // SECTOR

    segment_data = []
    previous = 0
    for end in [b[0] for b in boundaries] + [len(payload)]:
        segment_data.append(payload[previous:end])
        previous = end

    segment_headers = [
        header(raw, length=length, width=width, count=chunks,
               chunk_table=table_offset, segment_table=length,
               data_offset=first_data)]
    for i in range(1, number_of_parts):
        segment_headers.append(header(raw, length=64, width=width,
                                      count=chunks, chunk_table=0,
                                      segment_table=0, data_offset=64,
                                      part_number=i))

    entries = bytearray()
    for i in range(number_of_parts):
        prefix = first_data if i == 0 else 64
        declared_size = prefix + len(segment_data[i])
        started_here = 0 if i == 0 else boundaries[i - 1][2] + 1
        next_start = chunks if i == number_of_parts - 1 else boundaries[i][2] + 1
        logical_first = prefix if i == 0 else prefix + boundaries[i - 1][1]
        # The next segment's chunk_offset points after the previous
        # segment's spill-over tail; left_size identifies that tail.
        left = 0 if i == number_of_parts - 1 else boundaries[i][1]
        packed = struct.pack("<qiiii", declared_size, next_start - started_here,
                             started_here, logical_first, left)
        entries.extend(xor_table(packed))
    entries.extend(xor_table(bytes(24)))

    first = bytes(segment_headers[0]) + bytes(entries) + table + segment_data[0]
    assert len(first) == first_data + len(segment_data[0])
    if part_style:
        names = [f"disc.part{i + 1:02d}.isz" for i in range(number_of_parts)]
    else:
        names = ["disc.isz"] + [f"disc.i{i:02d}" for i in range(1, number_of_parts)]
    result = {names[0]: first}
    for i in range(1, number_of_parts):
        result[names[i]] = bytes(segment_headers[i]) + segment_data[i]
    return result


def inspect(reader: pathlib.Path, source: pathlib.Path, *,
            expected: str = "HELLO.TXT", success: bool = True,
            plugin: pathlib.Path | None = None,
            probe: pathlib.Path | None = None) -> None:
    destination = source.parent / ("extracted-" + source.stem.replace(".", "_"))
    command = [str(reader), str(source), "--extract", str(destination)]
    result = subprocess.run(command, capture_output=True, timeout=40)
    if not success:
        assert result.returncode != 0, (source, result.stdout, result.stderr)
        return
    assert result.returncode == 0, (source, result.stdout, result.stderr)
    assert (destination / expected).read_bytes() == b"world", source

    if plugin is not None:
        assert probe is not None
        output = source.parent / ("native-" + source.stem.replace(".", "_") + ".txt")
        test = subprocess.run([str(probe), str(plugin), str(source), str(output)],
                              capture_output=True, timeout=40)
        assert test.returncode == 0, (source, test.stdout, test.stderr)
        assert output.read_bytes() == b"world", source


def main(reader: pathlib.Path, plugin: pathlib.Path | None = None,
         probe: pathlib.Path | None = None) -> None:
    raw = bytes(test_images.iso_image())
    assert len(raw) >= SECTOR * 19
    with tempfile.TemporaryDirectory(prefix="iso-wcx-native-isz-split-") as td:
        root = pathlib.Path(td)
        for width in (2, 3, 4):
            for length in (48, 64):
                path = root / f"single-{width}-{length}.isz"
                path.write_bytes(isz_single(raw, width, length))
                inspect(reader, path, plugin=plugin, probe=probe)

        path = root / "implicit48.isz"
        path.write_bytes(isz_single(raw, 2, 48, implicit=True))
        inspect(reader, path, plugin=plugin, probe=probe)

        for part_style, number_of_parts in ((False, 2), (True, 2), (False, 3)):
            child = root / f"split-{part_style}-{number_of_parts}"
            child.mkdir()
            generated = isz_split(raw, 3, 64, part_style=part_style,
                                  number_of_parts=number_of_parts)
            for name, body in generated.items():
                (child / name).write_bytes(body)
            source = child / next(iter(generated))
            inspect(reader, source, plugin=plugin, probe=probe)

            companions = [child / x for x in list(generated)[1:]]
            last = companions[-1]
            good = last.read_bytes()
            last.unlink()
            inspect(reader, source, success=False)
            last.write_bytes(good)

            last.write_bytes(good[:64])
            inspect(reader, source, success=False)
            last.write_bytes(good)

            invalid_serial = bytearray(good)
            invalid_serial[6] ^= 0x40
            last.write_bytes(invalid_serial)
            inspect(reader, source, success=False)
            last.write_bytes(good)

            invalid_part = bytearray(good)
            invalid_part[34] = 0x7f
            last.write_bytes(invalid_part)
            inspect(reader, source, success=False)
            last.write_bytes(good)

        # A damaged pointer table is rejected rather than decoded with an
        # incorrect codec, and an unknown 5-byte pointer size is not guessed.
        invalid = bytearray(isz_single(raw, 3, 64))
        invalid[33] = 5
        source = root / "wrong-width.isz"
        source.write_bytes(invalid)
        inspect(reader, source, success=False)

        # UDF uses the identical native block layer, not an ISO-only path.
        udf_raw = bytes(test_images.udf_image())
        path = root / "udf-split"
        path.mkdir()
        files = isz_split(udf_raw, 3, 64, part_style=False)
        for name, content in files.items():
            (path / name).write_bytes(content)
        inspect(reader, path / "disc.isz", expected="hello")

    print("PASS: native ISZ single/split, 48/64 byte headers, indexed/implicit,"
          " 2/3/4 byte pointers, cross-part deflate/bzip2, corrupt/missing parts")


if __name__ == "__main__":
    args = [pathlib.Path(arg).resolve() for arg in sys.argv[1:]]
    assert len(args) in (1, 3)
    main(*args)
