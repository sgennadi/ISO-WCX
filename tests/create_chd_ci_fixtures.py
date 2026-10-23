#!/usr/bin/env python3
"""Create genuine MAME CHD v5 fixtures for architecture-matched WCX tests.

This program and chdman run ONLY in Linux CI; they are not included in
ISO-WCX.wcx/.wcx64 or the installable Total Commander ZIP.
"""
from __future__ import annotations

import pathlib
import shutil
import subprocess
import sys
import tempfile

from test_images import iso_image, raw_cd_image


def run(*args: str) -> None:
    completed = subprocess.run(
        list(args), capture_output=True, text=True, check=False, timeout=120
    )
    if completed.returncode:
        raise RuntimeError(
            f"MAME fixture generation failed: {args}\n"
            f"{completed.stdout[-1000:]}\n{completed.stderr[-1000:]}"
        )


def generate(destination: pathlib.Path) -> None:
    command = shutil.which("chdman")
    if not command:
        raise RuntimeError("chdman is required by CI fixture generation only")
    destination.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="iso-wcx-chd-wcx-fixtures-") as tmp:
        root = pathlib.Path(tmp)
        image = iso_image()
        assert len(image) > 4096 and len(image) % 2048 == 0
        iso_file = root / "source.iso"
        iso_file.write_bytes(image)
        dvd = destination / "dvd-lzma.chd"
        run(command, "createdvd", "-i", str(iso_file), "-o", str(dvd),
            "-c", "lzma", "-hs", "4096", "-np", "1")

        (root / "cd.bin").write_bytes(raw_cd_image(image, mode=1))
        cue = root / "cd.cue"
        cue.write_text(
            'FILE "cd.bin" BINARY\n'
            '  TRACK 01 MODE1/2352\n'
            '    INDEX 01 00:00:00\n',
            encoding="ascii",
        )
        cd = destination / "cd-cdlz.chd"
        run(command, "createcd", "-i", str(cue), "-o", str(cd),
            "-c", "cdlz", "-np", "1")

        # Avoid uploading accidentally huge or ungenerated fixtures.
        for fixture in (dvd, cd):
            assert fixture.is_file() and 124 < fixture.stat().st_size < 8 * 1024 * 1024
            assert fixture.read_bytes()[:8] == b"MComprHD"
            print(f"Generated {fixture.name}: {fixture.stat().st_size} bytes")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: create_chd_ci_fixtures.py DEST_DIRECTORY")
    generate(pathlib.Path(sys.argv[1]).resolve())
