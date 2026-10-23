#!/usr/bin/env python3
"""End-to-end in-process archive test: FAT partitioned IMG/VHD/VHDX.

Runs disc_inspect linked to the SAME Disc/VirtualArchive code used in WCX,
rather than invoking independent virtual-fat or virtual-disk helpers.
"""
from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile

from test_native_vhdx import make_image
from test_virtual_fat import build_disk, make_fixed_vhd


def run(executable: pathlib.Path, *args: object, ok: bool = True) -> str:
    result = subprocess.run(
        [str(executable), *(str(arg) for arg in args)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=90, check=False)
    if (result.returncode == 0) != ok:
        raise AssertionError(
            f"disc_inspect({args!r}) code={result.returncode} "
            f"stderr={result.stderr[-600:]!r}")
    return result.stdout.decode("utf-8")


def verify(executable: pathlib.Path, image: pathlib.Path, contents: dict[str, bytes],
           directory: pathlib.Path) -> None:
    listing = run(executable, image)
    assert "partition1" in listing, listing
    for path in contents:
        assert "partition1/" + path in listing, (image, path, listing)
    run(executable, image, "--extract", directory)
    for path, expected in contents.items():
        target = directory / "partition1" / path
        assert target.read_bytes() == expected, (image, target)


def main(executable: pathlib.Path) -> None:
    with tempfile.TemporaryDirectory(prefix="iso-wcx-integrated-fat-") as directory:
        root = pathlib.Path(directory)
        for kind in ("fat12", "fat16", "fat32"):
            raw = root / (kind + ".img")
            expected = build_disk(raw, kind)
            verify(executable, raw, expected, root / ("extracted-" + kind))
            if kind == "fat16":
                vhd = root / "fixed.vhd"
                make_fixed_vhd(raw, vhd)
                verify(executable, vhd, expected, root / "extracted-vhd")
            if kind == "fat12":
                raw_bytes = raw.read_bytes()
                fixed = root / "fixed.vhdx"
                dynamic = root / "dynamic.vhdx"
                fixed.write_bytes(make_image(raw_bytes, fixed=True))
                dynamic.write_bytes(make_image(raw_bytes, fixed=False))
                verify(executable, fixed, expected, root / "extracted-vhdx")
                verify(executable, dynamic, expected, root / "extracted-dynamic-vhdx")

        # A broken FAT file chain must not replace an existing destination,
        # and must leave no .iso-wcx staging file after the failed extraction.
        bad = root / "bad-fat12.img"
        build_disk(bad, "fat12", chain_error=True)
        output = root / "rollback" / "partition1"
        output.mkdir(parents=True)
        target = output / "README.TXT"
        target.write_bytes(b"PREVIOUS-CONTENT")
        run(executable, bad, "--extract", root / "rollback", ok=False)
        assert target.read_bytes() == b"PREVIOUS-CONTENT"
        assert not list(root.rglob("*.iso-wcx-*.part"))
        print("PASS: WCX core reads FAT12/16/32 IMG, fixed VHD and "
              "fixed/dynamic VHDX; broken FAT extraction rolls back")


if __name__ == "__main__":
    main(pathlib.Path(sys.argv[1]).resolve())
