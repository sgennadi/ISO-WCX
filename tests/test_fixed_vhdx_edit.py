#!/usr/bin/env python3
"""Independent transactional FAT16/32 writer inside fully mapped fixed VHDX.

The VHDX builder is independent of ISO-WCX and models dual headers,
CRC32C, metadata, fixed BAT and mapped payloads. This checks writes, full
logical extraction, header generation, immutable metadata, and rollback.
QEMU interoperability is a separate test-only CI job.
"""
from __future__ import annotations

import pathlib
import struct
import sys
import tempfile

from test_dynamic_vhd_edit import partitioned
from test_native_vhdx import (
    BAT, BLOCK, HEADER_A, HEADER_B, METADATA, MIB, make_image, seal, crc32c,
)
from test_floppy_edit import readback, run


def header(path: pathlib.Path, start: int) -> bytes:
    with path.open("rb") as file:
        file.seek(start)
        result = file.read(4096)
    assert len(result) == 4096 and result[:4] == b"head"
    expected = struct.unpack_from("<I", result, 4)[0]
    buf = bytearray(result)
    buf[4:8] = b"\0" * 4
    assert expected == crc32c(buf), "fixed VHDX header checksum invalid"
    return result


def assert_clean(root: pathlib.Path):
    assert not list(root.glob("*.iso-wcx-vhdx-*.img")), "temporary logical RAW leaked"
    assert not list(root.glob("*.iso-wcx-fat-*.tmp*")), "temporary VHDX clone leaked"


def must_reject(editor: pathlib.Path, path: pathlib.Path, payload: pathlib.Path,
                logical: str, root: pathlib.Path):
    original = path.read_bytes()
    run(editor, path, "add", logical, payload, ok=False)
    assert path.read_bytes() == original, "VHDX changed after a rejected write"
    assert_clean(root)


def test_regular(editor: pathlib.Path, reader: pathlib.Path,
                 root: pathlib.Path, kind: str):
    logical = partitioned(root, kind)
    original_raw = logical.read_bytes()
    image = root / f"{kind}-full-fixed.vhdx"
    image.write_bytes(make_image(original_raw, fixed=True, sector=512))
    before = image.read_bytes()
    old_a = header(image, HEADER_A)
    old_b = header(image, HEADER_B)
    assert old_a != old_b, "independent fixture requires different sequence headers"
    block0 = before[BAT:BAT + MIB]
    meta = before[METADATA:METADATA + MIB]

    payload = root / f"{kind}-payload.bin"
    payload.write_bytes((f"VHDX FAT {kind} Unicode ".encode() * 3000) + b"end")
    nested = "partition1/מחלקת קבצים"
    filename = "partition1/Тестовый документ שלום 📂.bin"
    run(editor, image, "mkdir", nested)
    run(editor, image, "add", nested + "/DATA.TXT", payload)
    run(editor, image, "add", filename, payload)
    readback(reader, image, {filename: payload.read_bytes(),
                             nested + "/DATA.TXT": payload.read_bytes()},
             root / f"extracted-{kind}")

    hdr_a = header(image, HEADER_A)
    hdr_b = header(image, HEADER_B)
    assert hdr_a == hdr_b, "VHDX header copies diverged after commit"
    assert struct.unpack_from("<Q", hdr_a, 8)[0] > max(
        struct.unpack_from("<Q", old_a, 8)[0],
        struct.unpack_from("<Q", old_b, 8)[0])
    assert hdr_a[16:32] != old_a[16:32], "FileWriteGuid not updated"
    assert hdr_a[32:48] != old_a[32:48], "DataWriteGuid not updated"
    assert hdr_a[48:64] == b"\0" * 16, "VHDX log unexpectedly dirty"
    after = image.read_bytes()
    assert len(after) == len(before), "fixed VHDX changed physical length"
    assert after[BAT:BAT + MIB] == block0, "fixed VHDX BAT was modified"
    assert after[METADATA:METADATA + MIB] == meta, "VHDX metadata modified"
    assert after[:HEADER_A] == before[:HEADER_A], "file identifier changed"
    assert after[HEADER_B + 4096:METADATA] == before[HEADER_B + 4096:METADATA], (
        "VHDX region/log/metadata padding unexpectedly changed")
    assert_clean(root)

    payload.write_bytes(b"replacement after VHDX edit")
    run(editor, image, "add", filename, payload)
    readback(reader, image, {filename: payload.read_bytes()},
             root / f"replaced-{kind}")
    run(editor, image, "delete", filename)
    run(editor, image, "delete", nested + "/DATA.TXT")
    run(editor, image, "delete", nested)
    assert filename not in run(reader, image).stdout.decode("utf-8")
    assert_clean(root)

    # Cancellation and invalid partition must never touch original physical bytes.
    before_failure = image.read_bytes()
    run(editor, image, "cancel", "partition1/CANCEL.TXT", payload, ok=False)
    run(editor, image, "add", "partition2/INVALID.TXT", payload, ok=False)
    run(editor, image, "add", "NO-PARTITION.TXT", payload, ok=False)
    assert image.read_bytes() == before_failure, "rollback failed"
    assert_clean(root)
    return original_raw


def test_refusals(editor: pathlib.Path, root: pathlib.Path, raw: bytes):
    src = bytearray(make_image(raw, fixed=True, sector=512))
    payload = root / "negative-content.txt"
    payload.write_bytes(b"negative case")
    logical = "partition1/UNSAFE.TXT"

    # Refuse a fixed-flag VHDX with one absent payload block, or an alias
    # into another fully-present physical BAT extent.
    missing = bytearray(src)
    struct.pack_into("<Q", missing, BAT + 8, 0)
    bad = root / "missing-payload.vhdx"
    bad.write_bytes(missing)
    must_reject(editor, bad, payload, logical, root)

    aliased = bytearray(src)
    struct.pack_into("<Q", aliased, BAT + 8, struct.unpack_from("<Q", src, BAT)[0])
    bad = root / "aliased-payload.vhdx"
    bad.write_bytes(aliased)
    must_reject(editor, bad, payload, logical, root)

    # One surviving header can be read, but strict writing needs both.
    one_bad = bytearray(src)
    one_bad[HEADER_A + 80] = 1
    bad = root / "single-good-header.vhdx"
    bad.write_bytes(one_bad)
    must_reject(editor, bad, payload, logical, root)

    # Fixed image must not declare unsupported logical 4K sectors.
    four_k = root / "four-k.vhdx"
    four_k.write_bytes(make_image(bytes(8 * MIB), fixed=True, sector=4096))
    must_reject(editor, four_k, payload, logical, root)

    # A dynamic sparse VHDX is not a fixed BAT image and must stay read-only.
    sparse = root / "dynamic-sparse.vhdx"
    sparse.write_bytes(make_image(raw, fixed=False, sector=512))
    must_reject(editor, sparse, payload, logical, root)

    # A dirty log or a parent descriptor must fail closed.
    dirty = bytearray(src)
    dirty[HEADER_A + 48:HEADER_A + 64] = b"L" * 16
    first = bytearray(dirty[HEADER_A:HEADER_A + 4096])
    seal(first)
    dirty[HEADER_A:HEADER_A + 4096] = first
    bad = root / "dirty.vhdx"
    bad.write_bytes(dirty)
    must_reject(editor, bad, payload, logical, root)


def main(editor: pathlib.Path, inspector: pathlib.Path):
    with tempfile.TemporaryDirectory(prefix="iso-wcx-fixed-vhdx-write-") as temp:
        root = pathlib.Path(temp)
        logical16 = test_regular(editor, inspector, root, "fat16")
        test_regular(editor, inspector, root, "fat32")
        test_refusals(editor, root, logical16)
    print("PASS: native fixed VHDX FAT16/32 staged Unicode edits, mirrored "
          "CRC headers, untouched BAT/metadata, rollback and strict refusals")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_fixed_vhdx_edit.py EDITOR INSPECTOR")
    main(*(pathlib.Path(value).resolve() for value in sys.argv[1:]))
