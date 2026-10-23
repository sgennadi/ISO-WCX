#!/usr/bin/env python3
"""Interoperability tests against actual MAME chdman CHD v5 containers.

chdman is used *only* to generate external CI fixtures. It is not a WCX
runtime dependency and is never shipped in the Total Commander ZIP.
"""
from __future__ import annotations

import pathlib
import shutil
import subprocess
import sys
import tempfile

from test_images import iso_image, udf_image, raw_cd_image


def run(args: list[str], timeout: int = 120) -> str:
    result = subprocess.run(args, capture_output=True, text=True,
                            timeout=timeout, check=False)
    assert result.returncode == 0, (
        f"command {args!r} failed ({result.returncode}): "
        f"{result.stdout[-4000:]}\n{result.stderr[-4000:]}")
    return result.stdout


def verify(reader: pathlib.Path, chd: pathlib.Path,
           output: pathlib.Path, name: str, data: bytes) -> None:
    try:
        listing = run([str(reader), str(chd)])
    except AssertionError as failure:
        # In CI only, use chdman's independent decoder to show whether a
        # corrupted test fixture, CD frame offset or plugin mapping failed.
        # This is diagnostic and is never run in the shipped WCX.
        if "cd" in chd.name and shutil.which("chdman"):
            cue = output.parent / "diagnostic.cue"
            binary = output.parent / "diagnostic.bin"
            extracted = subprocess.run(
                ["chdman", "extractcd", "-i", str(chd),
                 "-o", str(cue), "-ob", str(binary)],
                capture_output=True, text=True, timeout=90, check=False)
            if extracted.returncode == 0 and binary.is_file():
                data = binary.read_bytes()[:2 * 1024 * 1024]
                marker = b"\x01CD001\x01"
                positions = []
                pos = 0
                while len(positions) < 8:
                    pos = data.find(marker,pos)
                    if pos == -1:
                        break
                    positions.append((pos,pos // 2352,pos % 2352))
                    pos += len(marker)
                raise AssertionError(
                    f"CD image failed native reader; independent chdman "
                    f"ISO PVD positions (byte,2352-frame,offset): {positions}; "
                    f"CUE: {cue.read_text(errors='replace')[:500]!r}"
                ) from failure
        raise

    assert name in listing, (chd, listing)
    run([str(reader), str(chd), "--extract", str(output)])
    assert (output / name).read_bytes() == data, chd


def negative(reader: pathlib.Path, file: pathlib.Path,
             mutated: bytes, directory: pathlib.Path) -> None:
    file.write_bytes(mutated)
    result = subprocess.run([str(reader), str(file), "--extract",
                             str(directory)], capture_output=True,
                            timeout=30, check=False)
    assert result.returncode != 0, file


def main(reader: pathlib.Path) -> None:
    chdman = shutil.which("chdman")
    if not chdman:
        raise RuntimeError("mame-tools/chdman required only for this CI test")
    with tempfile.TemporaryDirectory(prefix="iso-wcx-chdman-") as tmp:
        root = pathlib.Path(tmp)
        image = root / "dvd.iso"
        image.write_bytes(iso_image())
        assert image.stat().st_size % 2048 == 0
        for compression in ("zlib", "lzma"):
            output = root / ("dvd-" + compression + ".chd")
            run([chdman, "createdvd", "-i", str(image), "-o",
                 str(output), "-c", compression, "-hs", "4096",
                 "-np", "1"])
            verify(reader, output, root / ("out-dvd-" + compression),
                   "HELLO.TXT", b"world")
        udf = root / "udf.iso"
        udf.write_bytes(udf_image())
        output = root / "udf-lzma.chd"
        run([chdman, "createdvd", "-i", str(udf), "-o", str(output),
             "-c", "lzma", "-hs", "4096", "-np", "1"])
        verify(reader, output, root / "out-udf", "hello", b"world")

        # Raw Mode 1/2352 CD input, converted by chdman into 2448-byte
        # frames including subcode. Unlike Mode 1/2048 it directly models
        # physical sector data and avoids ambiguity in cooked CUE conversion.
        (root / "cd.bin").write_bytes(raw_cd_image(image.read_bytes(),mode=1))
        cue = root / "cd.cue"
        cue.write_text('FILE "cd.bin" BINARY\n'
                       '  TRACK 01 MODE1/2352\n'
                       '    INDEX 01 00:00:00\n', encoding="ascii")
        cd = root / "cd-lzma.chd"
        run([chdman, "createcd", "-i", str(cue), "-o", str(cd),
             "-c", "cdlz", "-np", "1"])
        verify(reader, cd, root / "out-cd", "HELLO.TXT", b"world")

        # Fail closed on malformed file signatures and unsupported parents.
        raw = bytearray(cd.read_bytes())
        negative(reader, root / "corrupt.chd", b"NotAChd!" + raw[8:],
                 root / "bad-sig")
        raw[104] = 0x42  # CHD v5 parentsha1 is nonzero.
        negative(reader, root / "parent.chd", bytes(raw),
                 root / "bad-parent")
        # These formerly reached libchdr before our format-specific bounds
        # checks; now they must be rejected by the preflight header parser.
        oversized = bytearray(cd.read_bytes())
        struct.pack_into(">I", oversized, 56, 0xffffffff)
        negative(reader, root / "huge-hunk.chd", bytes(oversized),
                 root / "bad-hunk")
        oversized = bytearray(cd.read_bytes())
        struct.pack_into(">Q", oversized, 40, 0x7fffffffffffffff)
        negative(reader, root / "huge-map.chd", bytes(oversized),
                 root / "bad-map")
        print("PASS: MAME chdman compressed CHD v5 DVD zlib/lzma, "
              "UDF lzma, CD cdlz Mode1 and malformed/parent rejection")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: test_chdman_interop.py path-to-disc_inspect")
    main(pathlib.Path(sys.argv[1]).resolve())
