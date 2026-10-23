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

from test_images import iso_image, udf_image


def run(args: list[str], timeout: int = 120) -> str:
    result = subprocess.run(args, capture_output=True, text=True,
                            timeout=timeout, check=False)
    assert result.returncode == 0, (
        f"command {args!r} failed ({result.returncode}): "
        f"{result.stdout[-4000:]}\n{result.stderr[-4000:]}")
    return result.stdout


def verify(reader: pathlib.Path, chd: pathlib.Path,
           output: pathlib.Path, name: str, data: bytes) -> None:
    listing = run([str(reader), str(chd)])
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

        # A Mode 1/2048 CUE is normalized by chdman to full 2448-byte
        # CD frames with the track metadata libchdr supplies for our reader.
        (root / "cd.bin").write_bytes(image.read_bytes())
        cue = root / "cd.cue"
        cue.write_text('FILE "cd.bin" BINARY\n'
                       '  TRACK 01 MODE1/2048\n'
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
        print("PASS: MAME chdman compressed CHD v5 DVD zlib/lzma, "
              "UDF lzma, CD cdlz Mode1 and malformed/parent rejection")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: test_chdman_interop.py path-to-disc_inspect")
    main(pathlib.Path(sys.argv[1]).resolve())
